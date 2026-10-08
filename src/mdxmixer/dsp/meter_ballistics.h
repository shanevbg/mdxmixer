#pragma once
// How fast a meter on SCREEN is allowed to fall.
//
// Separate from PeakHold, and the difference is the whole reason this exists.
// A hold answers "which row made that noise a moment ago" and is right for the
// wire: MDR_Android polls MDropDX12, which polls mdxmixer, and a spike that
// does not survive that chain may as well not have been published. 1.5 seconds
// is the number peak_hold.h argues for and it stays.
//
// A meter being WATCHED is a different question. Held for 1.5 s and then
// dropped in one step, it reads as a row that is still loud long after the
// sound stopped, and then as a glitch when it falls off a cliff -- "can we
// make the system a little more responsive with the sonar muting and meters".
// Every hardware meter solves this the same way: instant attack, measured
// release.
//
// SO: the surface applies this to the UNHELD reading (DeviceLevel::peakNow,
// ChannelState::peakNow), and the wire goes on carrying the held one. Neither
// is a compromise for the other.
#include "dsp/peak_hold.h"   // kPeakUnknown
#include <cstddef>

namespace mdxm {

// How long the meter takes to fall the full scale, in milliseconds.
//
// 300 ms over the whole bar: three frames of the 100 ms refresh, which is
// enough for the eye to read it as falling rather than blinking out, and
// short enough that the bar is describing what is happening NOW.
//
// It was 650 first, reasoning from broadcast PPM return times (~1.5 s for
// 24 dB). Shane's verdict on that was "meters are sluggish", and he is right
// for this meter: a PPM is a logarithmic scale on a continuously-lit display,
// while this is a linear bar sampled ten times a second, so the same return
// time reads as lag rather than as ballistics.
constexpr double kMeterFallMsFullScale = 300.0;

// The level to draw now, given what is drawn, what has just been read, and how
// long since the last redraw.
//
// `reading` of kPeakUnknown means the source cannot be read at all -- not
// silence, and the caller must draw nothing rather than draw zero, so that is
// passed straight through.
inline float MeterFall(float shown, float reading, unsigned elapsedMs) {
    if (reading < 0.0f) return kPeakUnknown;        // cannot know: say so
    if (shown < 0.0f) return reading;               // first reading after unknown
    // Attack is instant. A meter that lags a transient is a meter that misses
    // the only event anyone is watching for.
    if (reading >= shown) return reading;
    if (elapsedMs == 0) return shown;
    const double step = (double)elapsedMs / kMeterFallMsFullScale;
    const double next = (double)shown - step;
    return next <= (double)reading ? reading : (float)next;
}

} // namespace mdxm
