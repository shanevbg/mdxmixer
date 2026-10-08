#include "audiodg_watch.h"

#include <tlhelp32.h>

namespace mdxm {

// The one impure part, and it does nothing but look. By NAME from a toolhelp
// snapshot rather than by opening the process: AUDIODG runs as SYSTEM and
// cannot be opened from here, so there is nothing to query and no handle to
// hold -- which is also why the pid is the whole signal.
DWORD FindAudiodgPid() {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"audiodg.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

} // namespace mdxm
