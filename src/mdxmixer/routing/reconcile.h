#pragma once
// Making a stored app assignment true, rather than aspirational (fj#1).
//
// Windows' per-app routing API is keyed by PROCESS ID, so an assignment can
// only be written while the app is running. mdxmixer's config records the
// assignment by EXE PATH, which is the thing a person means. Everything
// between those two is here: given what is playing now and what the config
// says should be true, which pids need their route written.
//
// Pure, so the rules that matter — never touch a process nobody assigned,
// never write a route that is already right — are tested rather than observed
// in a log after the fact.
#include <string>
#include <vector>

namespace mdxm {

// "this exe belongs on this channel, which lives on this endpoint"
struct RouteIntent {
    std::wstring exePath;
    std::wstring channelId;
    std::wstring endpointId;   // the channel's routable render endpoint
};

// A process that is playing right now, and where Windows currently sends it.
// `currentEndpointId` is GetPersistedDefaultRender for the pid — empty when
// the process has no per-app route at all, which is the usual case.
struct SessionRoute {
    std::wstring exePath;
    unsigned long pid = 0;
    std::wstring currentEndpointId;
};

// One route to write.
struct RouteFix {
    unsigned long pid = 0;
    std::wstring endpointId;
    std::wstring exePath;
    std::wstring channelId;
};

// What needs writing to make the live state match the stored intent.
//
// Two rules, and both are about restraint:
//
//   * a session whose exe is in NO channel's app list is left alone. mdxmixer
//     did not route it, and taking it over because it happens to be playing
//     would make the mixer a thing that moves other people's audio around.
//   * a session already on the right endpoint produces nothing. The write
//     costs a COM round trip per process and the reconcile runs on session
//     arrival, so the common case has to be free.
//
// Matching is by exe path, case-insensitively: Windows is inconsistent about
// the case of a process image path and two spellings of one path are one app.
std::vector<RouteFix> PlanRouteFixes(const std::vector<RouteIntent>& intents,
                                     const std::vector<SessionRoute>& sessions);

} // namespace mdxm
