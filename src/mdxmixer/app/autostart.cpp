#include "autostart.h"
#include <windows.h>
#include <string>

namespace mdxm {

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"mdxmixer";
}

bool SetAutostart(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    bool ok;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(exe) + L"\"";
        ok = RegSetValueExW(key, kValueName, 0, REG_SZ, (const BYTE*)quoted.c_str(),
                            (DWORD)((quoted.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LONG rc = RegDeleteValueW(key, kValueName);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

bool GetAutostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    bool present = RegQueryValueExW(key, kValueName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

} // namespace mdxm
