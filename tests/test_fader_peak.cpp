// test_fader_peak.cpp — what ONE fader is passing, from the channel's source
// peak and that fader's own gain.
//
// A channel carries ONE peak and it is measured before both gain stages: that
// is the right number for "which channel is making sound", and it is what goes
// over the wire (docs/ipc.md §2.1). It is the WRONG number to draw beside a
// fader, which is what this exists to fix.
//
// Shane caught it on the Sonar channels, measured on 2026-10-04: sonar:aux
// with its Personal fader at zero and its Streaming fader at 100 reported
// peak=0.715927 -- the signal arriving from the apps -- and the Mixer tab drew
// that as a green bar on the [P] row of a fader passing nothing. His words:
// "sonar aux [p] is set to zero but peak meter shows it green", and "the peak
// meters you get for the sonar channels are the streaming channels, not the
// personal channels". Both readings follow from the same mistake.
#include "test_framework.h"
#include "ipc/mixer_control.h"

using namespace mdxm;

namespace {
ChannelState Chan(float peak, float pvol, bool pmute, float svol, bool smute) {
    ChannelState c{ L"aux", L"Aux", true, pvol, pmute, svol, smute, false };
    c.peak = peak;
    return c;
}
} // namespace

MDXM_TEST_CASE(FaderPeak_FullFaderPassesTheSourceUnchanged) {
    const ChannelState c = Chan(0.7f, 1.0f, false, 1.0f, false);
    CHECK_NEAR(FaderPeak(c, Mix::Personal), 0.7f, 1e-6);
    CHECK_NEAR(FaderPeak(c, Mix::Streaming), 0.7f, 1e-6);
}

// A gain stage scales a peak linearly, so this is arithmetic rather than a
// guess -- see the note in mixer_control.h about what it does and does not
// claim.
MDXM_TEST_CASE(FaderPeak_HalfFaderHalvesIt) {
    const ChannelState c = Chan(0.8f, 0.5f, false, 0.25f, false);
    CHECK_NEAR(FaderPeak(c, Mix::Personal), 0.4f, 1e-6);
    CHECK_NEAR(FaderPeak(c, Mix::Streaming), 0.2f, 1e-6);
}

// Shane's own case, and the one that started this.
MDXM_TEST_CASE(FaderPeak_TheTwoSidesAreIndependent) {
    const ChannelState c = Chan(0.715927f, 0.0f, false, 1.0f, false);
    // Nothing is reaching his ears through this channel, so nothing is drawn.
    CHECK_NEAR(FaderPeak(c, Mix::Personal), 0.0f, 1e-6);
    // The stream is getting all of it, which is what the green bar was really
    // showing all along.
    CHECK_NEAR(FaderPeak(c, Mix::Streaming), 0.715927f, 1e-6);
}

MDXM_TEST_CASE(FaderPeak_MutePassesNothingAtAnyVolume) {
    const ChannelState c = Chan(0.9f, 1.0f, true, 1.0f, true);
    CHECK_NEAR(FaderPeak(c, Mix::Personal), 0.0f, 1e-6);
    CHECK_NEAR(FaderPeak(c, Mix::Streaming), 0.0f, 1e-6);
}

// "Cannot know" must not become "silent" on the way through, which is the
// whole of §2.1's first rule. An unhealthy channel has no capture to meter,
// and a fader cannot turn that into a measurement of silence.
MDXM_TEST_CASE(FaderPeak_UnknownStaysUnknown) {
    const ChannelState open = Chan(kPeakUnknown, 1.0f, false, 1.0f, false);
    CHECK(FaderPeak(open, Mix::Personal) == kPeakUnknown);
    CHECK(FaderPeak(open, Mix::Streaming) == kPeakUnknown);
    // Even muted, and even at zero: the mute says what this fader is passing,
    // not whether the source could be read.
    const ChannelState shut = Chan(kPeakUnknown, 0.0f, true, 0.0f, true);
    CHECK(FaderPeak(shut, Mix::Personal) == kPeakUnknown);
    CHECK(FaderPeak(shut, Mix::Streaming) == kPeakUnknown);
}
