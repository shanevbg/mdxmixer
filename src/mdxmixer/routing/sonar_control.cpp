#include "routing/sonar_control.h"
#include "config/json_utils.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace mdxm {

namespace {

// Where GG publishes the addresses of its own servers. The transport below is
// ported from MDropDX12's mixer_sonar_http.cpp, which established the
// discovery path and the certificate handling.
const wchar_t* const kCoreProps =
    L"C:\\ProgramData\\SteelSeries\\SteelSeries Engine 3\\coreProps.json";

// Short on purpose. GG hangs regularly — that is the whole reason this file
// exists — and a wedged GG must not park the caller.
constexpr int kConnectTimeoutMs = 1500;
constexpr int kReceiveTimeoutMs = 4000;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// "key":"value" out of a flat document, without a parser. Only used on
// coreProps.json, whose shape is three fixed fields.
std::wstring FlatField(const std::wstring& text, const wchar_t* key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\"";
    size_t i = text.find(needle);
    if (i == std::wstring::npos) return std::wstring();
    i = text.find(L':', i + needle.size());
    if (i == std::wstring::npos) return std::wstring();
    const size_t open = text.find(L'"', i);
    if (open == std::wstring::npos) return std::wstring();
    const size_t close = text.find(L'"', open + 1);
    if (close == std::wstring::npos) return std::wstring();
    return text.substr(open + 1, close - open - 1);
}

// "127.0.0.1:6327" -> host and port.
bool SplitHostPort(const std::wstring& addr, std::wstring& host, INTERNET_PORT& port) {
    const size_t colon = addr.rfind(L':');
    if (colon == std::wstring::npos) return false;
    host = addr.substr(0, colon);
    port = (INTERNET_PORT)_wtoi(addr.substr(colon + 1).c_str());
    return port != 0 && !host.empty();
}

// One HTTPS request to the GG server. `status` receives the HTTP code even
// when the call "fails", because the code is the interesting part here: 400
// means the server validated and refused, 500 means it tried and could not.
bool Request(const wchar_t* verb, const std::wstring& host, INTERNET_PORT port,
             const wchar_t* path, const std::string& body,
             DWORD* status, std::wstring* reply, std::wstring* err) {
    auto fail = [err](const wchar_t* why) { if (err) *err = why; return false; };
    if (status) *status = 0;

    HINTERNET session = WinHttpOpen(L"mdxmixer/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return fail(L"WinHttpOpen failed");
    WinHttpSetTimeouts(session, kConnectTimeoutMs, kConnectTimeoutMs,
                       kReceiveTimeoutMs, kReceiveTimeoutMs);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) { WinHttpCloseHandle(session); return fail(L"GG is not answering"); }

    HINTERNET request = WinHttpOpenRequest(connect, verb, path, nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return fail(L"could not open the request");
    }

    // GG's certificate is self-signed for localhost. Relaxed for this one
    // loopback call and nowhere else.
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                          : L"Content-Type: application/json\r\n";
    bool sent = WinHttpSendRequest(request, headers, (DWORD)-1L,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                   (DWORD)body.size(), (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(request, nullptr);

    bool ok = false;
    if (sent) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (status) *status = code;
        std::string raw;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::vector<char> chunk(avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
            raw.append(chunk.data(), read);
        }
        if (reply) *reply = Widen(raw);
        ok = (code >= 200 && code < 300);
        if (!ok && err) {
            wchar_t buf[96];
            swprintf(buf, 96, L"GG answered HTTP %lu", code);
            *err = buf;
        }
    } else if (err) {
        *err = L"GG did not answer";
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// The GG server's address, from coreProps.json.
bool GgAddress(std::wstring& host, INTERNET_PORT& port, std::wstring* err) {
    std::ifstream f(kCoreProps, std::ios::binary);
    if (!f) {
        if (err) *err = L"SteelSeries GG is not installed";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::wstring text = Widen(ss.str());
    const std::wstring addr = FlatField(text, L"ggEncryptedAddress");
    if (addr.empty() || !SplitHostPort(addr, host, port)) {
        if (err) *err = L"coreProps.json has no ggEncryptedAddress";
        return false;
    }
    return true;
}

} // namespace

const wchar_t* SonarStateName(SonarState s) {
    switch (s) {
    case SonarState::Enabled:      return L"enabled";
    case SonarState::Disabled:     return L"disabled";
    case SonarState::NotInstalled: return L"not installed";
    default:                       return L"unknown";
    }
}

SonarState QuerySonar(std::wstring* err) {
    std::wstring host;
    INTERNET_PORT port = 0;
    if (!GgAddress(host, port, err))
        return (err && *err == L"SteelSeries GG is not installed") ? SonarState::NotInstalled
                                                                   : SonarState::Unknown;
    std::wstring reply;
    DWORD status = 0;
    if (!Request(L"GET", host, port, L"/subApps", std::string(), &status, &reply, err))
        return SonarState::Unknown;

    try {
        JsonValue root = JsonParse(reply);
        const JsonValue& sonar = root[L"subApps"][L"sonar"];
        if (!sonar.isObject()) {
            if (err) *err = L"GG did not report a sonar sub-app";
            return SonarState::Unknown;
        }
        return sonar[L"isEnabled"].asBool(false) ? SonarState::Enabled : SonarState::Disabled;
    } catch (...) {
        if (err) *err = L"could not read GG's reply";
        return SonarState::Unknown;
    }
}

bool SetSonarEnabled(bool enabled, std::wstring* err) {
    // Asking for the state it already holds is not a failure, and for
    // "enable" GG answers 500 to it rather than shrugging. Check first.
    std::wstring qerr;
    const SonarState now = QuerySonar(&qerr);
    if (now == SonarState::NotInstalled || now == SonarState::Unknown) {
        if (err) *err = qerr;
        return false;
    }
    if ((now == SonarState::Enabled) == enabled) return true;

    std::wstring host;
    INTERNET_PORT port = 0;
    if (!GgAddress(host, port, err)) return false;

    const std::string body = std::string("{\"name\":\"sonar\",\"isEnabled\":") +
                             (enabled ? "true" : "false") +
                             ",\"additionalArguments\":[]}";
    DWORD status = 0;
    if (!Request(L"POST", host, port, L"/subApps/status", body, &status, nullptr, err))
        return false;

    // GG answers before the sub-app has finished starting or stopping, so the
    // call is only believed once the state it claims is the state it reports.
    for (int i = 0; i < 40; ++i) {          // up to ~10 s
        Sleep(250);
        const SonarState s = QuerySonar(nullptr);
        if ((s == SonarState::Enabled) == enabled) return true;
    }
    if (err) *err = L"GG accepted it but Sonar did not reach that state in 10 s";
    return false;
}

} // namespace mdxm
