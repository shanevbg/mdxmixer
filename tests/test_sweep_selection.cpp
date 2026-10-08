// test_sweep_selection.cpp — skipping the endpoints nobody is looking at.
//
// The sweep that feeds the mixer cost 98-161 ms of every 250 ms tick on
// Shane's machine, on the UI thread, because it opened a property store and
// activated two COM interfaces for each of 53 endpoints four times a second.
// Caching the interfaces is the main cure; this is the other one he asked for:
// "If they can't see a device or it's disabled can you skip that somehow".
#include "test_framework.h"
#include "device/sweep_selection.h"

using namespace mdxm;

namespace {

DeviceLevel Dev(const wchar_t* id, const wchar_t* name, bool hidden) {
    DeviceLevel d;
    d.id = id;
    d.name = name;
    d.hidden = hidden;
    d.isRender = true;
    return d;
}

bool Has(const std::vector<std::wstring>& v, const wchar_t* id) {
    for (const auto& s : v) if (s == id) return true;
    return false;
}

const std::vector<std::wstring> kSonar{ L"SteelSeries Sonar - " };

} // namespace

MDXM_TEST_CASE(IdentityOnly_SkipsAHiddenDevice) {
    const std::vector<DeviceLevel> levels{
        Dev(L"{vbm1}", L"VBMatrix In 1 (VB-Audio Matrix VAIO)", true),
        Dev(L"{head}", L"Headphones (12- WF-1000XM5)", false),
    };
    const auto skip = IdentityOnlyIds(levels, {}, kSonar);
    CHECK(skip.size() == 1);
    CHECK(Has(skip, L"{vbm1}"));
}

MDXM_TEST_CASE(IdentityOnly_NeverSkipsAVisibleDevice) {
    const std::vector<DeviceLevel> levels{
        Dev(L"{head}", L"Headphones (12- WF-1000XM5)", false),
        Dev(L"{cable}", L"CABLE Input (VB-Audio Virtual Cable)", false),
    };
    CHECK(IdentityOnlyIds(levels, {}, kSonar).empty());
}

// A hide in the device list must not break a meter in the mixer list. The Aux
// channel's meter is joined onto this endpoint by name, so its peak is being
// read even when its own row is not drawn.
MDXM_TEST_CASE(IdentityOnly_KeepsMeteringHiddenSonarEndpoints) {
    const std::vector<DeviceLevel> levels{
        Dev(L"{aux}", L"SteelSeries Sonar - Aux (SteelSeries Sonar Virtual Audio Device)", true),
        Dev(L"{vbm1}", L"VBMatrix In 1 (VB-Audio Matrix VAIO)", true),
    };
    const auto skip = IdentityOnlyIds(levels, {}, kSonar);
    CHECK(!Has(skip, L"{aux}"));
    CHECK(Has(skip, L"{vbm1}"));
}

// A hidden row is still a route. Its level is the answer to "is this thing
// even getting audio", which is the question the hidden row was never meant to
// stop anyone asking.
MDXM_TEST_CASE(IdentityOnly_KeepsTheEndpointsTheEngineIsUsing) {
    const std::vector<DeviceLevel> levels{
        Dev(L"{cableOut}", L"CABLE Output (VB-Audio Virtual Cable)", true),
        Dev(L"{vbm1}", L"VBMatrix In 1 (VB-Audio Matrix VAIO)", true),
    };
    const auto skip = IdentityOnlyIds(levels, { L"{cableOut}" }, kSonar);
    CHECK(!Has(skip, L"{cableOut}"));
    CHECK(Has(skip, L"{vbm1}"));
}

// An endpoint with no id cannot be addressed, so it cannot be skipped by id
// either. Dropping it into the skip list as an empty string would skip the
// FIRST endpoint of every later sweep, which is the kind of bug that looks
// like a flickering meter.
MDXM_TEST_CASE(IdentityOnly_IgnoresAnEndpointWithNoId) {
    std::vector<DeviceLevel> levels{ Dev(L"", L"Nameless", true) };
    CHECK(IdentityOnlyIds(levels, {}, kSonar).empty());
}

// The real shape of this machine: 8 VBMatrix In rows hidden, Sonar's six
// visible, one headset live. The sweep should end up asking about 7 endpoints
// instead of 15.
MDXM_TEST_CASE(IdentityOnly_HandlesTheMachineThisWasWrittenFor) {
    std::vector<DeviceLevel> levels;
    for (int i = 1; i <= 8; ++i) {
        wchar_t id[32], name[64];
        swprintf(id, 32, L"{vbm%d}", i);
        swprintf(name, 64, L"VBMatrix In %d (VB-Audio Matrix VAIO)", i);
        levels.push_back(Dev(id, name, true));
    }
    levels.push_back(Dev(L"{aux}", L"SteelSeries Sonar - Aux (SteelSeries Sonar Virtual Audio Device)", false));
    levels.push_back(Dev(L"{head}", L"Headphones (12- WF-1000XM5)", false));
    CHECK(IdentityOnlyIds(levels, {}, kSonar).size() == 8);
}
