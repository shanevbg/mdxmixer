#include "routing/reconcile.h"
#include <windows.h>

namespace mdxm {

std::vector<RouteFix> PlanRouteFixes(const std::vector<RouteIntent>& intents,
                                     const std::vector<SessionRoute>& sessions) {
    std::vector<RouteFix> out;
    for (const SessionRoute& s : sessions) {
        if (s.exePath.empty() || s.pid == 0) continue;
        const RouteIntent* want = nullptr;
        for (const RouteIntent& i : intents) {
            if (i.endpointId.empty()) continue;   // channel has nowhere to send it yet
            if (_wcsicmp(i.exePath.c_str(), s.exePath.c_str()) == 0) { want = &i; break; }
        }
        if (!want) continue;                      // not ours to move
        if (_wcsicmp(s.currentEndpointId.c_str(), want->endpointId.c_str()) == 0)
            continue;                             // already right
        out.push_back({ s.pid, want->endpointId, s.exePath, want->channelId });
    }
    return out;
}

} // namespace mdxm
