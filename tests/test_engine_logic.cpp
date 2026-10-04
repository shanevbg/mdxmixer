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

// ── The render watchdog's decision ───────────────────────────────────────
//
// The bug these exist for, in full, because it took a morning of silence to
// find and nothing in the program could see it:
//
// Coming out of Modern Standby on 2026-10-04 the Bluetooth headset's endpoint
// stayed listed, stayed DEVICE_STATE_ACTIVE, and went on answering its volume
// and its battery -- while its WASAPI render event simply stopped being
// signalled. render_stream.cpp waited INFINITE on that event, so no call ever
// returned an error, `invalidated` was never set, and all four places in
// engine.cpp that asked Invalidated() were blind by construction. MDXM_DIAG at
// the time:
//
//     MDXM_RING|id=sonar|depth=96000|drops=57654426|underruns=0
//
// The ring at its full capacity, 57.6 million frames dropped, and underruns=0
// -- the mix thread never once ran to find the ring empty, because MixPull is
// driven by the render callback. The capture end was a Sonar virtual endpoint
// which survived the resume and went on filling a ring nothing drained.
//
// So the watchdog's question is not "did anything report an error" but "are
// frames moving", and these cover the two ways that question is NOT enough on
// its own.

MDXM_TEST_CASE(Watchdog_RestartsOnlyWhenFramesStopWithARenderUp) {
    // The fault: a render is up, frames are not moving, and it has been long
    // enough that this is not a scheduling hiccup.
    CHECK(WatchdogShouldRestart(false, true, kWatchdogStuckMs));
    CHECK(WatchdogShouldRestart(false, true, kWatchdogStuckMs * 10));
}

MDXM_TEST_CASE(Watchdog_LeavesAWorkingStreamAlone) {
    // Frames moving is the whole answer, however long the last gap was.
    CHECK(!WatchdogShouldRestart(true, true, 0));
    CHECK(!WatchdogShouldRestart(true, true, kWatchdogStuckMs * 100));
}

MDXM_TEST_CASE(Watchdog_DoesNotRestartWhatIsNotRunning) {
    // No output device at all is a legitimate state: every headset switched
    // off and nothing else to play to. Frames are not moving and must not be
    // expected to. Restarting here would be a Start() against no endpoint,
    // once a second, forever.
    CHECK(!WatchdogShouldRestart(false, false, kWatchdogStuckMs * 10));
}

MDXM_TEST_CASE(Watchdog_WaitsOutABriefStall) {
    // A graph rebuild holds m_mutex, so the watchdog blocks on it rather than
    // racing it -- but a tick that lands either side of a Stop()/Start() pair
    // still sees no movement. Below the threshold it keeps its hands off.
    CHECK(!WatchdogShouldRestart(false, true, 0));
    CHECK(!WatchdogShouldRestart(false, true, kWatchdogStuckMs - 1));
}
