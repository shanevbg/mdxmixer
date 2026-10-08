#pragma once
// ring_speed.h — how fast to run the producer so a ring holds its cushion.
//
// The mix wants the ring sitting at the cushion: deeper is latency nobody
// asked for, shallower is an underrun waiting to happen. Two independent
// clocks guarantee it will not stay there on its own, so something has to
// steer it, and the cheapest steering available is to resample the producer's
// output a fraction of a percent off real time.
//
// TOO FULL -> speed above 1.0. More input consumed per output frame, so fewer
// frames reach the ring each second than the consumer takes, and the depth
// falls. Shane's design (fj#13): "drain the ring by playing sound back at 1.1
// times until latency down", with "varispeed approaching 5% as the number
// approaches 500ms". Measured cause: a Bluetooth driver switch stalled the
// consumer, the ring filled, and 1.99 s of latency stayed for ever.
//
// TOO EMPTY -> speed below 1.0, and this half was missing at first. With
// multipoint connecting and the codec on AAC the ring ran SHORT, falling from
// 1097 frames to 191 against a 720-frame cushion and taking ten underruns,
// while the trim sat at exactly 1.0 because it only knew how to speed up. A
// controller that corrects one sign of drift and ignores the other is half a
// controller, and the half it ignores produces audible gaps for ever.
//
// WHY PROPORTIONAL. The common case is a few milliseconds either way --
// ordinary drift between the cable's clock and the headset's -- and it has to
// be inaudible: at a few hundredths of a percent it is. The full five percent
// only appears at the extremes, where the alternative is half a second of
// latency or a stream that is already starving, and where it is worth hearing.
//
// Pure, so both directions are testable without an audio device.
#include <cstddef>

namespace mdxm {

// The maximum trim, either way. Five percent is about four fifths of a
// semitone -- audible on a sustained tone if you listen for it, which is the
// right trade against the alternatives at the extremes.
constexpr double kMaxRingTrim = 0.05;

// `depth`, `target` (the cushion) and `capacity` in frames. Returns the factor
// to multiply the producer's resample ratio by: above 1.0 drains, below 1.0
// fills, exactly 1.0 leaves it alone.
inline double RingSpeed(size_t depth, size_t target, size_t capacity) {
    if (target == 0 || capacity <= target) return 1.0;   // nothing to aim at
    if (depth > target) {
        const size_t excess = depth - target;
        const size_t span = capacity - target;
        const double frac = excess >= span ? 1.0 : (double)excess / (double)span;
        return 1.0 + kMaxRingTrim * frac;
    }
    if (depth < target) {
        // The deficit is measured against the cushion itself: empty is the
        // worst case and earns the full correction.
        const double frac = (double)(target - depth) / (double)target;
        return 1.0 - kMaxRingTrim * frac;
    }
    return 1.0;
}

} // namespace mdxm
