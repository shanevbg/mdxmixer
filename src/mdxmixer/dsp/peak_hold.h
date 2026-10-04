#pragma once
// A peak that stays findable for a moment after the sound that made it.
//
// An instantaneous peak is useless for the thing a meter is actually FOR. The
// motivating case is Shane's: twenty-five faders, one of them suddenly
// blasting, and "the fader positions do not move when it happens" — so the
// only thing that can point at the culprit is the level. By the time a list
// redraws, an instantaneous read has already fallen back to nothing and the
// row that blasted looks exactly like the twenty-four that did not.
//
// So: the highest value seen in the last `hold` seconds, then release. A
// sliding maximum, which is what every hardware meter with a peak LED does.
//
// TIME IS COUNTED IN TICKS THE CALLER DEFINES — frames on the audio thread,
// milliseconds on the control thread. The audio-thread user (Engine::MixPull)
// must not call a clock and already knows exactly how many frames it
// processed; the control-thread user (the endpoint sweep) knows how long it
// has been since its last sweep. One mechanism serves both, nothing calls
// QueryPerformanceCounter on an audio thread, and a unit test can advance
// time exactly instead of sleeping.
//
// Header-only and allocation-free, so it is safe to use on the mix thread.
#include <cstddef>
#include <cstdint>

namespace mdxm {

// How long a peak stays up, in milliseconds, and it is ONE number for every
// meter in the program on purpose.
//
// 1.5 s, chosen against the two things that bracket it. Below about a second a
// spike can be gone before a remote list has redrawn — MDR_Android polls
// MDropDX12, which polls mdxmixer, and that chain is not fast. Above two or
// three seconds the meter stops tracking the music and starts describing the
// recent past, so two rows that took turns being loud both read loud and the
// ordering says nothing. It is also roughly where a hardware peak LED sits,
// for the same reason.
constexpr size_t kPeakHoldMs = 1500;

// The same duration in frames, for the mix thread. uint64 arithmetic because
// 1500 * 192000 overflows a 32-bit intermediate.
inline size_t PeakHoldFramesFor(uint32_t sampleRate) {
    return (size_t)((uint64_t)sampleRate * kPeakHoldMs / 1000);
}

// The value meaning "there is no reading", distinct from 0.0f meaning "there
// is a reading and it is silence".
//
// That difference is the whole point of publishing a peak at all. A source
// that cannot be metered must say so rather than reporting zero, which would
// look authoritative and sort wrongly — the same rule MDXM_DEVLVL already
// applies to `battery`.
constexpr float kPeakUnknown = -1.0f;

class PeakHold {
public:
    // `holdTicks` is how long a peak survives after the audio that made it, in
    // the caller's own unit: zero disables the hold and Value() becomes
    // whatever was pushed last.
    void Configure(size_t holdTicks) { m_holdFrames = holdTicks; }

    // One block. `peak` is that block's own maximum, `frames` how many of the
    // caller's ticks it covered.
    void Push(float peak, size_t frames) {
        if (peak < 0.0f) peak = 0.0f;
        m_known = true;
        if (peak >= m_value) {
            // A new high restarts the hold. Equality counts as a new high so a
            // steady signal never expires and re-latches.
            m_value = peak;
            m_remaining = m_holdFrames;
            return;
        }
        if (m_remaining > frames) {
            m_remaining -= frames;
            return;
        }
        // The hold expired: drop straight to what is there now. Not a decay
        // slope — a slope reads as "it is getting quieter", which is a
        // different claim from "that was a moment ago".
        m_value = peak;
        m_remaining = m_holdFrames;
    }

    // Nothing can be read from this source right now: an unhealthy channel, an
    // endpoint that would not activate a meter. Forgets the held value rather
    // than freezing it, because a stale peak presented as current is worse
    // than no peak — it points at the wrong row.
    void MarkUnknown() {
        m_known = false;
        m_value = 0.0f;
        m_remaining = 0;
    }

    // kPeakUnknown until something is pushed.
    float Value() const { return m_known ? m_value : kPeakUnknown; }
    bool Known() const { return m_known; }

private:
    size_t m_holdFrames = 0;
    size_t m_remaining = 0;
    float m_value = 0.0f;
    bool m_known = false;
};

// The maximum absolute sample in an interleaved stereo block. Separate from
// PeakHold so the scan can be tested on its own and so a caller that already
// has a peak (IAudioMeterInformation hands one over) skips it.
inline float BlockPeak(const float* interleaved, size_t frames) {
    float hi = 0.0f;
    const size_t n = frames * 2;
    for (size_t i = 0; i < n; ++i) {
        const float a = interleaved[i] < 0.0f ? -interleaved[i] : interleaved[i];
        if (a > hi) hi = a;
    }
    return hi;
}

} // namespace mdxm
