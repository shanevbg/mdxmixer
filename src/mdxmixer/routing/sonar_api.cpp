#include "routing/sonar_api.h"
#include "config/json_utils.h"
#include "app/log.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace mdxm {

namespace {

const wchar_t* const kCoreProps =
    L"C:\\ProgramData\\SteelSeries\\SteelSeries Engine 3\\coreProps.json";

// The channel keys Sonar uses, and the labels a person recognises.
struct ChannelDef { const wchar_t* key; const wchar_t* label; };

const ChannelDef kChannels[] = {
    { L"masters",     L"Master" },
    { L"game",        L"Game"   },
    { L"chatRender",  L"Chat"   },
    { L"chatCapture", L"Mic"    },
    { L"media",       L"Media"  },
    { L"aux",         L"Aux"    },
};

// 1 Hz for levels is the hard limit; Sonar is not a fast server and the
// faders are not worth more.
const unsigned kVolumeIntervalMs = 1000;

bool Elapsed(unsigned last, unsigned intervalMs) {
    return last == 0 || (GetTickCount() - last) >= intervalMs;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// Pulls "key":"value" out of flat JSON without a parser. Used only for the two
// bootstrap documents, whose shape is fixed and tiny; everything richer goes
// through json_utils below.
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

struct Url {
    bool https = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;
};

bool SplitUrl(const std::wstring& url, const std::wstring& path, Url& out) {
    const size_t i = url.find(L"://");
    if (i == std::wstring::npos) return false;
    out.https = url.compare(0, 5, L"https") == 0;
    const std::wstring rest = url.substr(i + 3);
    const size_t colon = rest.find(L':');
    if (colon == std::wstring::npos) return false;
    out.host = rest.substr(0, colon);
    out.port = (INTERNET_PORT)_wtoi(rest.substr(colon + 1).c_str());
    out.path = path;
    return out.port != 0 && !out.host.empty();
}

// One request. Returns false for a transport failure or any non-2xx, and sets
// *connectionFailed when nothing answered at all — the only case worth
// re-discovering for.
bool RawRequest(const wchar_t* verb, const Url& u, std::wstring* body,
                bool* connectionFailed) {
    if (connectionFailed) *connectionFailed = false;

    HINTERNET session = WinHttpOpen(L"mdxmixer/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (connectionFailed) *connectionFailed = true;
        return false;
    }
    WinHttpSetTimeouts(session, SonarHttp::kConnectTimeoutMs, SonarHttp::kConnectTimeoutMs,
                       SonarHttp::kReceiveTimeoutMs, SonarHttp::kReceiveTimeoutMs);

    HINTERNET connect = WinHttpConnect(session, u.host.c_str(), u.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        if (connectionFailed) *connectionFailed = true;
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(connect, verb, u.path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (connectionFailed) *connectionFailed = true;
        return false;
    }

    if (u.https) {
        // GG's certificate is self-signed for localhost. Relaxed for this one
        // loopback call and nowhere else — everything after discovery is plain
        // HTTP to 127.0.0.1.
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }

    bool ok = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        std::string raw;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::vector<char> chunk(avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
            raw.append(chunk.data(), read);
        }
        ok = (status >= 200 && status < 300);
        if (body) *body = Widen(raw);
    } else if (connectionFailed) {
        *connectionFailed = true;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

} // namespace

// ── Transport ────────────────────────────────────────────────────────────

bool SonarHttp::Discover() {
    m_base.clear();

    std::ifstream f(kCoreProps, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::wstring props = Widen(ss.str());

    const std::wstring gg = FlatField(props, L"ggEncryptedAddress");
    if (gg.empty()) return false;

    Url u;
    if (!SplitUrl(L"https://" + gg, L"/subApps", u)) return false;

    std::wstring body;
    if (!RawRequest(L"GET", u, &body, nullptr)) return false;

    // "webServerAddress" appears for several sub-apps, so take the one AFTER
    // the "sonar" key or a neighbouring sub-app's address gets picked up
    // instead.
    const size_t sonar = body.find(L"\"sonar\"");
    if (sonar == std::wstring::npos) return false;
    const std::wstring addr = FlatField(body.substr(sonar), L"webServerAddress");
    if (addr.empty()) return false;

    m_base = (addr.rfind(L"http", 0) == 0) ? addr : (L"http://" + addr);
    while (!m_base.empty() && m_base.back() == L'/') m_base.pop_back();
    return true;
}

bool SonarHttp::Request(const wchar_t* verb, const std::wstring& path,
                        std::wstring* body, bool allowRediscover) {
    if (!IsDiscovered() && !Discover()) return false;

    Url u;
    if (!SplitUrl(m_base, path, u)) return false;

    bool connectionFailed = false;
    if (RawRequest(verb, u, body, &connectionFailed)) return true;

    // Only a connection-level failure is worth retrying, and only once: a GG
    // that restarted onto a new port looks exactly like one that hung, and
    // retrying an HTTP status would just repeat a rejection.
    if (connectionFailed && allowRediscover && !m_pinned) {
        Forget();
        if (Discover()) return Request(verb, path, body, false);
    }
    return false;
}

bool SonarHttp::Get(const std::wstring& path, std::wstring& body) {
    body.clear();
    return Request(L"GET", path, &body, true);
}

bool SonarHttp::Put(const std::wstring& path) {
    return Request(L"PUT", path, nullptr, true);
}

// ── Channels ─────────────────────────────────────────────────────────────

std::wstring SonarKeyFromChannelId(const std::wstring& id) {
    const std::wstring prefix = L"sonar:";
    if (id.compare(0, prefix.size(), prefix) != 0) return std::wstring();
    return id.substr(prefix.size());
}

std::wstring SonarChannels::WritePath(const std::wstring& key, bool personal,
                                      const wchar_t* property,
                                      const std::wstring& value) const {
    return SonarWritePath(key, personal, m_streamMode, property, value);
}

std::wstring SonarWritePath(const std::wstring& key, bool personal,
                            bool streamMode, const wchar_t* property,
                            const std::wstring& value) {
    // Sonar's JSON and Sonar's write path disagree about ONE name, and the
    // disagreement is a single letter.
    //
    // The document has a top-level `masters` object sitting BESIDE `devices`:
    //
    //     { "masters": {...}, "devices": { game, chatRender, chatCapture,
    //                                      media, aux } }
    //
    // so `masters` is the right key to READ. It is the wrong one to WRITE:
    // the write path names a device, there is no device called `masters`, and
    // the request comes back 400 with the fader unmoved. That looks exactly
    // like Sonar refusing to let the master be driven from outside its own
    // app. It is not a refusal — it is a typo.
    //
    // Verified in mdx12 with a probe script:
    //     .../monitoring/masters/Volume/0.42  -> 400, value unchanged
    //     .../monitoring/master/Volume/0.42   -> 200, value moves
    // and the same for isMuted. Only streamer mode was exercised, because that
    // is the mode this machine runs; the classic path uses the same singular
    // on the same reasoning, unverified.
    const std::wstring seg = (key == L"masters") ? L"master" : key;

    if (streamMode) {
        // Sonar's own names for the two sliders. slider BEFORE channel — see
        // the note above.
        const std::wstring fader = personal ? L"monitoring" : L"streaming";
        return L"/volumeSettings/streamer/" + fader + L"/" + seg + L"/" +
               property + L"/" + value;
    }
    return L"/volumeSettings/classic/" + seg + L"/" + property + L"/" + value;
}

bool SonarChannels::Refresh(bool force) {
    if (!force && !Elapsed(m_lastTick, kVolumeIntervalMs)) return m_available;

    std::wstring body;
    if (!m_http.Get(L"/mode", body)) {
        m_available = false;
        return false;
    }
    // The document is a bare quoted string, "stream" or "classic".
    m_streamMode = body.find(L"classic") == std::wstring::npos;

    if (!m_http.Get(m_streamMode ? L"/volumeSettings/streamer"
                                 : L"/volumeSettings/classic", body)) {
        m_available = false;
        return false;
    }
    m_lastTick = GetTickCount();

    try {
        const JsonValue root = JsonParse(body);
        const JsonValue& devices = root[L"devices"];

        std::vector<SonarChannel> built;
        for (const ChannelDef& def : kChannels) {
            const bool isMaster = (wcscmp(def.key, L"masters") == 0);
            const JsonValue& node = isMaster ? root[L"masters"] : devices[def.key];
            if (node.isNull()) continue;

            SonarChannel c;
            c.key = def.key;
            c.label = def.label;

            if (m_streamMode) {
                const JsonValue& stream = node[L"stream"];
                const JsonValue& mon = stream[L"monitoring"];
                const JsonValue& str = stream[L"streaming"];
                if (mon.isNull() && str.isNull()) continue;
                c.personalVol   = mon[L"volume"].asFloat(1.0f);
                c.personalMute  = mon[L"muted"].asBool(false);
                c.streamingVol  = str[L"volume"].asFloat(1.0f);
                c.streamingMute = str[L"muted"].asBool(false);
            } else {
                // Classic mode has ONE slider. It is reported on both halves
                // so the surface does not have to grow a third shape; a write
                // to either moves the same thing, which is what classic mode
                // means.
                const JsonValue& classic = node[L"classic"];
                c.personalVol = c.streamingVol = classic[L"volume"].asFloat(1.0f);
                c.personalMute = c.streamingMute = classic[L"muted"].asBool(false);
            }

            // The master's mute is DESTRUCTIVE, so it is not offered.
            //
            // Measured in mdx12 once the write path was corrected: it writes
            // every render channel's isMuted — then, on unmute, puts back a
            // snapshot Sonar took at some earlier point rather than the states
            // that were live a moment before. With a deliberately mixed
            // pattern and three seconds of settling at each step, so it is not
            // a race:
            //
            //   mixed        aux=1 chatCapture=1 chatRender=0 game=1 media=0
            //   master=1     aux=1 chatCapture=1 chatRender=1 game=1 media=1
            //   master=0     aux=0 chatCapture=1 chatRender=0 game=0 media=0
            //
            // aux and game were muted going in and came back unmuted. Shane
            // hit this as "when I unmute I get all the channels unmuted". The
            // mic (chatCapture) is left alone throughout.
            //
            // Volume is NOT affected and stays available: it is a true
            // multiplier, and restoring the master restores every channel
            // exactly — 0.78 -> 0.273 at master 0.35 -> 0.78 again at 1.0.
            c.canMute = !isMaster;
            built.push_back(c);
        }

        m_channels.swap(built);
        m_available = !m_channels.empty();
    } catch (...) {
        // A parse failure is not a disconnection: keep the snapshot and say
        // nothing moved, rather than blanking every fader on one bad reply.
        Log(1, L"sonar: could not read the volume document");
        return false;
    }
    return m_available;
}

bool SonarChannels::SetVolume(const std::wstring& key, bool personal, float vol) {
    if (!m_available || key.empty()) return false;

    const float clamped = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
    wchar_t value[32];
    swprintf(value, 32, L"%.2f", clamped);

    if (!m_http.Put(WritePath(key, personal, L"Volume", value))) return false;

    // Optimistic, then corrected. The caller's next Refresh replaces this with
    // what Sonar actually holds; reading it back now costs a GET per write and
    // races the service, which has not necessarily applied the PUT yet.
    for (auto& c : m_channels)
        if (c.key == key) { (personal ? c.personalVol : c.streamingVol) = clamped; break; }
    return true;
}

bool SonarChannels::SetMute(const std::wstring& key, bool personal, bool mute) {
    if (!m_available || key.empty()) return false;

    // Refuse a channel that reports canMute=false rather than sending the
    // write. For the master that write LANDS, which is precisely the problem:
    // it mutes every render channel and loses their individual states on the
    // way back. See the measurements in Refresh().
    for (const auto& c : m_channels)
        if (c.key == key && !c.canMute) return false;

    // `isMuted`, camelCase. `Mute` returns 404, and it does not match
    // `Volume`'s capitalisation — the API is simply inconsistent here.
    if (!m_http.Put(WritePath(key, personal, L"isMuted", mute ? L"true" : L"false")))
        return false;

    for (auto& c : m_channels)
        if (c.key == key) { (personal ? c.personalMute : c.streamingMute) = mute; break; }
    return true;
}

} // namespace mdxm
