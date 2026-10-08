#pragma once
// Which endpoints the sweep still has to ASK about, and which it can list from
// its own records.
//
// "If they can't see a device or it's disabled can you skip that somehow" --
// Shane, 2026-10-05, and the answer has two halves. Disabled was already
// handled: ListEndpointVolumes enumerates ACTIVE and UNPLUGGED only, and never
// activates anything on an endpoint that is not ACTIVE. Hidden was not, and on
// this machine hidden is the bigger half -- installing Voicemeeter and
// VB-Matrix took the endpoint count to 53, and sixteen of those are VBMatrix
// In/Out rows that exist only because the driver makes eight of each.
//
// An endpoint the user has hidden has no row to draw, so its volume and its
// meter are being collected for nobody. It still needs its IDENTITY: a device
// missing from the list can be neither renamed nor un-hidden.
//
// WHAT MUST NOT BE SKIPPED, even when hidden, is anything whose reading
// something else consumes. Two of those exist and both are easy to overlook:
//
//   * the Sonar endpoints. A Sonar CHANNEL's meter is joined onto its endpoint
//     by name (SonarChannelPeak), so hiding "SteelSeries Sonar - Aux" in the
//     device list would silently kill the Aux channel's meter in the mixer --
//     a hide in one list breaking a meter in another, which is exactly the
//     kind of fault nobody connects back to its cause.
//   * whatever the engine is playing to or capturing from. Its level is the
//     answer to "is this thing even getting audio", and a hidden row is still
//     a route.
#include "device/endpoint_volume.h"
#include <string>
#include <vector>

namespace mdxm {

// The endpoints a sweep may collect identity alone for: hidden, and nothing
// else reading them.
//
// `keepIds` are endpoints in use by the engine; `keepNamePrefixes` are the
// Windows-name prefixes whose meters another surface joins onto (Sonar's).
// Both are passed in rather than known here, so this stays a pure decision
// that a test can state its own cases for.
inline std::vector<std::wstring> IdentityOnlyIds(
        const std::vector<DeviceLevel>& levels,
        const std::vector<std::wstring>& keepIds,
        const std::vector<std::wstring>& keepNamePrefixes) {
    std::vector<std::wstring> out;
    for (const auto& d : levels) {
        if (d.id.empty() || !d.hidden) continue;
        bool keep = false;
        for (const auto& k : keepIds)
            if (!k.empty() && k == d.id) { keep = true; break; }
        if (!keep)
            for (const auto& p : keepNamePrefixes)
                if (!p.empty() && d.name.rfind(p, 0) == 0) { keep = true; break; }
        if (!keep) out.push_back(d.id);
    }
    return out;
}

} // namespace mdxm
