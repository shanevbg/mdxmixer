#include "test_framework.h"
#include "dsp/peak_hold.h"
#include "routing/sonar_api.h"
#include <vector>

using namespace mdxm;

// ── The hold itself ──────────────────────────────────────────────────────

MDXM_TEST_CASE(PeakHold_UnknownUntilSomethingIsPushed) {
    PeakHold h;
    h.Configure(1000);
    CHECK(!h.Known());
    CHECK_NEAR(h.Value(), kPeakUnknown, 1e-9);
    h.Push(0.0f, 10);
    // A pushed ZERO is a reading, and reads as one: silence is an answer.
    CHECK(h.Known());
    CHECK_NEAR(h.Value(), 0.0f, 1e-9);
}

MDXM_TEST_CASE(PeakHold_KeepsASpikeUpThenReleases) {
    PeakHold h;
    h.Configure(1000);                  // ticks, whatever the caller's unit is
    h.Push(0.8f, 100);
    CHECK_NEAR(h.Value(), 0.8f, 1e-6);
    // Nine further quiet blocks: 900 ticks, still inside the 1000-tick hold.
    for (int i = 0; i < 9; ++i) h.Push(0.0f, 100);
    CHECK_NEAR(h.Value(), 0.8f, 1e-6);
    // The tenth takes it past the window and the hold releases to what is
    // actually there. This is the behaviour the whole feature rests on: a
    // channel that blasted is still the highest row a moment later, and is not
    // still the highest row a minute later.
    h.Push(0.0f, 100);
    CHECK_NEAR(h.Value(), 0.0f, 1e-6);
}

MDXM_TEST_CASE(PeakHold_NewHighRestartsTheWindow) {
    PeakHold h;
    h.Configure(1000);
    h.Push(0.5f, 900);                  // 900 of the 1000 ticks used up
    h.Push(0.9f, 100);                  // a new high: the clock starts again
    CHECK_NEAR(h.Value(), 0.9f, 1e-6);
    for (int i = 0; i < 9; ++i) h.Push(0.0f, 100);
    CHECK_NEAR(h.Value(), 0.9f, 1e-6);  // would have expired without the reset
    h.Push(0.0f, 100);
    CHECK_NEAR(h.Value(), 0.0f, 1e-6);
}

MDXM_TEST_CASE(PeakHold_SteadySignalNeverExpires) {
    // Equality counts as a new high, so a signal sitting at a constant level
    // does not latch, expire, re-latch and flicker in a client's list.
    PeakHold h;
    h.Configure(1000);
    for (int i = 0; i < 100; ++i) h.Push(0.3f, 100);
    CHECK_NEAR(h.Value(), 0.3f, 1e-6);
}

MDXM_TEST_CASE(PeakHold_MarkUnknownForgetsRatherThanFreezes) {
    // The hazard this exists for: a headset switches off mid-track. Holding
    // the last value would point a client at a device that is not there.
    PeakHold h;
    h.Configure(1000);
    h.Push(0.7f, 100);
    h.MarkUnknown();
    CHECK(!h.Known());
    CHECK_NEAR(h.Value(), kPeakUnknown, 1e-9);
    // And it comes back as a reading, not as the old one.
    h.Push(0.1f, 100);
    CHECK_NEAR(h.Value(), 0.1f, 1e-6);
}

MDXM_TEST_CASE(PeakHold_NegativeInputIsClampedNotTreatedAsUnknown) {
    // kPeakUnknown travels in the same float as a real level, so a caller can
    // hand one straight back in by mistake. Pushing it must not quietly become
    // a held -1 that a client reads as "cannot know".
    PeakHold h;
    h.Configure(1000);
    h.Push(kPeakUnknown, 100);
    CHECK(h.Known());
    CHECK_NEAR(h.Value(), 0.0f, 1e-9);
}

MDXM_TEST_CASE(PeakHold_ZeroWindowIsInstantaneous) {
    PeakHold h;
    h.Configure(0);
    h.Push(0.9f, 100);
    CHECK_NEAR(h.Value(), 0.9f, 1e-6);
    h.Push(0.2f, 100);
    CHECK_NEAR(h.Value(), 0.2f, 1e-6);
}

MDXM_TEST_CASE(PeakHold_FramesForRateMatchesTheMilliseconds) {
    // One duration, two units, and they have to agree: the mix thread counts
    // frames while the endpoint sweep counts milliseconds, and a client is
    // told one number in docs/ipc.md.
    CHECK(PeakHoldFramesFor(48000) == 48000 * kPeakHoldMs / 1000);
    CHECK(PeakHoldFramesFor(44100) == 44100 * kPeakHoldMs / 1000);
    // 192000 * 1500 overflows a signed 32-bit intermediate; the uint64 cast in
    // PeakHoldFramesFor is what stops this being a negative hold window.
    CHECK(PeakHoldFramesFor(192000) == (size_t)192000 * kPeakHoldMs / 1000);
    CHECK(PeakHoldFramesFor(192000) > PeakHoldFramesFor(48000));
}

// ── Scanning a block ─────────────────────────────────────────────────────

