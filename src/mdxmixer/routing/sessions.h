#pragma once
// What is actually playing, and where: IAudioSessionManager2 swept across all
// active render endpoints (spec routing/ component). The routing tab and the
// exe-path -> pid mapping for per-app assignment both come from here.
#include <windows.h>
#include <string>
#include <vector>

namespace mdxm {

struct SessionInfo {
    std::wstring exePath;      // full image path (empty for system sessions, which are skipped)
    DWORD pid = 0;
    std::wstring endpointId;   // render endpoint the session lives on
    std::wstring endpointName;
    bool active = false;       // AudioSessionStateActive
};

// COM failures skip that endpoint/session; never throws. Self-initializes COM.
std::vector<SessionInfo> EnumerateSessions();

} // namespace mdxm
