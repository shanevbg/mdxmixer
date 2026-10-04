#include "test_framework.h"
#include "ui/battery_overlay.h"

using namespace mdxm;

namespace {
DeviceLevel Dev(const wchar_t* id, const wchar_t* name, const wchar_t* container,
                int battery, bool active = true) {
    DeviceLevel d;
    d.id = id;
    d.name = name;
    d.displayName = name;
    d.containerId = container;
    d.battery = battery;
    d.active = active;
    return d;
}
} // namespace

MDXM_TEST_CASE(Overlay_TokensAreSubstituted) {
    CHECK(FormatBatteryLine(L"Batt: $b%", L"XM5 White", L"Headphones (9- WF-1000XM5)", 62) ==
          L"Batt: 62%");
    CHECK(FormatBatteryLine(L"$sn $b%", L"XM5 White", L"Headphones (9- WF-1000XM5)", 62) ==
          L"XM5 White 62%");
    CHECK(FormatBatteryLine(L"$n", L"XM5 White", L"Headphones (9- WF-1000XM5)", 62) ==
          L"Headphones (9- WF-1000XM5)");
}

MDXM_TEST_CASE(Overlay_LongestTokenWins) {
    // "$sn" must not be read as "$s" followed by "n", nor as "$n" preceded by
    // a stray "$s" — both would put the wrong text on screen rather than
    // failing visibly.
    CHECK(FormatBatteryLine(L"$sn", L"ALIAS", L"WINDOWS", 50) == L"ALIAS");
}

MDXM_TEST_CASE(Overlay_UnknownTokenIsPrintedAsTyped) {
    // Someone writing "$5" means "$5". Eating it would be worse than showing
    // it, because nothing on screen would say what happened.
    CHECK(FormatBatteryLine(L"$5 $z", L"A", L"B", 50) == L"$5 $z");
    CHECK(FormatBatteryLine(L"cost $$5", L"A", L"B", 50) == L"cost $5");
    // A trailing dollar must not read past the end of the string.
    CHECK(FormatBatteryLine(L"ends with $", L"A", L"B", 50) == L"ends with $");
}

MDXM_TEST_CASE(Overlay_NoBatteryReadsAsDashesNotMinusOne) {
    CHECK(FormatBatteryLine(L"$b%", L"A", L"B", -1) == L"--%");
}

MDXM_TEST_CASE(Overlay_OneLinePerPhysicalDevice) {
    // A headset publishes a stereo endpoint and a hands-free one under the
    // same ContainerId, carrying the same battery. Without the de-duplication
    // every headset is listed twice, which is exactly the case this overlay
    // exists to make legible — two LINES must mean two HEADSETS.
    std::vector<DeviceLevel> v = {
        Dev(L"{a}", L"Headphones (XM5)", L"{ctr-1}", 62),
        Dev(L"{b}", L"Headset (XM5)",    L"{ctr-1}", 62),
        Dev(L"{c}", L"Headphones (XM6)", L"{ctr-2}", 18),
    };
    auto picked = BatteryDevices(v);
    CHECK(picked.size() == 2);
    if (picked.size() == 2) {
        CHECK(picked[0]->id == L"{a}");   // the stereo half, listed first
        CHECK(picked[1]->id == L"{c}");
    }
}

MDXM_TEST_CASE(Overlay_SkipsDisconnectedAndBatterylessDevices) {
    std::vector<DeviceLevel> v = {
        Dev(L"{a}", L"Speakers", L"{ctr-1}", -1),          // no battery
        Dev(L"{b}", L"Dormant buds", L"{ctr-2}", 44, false), // paired, not connected
        Dev(L"{c}", L"Live buds", L"{ctr-3}", 70),
    };
    auto picked = BatteryDevices(v);
    CHECK(picked.size() == 1);
    if (picked.size() == 1) CHECK(picked[0]->id == L"{c}");
}

MDXM_TEST_CASE(Overlay_NullContainerDevicesAreNotCollapsed) {
    // Windows hands every device with no real container the same placeholder
    // GUID. Treating that as an anchor would merge unrelated devices into one
    // line — the same trap that once collapsed six Sonar endpoints into one.
    const wchar_t* kNull = L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";
    std::vector<DeviceLevel> v = {
        Dev(L"{a}", L"Thing one", kNull, 30),
        Dev(L"{b}", L"Thing two", kNull, 90),
    };
    CHECK(BatteryDevices(v).size() == 2);
}

MDXM_TEST_CASE(Overlay_ColorsMatchMdx12Thresholds) {
    // MDropDX12's renderer: green above half, amber down to a quarter, red at
    // a quarter and below. The boundaries are exclusive on the way down —
    // exactly 50 is amber and exactly 25 is red — and that is deliberate, so
    // the ported thresholds are pinned rather than re-guessed.
    CHECK(BatteryColor(100) == RGB(0x3C, 0xE0, 0x7A));
    CHECK(BatteryColor(51)  == RGB(0x3C, 0xE0, 0x7A));
    CHECK(BatteryColor(50)  == RGB(0xFF, 0xC8, 0x3C));
    CHECK(BatteryColor(26)  == RGB(0xFF, 0xC8, 0x3C));
    CHECK(BatteryColor(25)  == RGB(0xFF, 0x50, 0x50));
    CHECK(BatteryColor(0)   == RGB(0xFF, 0x50, 0x50));
    // Unknown is not empty: -1 means "not reported", and alarm red would say
    // something the data does not.
    CHECK(BatteryColor(-1)  == RGB(0xFF, 0xC8, 0x3C));
}

MDXM_TEST_CASE(Overlay_SnapHandlesNegativeMonitorOrigins) {
    // A monitor left of and above the primary. Shane's are at coordinates
    // like -2700, which is what makes typing them unreasonable and the
    // arithmetic worth testing: a sign error here lands the overlay on the
    // wrong screen, or off every screen.
    const RECT mon = { -2560, -300, 0, 1140 };   // 2560 x 1440
    const int w = 120, h = 30, inset = 12;

    POINT tl = SnapToCorner(mon, w, h, 0);
    CHECK(tl.x == -2560 + inset);
    CHECK(tl.y == -300 + inset);

    POINT tr = SnapToCorner(mon, w, h, 1);
    CHECK(tr.x == 0 - w - inset);
    CHECK(tr.y == -300 + inset);

    POINT bl = SnapToCorner(mon, w, h, 2);
    CHECK(bl.x == -2560 + inset);
    CHECK(bl.y == 1140 - h - inset);

    POINT br = SnapToCorner(mon, w, h, 3);
    CHECK(br.x == 0 - w - inset);
    CHECK(br.y == 1140 - h - inset);
}

MDXM_TEST_CASE(Overlay_SnapStaysInsideAPrimaryAtTheOrigin) {
    const RECT mon = { 0, 0, 1920, 1080 };
    POINT br = SnapToCorner(mon, 200, 40, 3);
    CHECK(br.x == 1920 - 200 - 12);
    CHECK(br.y == 1080 - 40 - 12);
    // Inside the monitor on every edge, which is the property that actually
    // matters — a corner must never put it off the screen it names.
    CHECK(br.x > mon.left && br.x + 200 <= mon.right);
    CHECK(br.y > mon.top && br.y + 40 <= mon.bottom);
}
