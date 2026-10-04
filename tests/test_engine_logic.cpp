#include "test_framework.h"
#include "engine/engine.h"

using namespace mdxm;

MDXM_TEST_CASE(FallbackOutput_PicksBoundWhenPresent) {
    std::vector<EndpointInfo> eps = {
        {L"{hp}", L"Headphones", true, true}, {L"{spk}", L"Speakers", true, true} };
    CHECK(PickPersonalOutput(eps, {L"{hp}", L"Headphones"}, L"{spk}") == L"{hp}");
}

MDXM_TEST_CASE(FallbackOutput_PicksDefaultWhenBoundGone) {
    // Review Focus #5: headset disappears mid-stream -> default render device.
    std::vector<EndpointInfo> eps = { {L"{spk}", L"Speakers", true, true} };
    CHECK(PickPersonalOutput(eps, {L"{hp}", L"Headphones"}, L"{spk}") == L"{spk}");
}

MDXM_TEST_CASE(FallbackOutput_EmptyWhenNothingLeft) {
    std::vector<EndpointInfo> eps;
    CHECK(PickPersonalOutput(eps, {L"{hp}", L"Headphones"}, L"").empty());
}

MDXM_TEST_CASE(FallbackOutput_NameFallbackAfterRepair) {
    // Re-paired headset: new id, same name — must find it, not fall back.
    std::vector<EndpointInfo> eps = {
        {L"{hp-new}", L"WF-1000XM6", true, true}, {L"{spk}", L"Speakers", true, true} };
    CHECK(PickPersonalOutput(eps, {L"{hp-old}", L"WF-1000XM6"}, L"{spk}") == L"{hp-new}");
}

// The failover tests that lived here went with the FailoverDecider they
// tested: the engine now drives MDropDX12's ported FailoverWatcher, and
// tests/test_failover_watcher.cpp covers the same behaviours against it,
// plus the two protections the decider never had.

MDXM_TEST_CASE(Personal_FallbackRefusesToFeedItsOwnSource) {
    // The loop this prevents, measured on the target machine: the system
    // default is a Sonar channel, Sonar's output goes into a cable, and the
    // mixer captures that cable. Falling back to the default would close the
    // circle -- cable -> mixer -> Sonar -> cable -- at whatever level the
    // headset happens to be at. Silence is the better failure.
    std::vector<EndpointInfo> eps = {
        {L"{sonar-gaming}", L"SteelSeries Sonar - Gaming", true, true},
        {L"{cable-in}",     L"CABLE Input",                true, true},
    };
    const DeviceRef goneHeadset{ L"{headset}", L"Headphones (11- WF-1000XM5)" };
    std::vector<std::wstring> avoid = { L"{cable-in}", L"{sonar-gaming}" };

    CHECK(PickPersonalOutput(eps, goneHeadset, L"{sonar-gaming}", avoid).empty());
    // With nothing to avoid it would happily take the default, which is what
    // it used to do unconditionally.
    CHECK(PickPersonalOutput(eps, goneHeadset, L"{sonar-gaming}", {}) == L"{sonar-gaming}");
    // A real device is still chosen when one is offered.
    eps.push_back({L"{speakers}", L"Speakers", true, true});
    CHECK(PickPersonalOutput(eps, {L"{speakers}", L"Speakers"}, L"{sonar-gaming}", avoid)
          == L"{speakers}");
}
