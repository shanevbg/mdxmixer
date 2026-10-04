#include "device/device_order.h"
#include "device/device_identity.h"
#include <algorithm>
#include <map>
#include <string>

namespace mdxm {

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// One physical device on one side of the mixer. A headset publishes its stereo
// and hands-free endpoints under the same container, and those belong together;
// its microphone belongs in the input section, so the flow is part of the key.
std::wstring GroupKey(const DeviceLevel& d) {
    std::wstring side = d.isRender ? L"r:" : L"c:";
    return side + (IsNullContainer(d.containerId) ? L"id:" + d.id : L"ct:" + d.containerId);
}

struct Group {
    bool isDefault = false;
    bool present = false;
    std::wstring name;        // the stereo half's name, so the twin sorts with it
    bool nameIsHandsFree = true;
};

} // namespace

void SortDevices(std::vector<DeviceLevel>& devices) {
    // Collapse each physical device to one set of tier flags first, so the two
    // endpoints of a headset cannot land in different tiers.
    std::map<std::wstring, Group> groups;
    for (const DeviceLevel& d : devices) {
        Group& g = groups[GroupKey(d)];
        g.isDefault = g.isDefault || d.isDefault;
        // `active`, not `present`: present means PAIRED, which every dormant
        // headset is, so this tier sorted nothing.
        g.present = g.present || d.active;
        // The group is named after its stereo endpoint — "Headphones (XM5-2)"
        // rather than "Headset (XM5-2)" — so the pair files under the name the
        // user reads it by. Smallest name wins among equals, so the order does
        // not depend on enumeration order.
        std::wstring cand = Lower(d.displayName);
        bool better = g.name.empty() ||
                      (g.nameIsHandsFree && !d.isHandsFree) ||
                      (g.nameIsHandsFree == d.isHandsFree && cand < g.name);
        if (better) { g.name = cand; g.nameIsHandsFree = d.isHandsFree; }
    }

    std::stable_sort(devices.begin(), devices.end(),
                     [&](const DeviceLevel& a, const DeviceLevel& b) {
        const Group& ga = groups[GroupKey(a)];
        const Group& gb = groups[GroupKey(b)];
        // A device the user pinned, then one the failover list pins while it
        // is connected, then everything else.
        if (a.pinned != b.pinned) return a.pinned;            // 1
        if (a.autoPinned != b.autoPinned) return a.autoPinned;
        if (a.isRender != b.isRender) return a.isRender;      // 2
        if (ga.isDefault != gb.isDefault) return ga.isDefault; // 3
        if (ga.present != gb.present) return ga.present;      // 4
        if (ga.name != gb.name) return ga.name < gb.name;     // 5: groups by name
        if (a.isHandsFree != b.isHandsFree) return b.isHandsFree;  // stereo first
        std::wstring na = Lower(a.displayName), nb = Lower(b.displayName);
        if (na != nb) return na < nb;                         // 6
        return a.id < b.id;
    });
}

} // namespace mdxm