MDXM_TEST_CASE(BlockPeak_TakesTheLargestMagnitudeAcrossBothChannels) {
    // Interleaved stereo, and the loudest sample is NEGATIVE and on the right.
    // Scanning only the left channel, or forgetting the absolute value, both
    // pass a naive test and fail this one.
    std::vector<float> buf = { 0.1f, -0.9f, 0.2f, 0.3f, -0.4f, 0.05f };
    CHECK_NEAR(BlockPeak(buf.data(), 3), 0.9f, 1e-6);
    std::vector<float> quiet(64, 0.0f);
    CHECK_NEAR(BlockPeak(quiet.data(), 32), 0.0f, 1e-9);
    CHECK_NEAR(BlockPeak(buf.data(), 0), 0.0f, 1e-9);
}

// ── Joining a peak onto a Sonar channel ──────────────────────────────────

static DeviceLevel Ep(const wchar_t* name, bool render, float peak) {
    DeviceLevel d;
    d.name = name;
    d.displayName = L"renamed by the user";
    d.isRender = render;
    d.peak = peak;
    return d;
}

// The real list from this machine, names and flows as Windows reports them.
static std::vector<DeviceLevel> SonarEndpoints() {
    return {
        Ep(L"SteelSeries Sonar - Aux (SteelSeries Sonar Virtual Audio Device)", true, 0.4457f),
        Ep(L"SteelSeries Sonar - Chat (SteelSeries Sonar Virtual Audio Device)", true, 0.0f),
        Ep(L"SteelSeries Sonar - Media (SteelSeries Sonar Virtual Audio Device)", true, 0.0f),
        Ep(L"SteelSeries Sonar - Gaming (SteelSeries Sonar Virtual Audio Device)", true, 0.0f),
        Ep(L"SteelSeries Sonar - Stream (SteelSeries Sonar Virtual Audio Device)", true, 0.3608f),
        // Both flows carry a device of this name. The render one comes FIRST in
        // the enumeration, which is what makes the flow test meaningful.
        Ep(L"SteelSeries Sonar - Microphone (SteelSeries Sonar Virtual Audio Device)", true, 0.77f),
        Ep(L"SteelSeries Sonar - Microphone (SteelSeries Sonar Virtual Audio Device)", false, 0.22f),
        Ep(L"Headphones (3- Razer BlackShark V2 Pro (BT))", true, 0.0027f),
    };
}

MDXM_TEST_CASE(SonarPeak_MapsEachKeyToItsOwnEndpoint) {
    const auto eps = SonarEndpoints();
    CHECK_NEAR(SonarChannelPeak(eps, L"aux"), 0.4457f, 1e-6);
    CHECK_NEAR(SonarChannelPeak(eps, L"media"), 0.0f, 1e-9);
    // "game" -> "Gaming", which is the pair that cannot be derived.
    CHECK_NEAR(SonarChannelPeak(eps, L"game"), 0.0f, 1e-9);
    CHECK_NEAR(SonarChannelPeak(eps, L"chatRender"), 0.0f, 1e-9);
}

MDXM_TEST_CASE(SonarPeak_ChatMicTakesTheCaptureSideNotTheRenderOne) {
    // Two devices, one name, different flows, render listed first. Taking
    // whichever matched first would give the mic channel the render device's
    // level -- right by luck until the enumeration order changes.
    const auto eps = SonarEndpoints();
    CHECK_NEAR(SonarChannelPeak(eps, L"chatCapture"), 0.22f, 1e-6);
}

MDXM_TEST_CASE(SonarPeak_MasterHasNoEndpointAndSaysSo) {
    // Not an oversight: the master is an output, and in streamer mode its two
    // halves land on two different devices, so one number cannot speak for it.
    const auto eps = SonarEndpoints();
    CHECK_NEAR(SonarChannelPeak(eps, L"masters"), kPeakUnknown, 1e-9);
    CHECK_NEAR(SonarChannelPeak(eps, L"notAChannel"), kPeakUnknown, 1e-9);
    CHECK_NEAR(SonarChannelPeak(eps, L""), kPeakUnknown, 1e-9);
}

MDXM_TEST_CASE(SonarPeak_UnknownWhenSonarIsNotInstalled) {
    // No Sonar endpoints at all, which is most machines. Nothing is invented.
    std::vector<DeviceLevel> eps = {
        Ep(L"Headphones (3- Razer BlackShark V2 Pro (BT))", true, 0.0027f),
        Ep(L"CABLE Input (VB-Audio Virtual Cable)", true, 0.5f),
    };
    CHECK_NEAR(SonarChannelPeak(eps, L"aux"), kPeakUnknown, 1e-9);
}

MDXM_TEST_CASE(SonarPeak_MatchesTheWindowsNameAndNotTheAlias) {
    // Every entry above carries displayName = "renamed by the user". Matching
    // on the alias would find nothing here and, worse, would start finding the
    // wrong device the moment someone named one of their own "SteelSeries
    // Sonar - Aux".
    const auto eps = SonarEndpoints();
    CHECK_NEAR(SonarChannelPeak(eps, L"aux"), 0.4457f, 1e-6);
}

MDXM_TEST_CASE(SonarPeak_CarriesAnUnreadableMeterThroughAsUnknown) {
    // An endpoint that is present but could not be metered must not become a
    // zero on the way through the join.
    std::vector<DeviceLevel> eps = {
        Ep(L"SteelSeries Sonar - Aux (SteelSeries Sonar Virtual Audio Device)",
           true, kPeakUnknown),
    };
    CHECK_NEAR(SonarChannelPeak(eps, L"aux"), kPeakUnknown, 1e-9);
}
