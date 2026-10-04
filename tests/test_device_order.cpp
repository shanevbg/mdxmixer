#include "test_framework.h"
#include "device/device_order.h"

using namespace mdxm;

namespace {
DeviceLevel Dev(const wchar_t* id, const wchar_t* name, bool render = true) {
    DeviceLevel d;
    d.id = id;
    d.name = name;
    d.displayName = name;
    d.isRender = render;
    return d;
}
std::wstring Names(const std::vector<DeviceLevel>& v) {
    std::wstring out;
    for (const auto& d : v) { out += d.displayName; out += L";"; }
    return out;
}
} // namespace

MDXM_TEST_CASE(Order_PinnedFirstThenDefaultThenConnected) {
    std::vector<DeviceLevel> v = {
        Dev(L"{a}", L"Zebra speakers"),
        Dev(L"{b}", L"Alpha speakers"),
        Dev(L"{c}", L"Default speakers"),
        Dev(L"{d}", L"Pinned buds"),
    };
    v[0].active = true;
    // `active`, not `present`. present means PAIRED and is true of a headset
    // switched off in a drawer, so sorting on it sorted nothing — which is how
    // six dormant pairings came to sit at the top of the mixer.
    v[1].active = false;               // absent: below the connected ones
    v[2].isDefault = true;
    v[3].pinned = true;
    SortDevices(v);
    CHECK(Names(v) == L"Pinned buds;Default speakers;Zebra speakers;Alpha speakers;");
}

// A device the user pinned outranks one the failover allowlist pins, and both
// outrank the system default.
MDXM_TEST_CASE(Order_FailoverPinsSitBelowUserPinsAndAboveTheDefault) {
    std::vector<DeviceLevel> v = {
        Dev(L"{a}", L"Default speakers"),
        Dev(L"{b}", L"Failover buds"),
        Dev(L"{c}", L"User pinned buds"),
    };
    v[0].isDefault = true;
    v[1].autoPinned = true;
    v[2].pinned = true;
    SortDevices(v);
    CHECK(Names(v) == L"User pinned buds;Failover buds;Default speakers;");
}

// A paired-but-switched-off device on the allowlist must NOT be held at the
// top: that is the whole of Shane's "I see a lot of disconnected devices at
// the top of the list". The caller is what refuses to set autoPinned on an
// inactive device, so this pins the contract the sort relies on.
MDXM_TEST_CASE(Order_InactiveDeviceSinksEvenWhenPresent) {
    std::vector<DeviceLevel> v = {
        Dev(L"{a}", L"Live speakers"),
        Dev(L"{b}", L"Dormant buds"),
    };
    v[1].present = true;               // paired...
    v[1].active = false;               // ...but not connected
    SortDevices(v);
    CHECK(Names(v) == L"Live speakers;Dormant buds;");
}

MDXM_TEST_CASE(Order_OutputsBeforeInputs) {
    std::vector<DeviceLevel> v = {
        Dev(L"{mic}", L"Microphone", false),
        Dev(L"{spk}", L"Speakers", true),
    };
    SortDevices(v);
    CHECK(v[0].isRender);
    CHECK(!v[1].isRender);
}

MDXM_TEST_CASE(Order_HandsFreeTwinStaysUnderItsStereoHalf) {
    // The whole point of the container key: the default output ranks the pair
    // to the top, and its hands-free twin — which is NOT the default and would
    // otherwise be flung to the bottom — comes with it, directly underneath.
    std::vector<DeviceLevel> v = {
        Dev(L"{other}", L"Speakers (Realtek)"),
        Dev(L"{hfp}", L"Headset (XM5-2)"),
        Dev(L"{a2dp}", L"Headphones (XM5-2)"),
    };
    v[1].containerId = L"{ct-2}";
    v[1].isHandsFree = true;
    v[2].containerId = L"{ct-2}";
    v[2].isDefault = true;
    SortDevices(v);
    CHECK(Names(v) == L"Headphones (XM5-2);Headset (XM5-2);Speakers (Realtek);");
}

MDXM_TEST_CASE(Order_IsStableWhateverWindowsEnumeratedFirst) {
    // Windows reshuffles its enumeration whenever something reconnects. Two
    // opposite input orders must produce the same list, or the five XM5s move
    // under the cursor.
    std::vector<DeviceLevel> a = {
        Dev(L"{1}", L"WF-1000XM5-1"), Dev(L"{2}", L"WF-1000XM5-2"),
        Dev(L"{3}", L"WF-1000XM5-3"), Dev(L"{4}", L"WF-1000XM5-4"),
    };
    std::vector<DeviceLevel> b(a.rbegin(), a.rend());
    SortDevices(a);
    SortDevices(b);
    CHECK(Names(a) == Names(b));
    CHECK(Names(a) == L"WF-1000XM5-1;WF-1000XM5-2;WF-1000XM5-3;WF-1000XM5-4;");
}

MDXM_TEST_CASE(Order_SortsByOurNameNotTheWindowsOne) {
    // Renaming is the point of the alias, so the list has to file the device
    // where the user's own name puts it.
    std::vector<DeviceLevel> v = {
        Dev(L"{1}", L"WF-1000XM5-1"),
        Dev(L"{2}", L"WF-1000XM5-2"),
    };
    v[1].displayName = L"Aqua buds";
    SortDevices(v);
    CHECK(Names(v) == L"Aqua buds;WF-1000XM5-1;");
}

MDXM_TEST_CASE(Order_PlaceholderContainerDoesNotClumpUnrelatedDevices) {
    // The bug this rule was written for: six Sonar endpoints share the
    // placeholder ContainerId, so grouping on it put the whole clump in the
    // default device's tier and left them in raw enumeration order.
    const wchar_t* kNull = L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";
    std::vector<DeviceLevel> v = {
        Dev(L"{s-aux}", L"Sonar - Aux"),
        Dev(L"{s-gaming}", L"Sonar - Gaming"),
        Dev(L"{hp}", L"Headphones (XM6)"),
    };
    for (auto& d : v) d.containerId = kNull;
    v[2].containerId = L"{ct-xm6}";
    v[2].isDefault = true;            // the real default, and the only one
    SortDevices(v);
    CHECK(Names(v) == L"Headphones (XM6);Sonar - Aux;Sonar - Gaming;");
}
