#pragma once
// Linear-interpolation resampler with fractional carry across blocks.
// Same-rate is an exact copy, per the passthrough-monitor lessons (bit-identical check in tests).
//
// Model: output sample at position t (in input-frame units) interpolates between
// input frames floor(t) and floor(t)+1, where negative indices (-1, -2) are the
// carried last frames of the previous block.
//
// Two usage patterns, both tested:
//  * PUSH (capture side): feed every captured block, take however many frames
//    come out — size the output with EstimateOut().
//  * PULL (render side): ask NeedInput(k) for the EXACT source frames required
//    to produce k output frames, feed exactly that, and Process returns exactly k.
//    Feeding ceil(k*ratio)+slack instead silently over-consumes source frames
//    every call and walks the position off the front of the buffer — the bug the
//    pull tests pin.
#include <cmath>
#include <cstring>

namespace mdxm {

class LinearResampler {
public:
    void SetRates(double inRate, double outRate) {
        m_inRate = inRate;
        m_outRate = outRate;
        m_speed = 1.0;
        m_ratio = inRate / outRate;      // input frames per output frame
        m_passthrough = (inRate == outRate);
        m_pos = 0.0;
        m_havePrev = false;
    }

    // Consume input faster than real time, to drain a backlogged ring (fj#13).
    //
    // A speed of 1.05 takes five percent more input per output frame, so the
    // producer puts five percent fewer frames into the ring per second than the
    // consumer takes out, and the depth falls at five percent of real time. The
    // audio plays correspondingly faster and higher; see dsp/ring_speed.h for
    // why the rate is proportional to the backlog rather than fixed, and why a
    // speed BELOW 1.0 -- refilling a ring that has run short -- matters just as
    // much as draining one that has filled.
    //
    // DOES NOT RESET THE POSITION, unlike SetRates: this is adjusted while
    // audio is flowing, and resetting m_pos mid-stream would put a
    // discontinuity into the very signal the mechanism exists to keep smooth.
    //
    // It also has to switch passthrough OFF. At 48 kHz into a 48 kHz mix the
    // rates are equal and the resampler is skipped entirely, so without this a
    // speed trim would silently do nothing -- which is exactly the case on this
    // machine.
    void SetSpeed(double speed) {
        if (speed <= 0.0) speed = 1.0;
        m_speed = speed;
        m_ratio = (m_inRate / m_outRate) * speed;
        m_passthrough = (m_inRate == m_outRate) && (speed == 1.0);
    }
    double Speed() const { return m_speed; }
    bool IsPassthrough() const { return m_passthrough; }

    size_t EstimateOut(size_t inFrames) const {
        return m_passthrough ? inFrames : (size_t)std::ceil((double)inFrames / m_ratio) + 2;
    }

    // Exact input frames required so the NEXT Process call can produce outFrames.
    size_t NeedInput(size_t outFrames) const {
        if (m_passthrough || outFrames == 0) return outFrames;
        long need = (long)std::floor(m_pos + (double)(outFrames - 1) * m_ratio) + 2;
        return need > 0 ? (size_t)need : 0;
    }

    size_t Process(const float* in, size_t inFrames, float* out, size_t maxOutFrames) {
        if (m_passthrough) {
            size_t n = inFrames < maxOutFrames ? inFrames : maxOutFrames;
            std::memcpy(out, in, n * 2 * sizeof(float));
            return n;
        }
        if (inFrames == 0) return 0;
        size_t produced = 0;
        while (produced < maxOutFrames) {
            long i0 = (long)std::floor(m_pos);
            double frac = m_pos - (double)i0;
            if (i0 + 1 >= (long)inFrames) break;             // need the next frame; carry to next block
            float l0 = SampleL(in, i0), r0 = SampleR(in, i0);
            float l1 = SampleL(in, i0 + 1), r1 = SampleR(in, i0 + 1);
            out[produced*2]   = (float)(l0 + (l1 - l0) * frac);
            out[produced*2+1] = (float)(r0 + (r1 - r0) * frac);
            ++produced;
            m_pos += m_ratio;
        }
        // Carry: keep the last two input frames, rebase position for the next block.
        if (inFrames >= 2) {
            m_hist[0] = in[(inFrames-2)*2]; m_hist[1] = in[(inFrames-2)*2+1];
            m_hist[2] = in[(inFrames-1)*2]; m_hist[3] = in[(inFrames-1)*2+1];
        } else {   // single-frame block: shift
            m_hist[0] = m_hist[2]; m_hist[1] = m_hist[3];
            m_hist[2] = in[0];     m_hist[3] = in[1];
        }
        m_havePrev = true;
        m_pos -= (double)inFrames;
        return produced;
    }

private:
    // Negative indices reach the carried history: -1 = last frame of the previous
    // block, -2 = the one before it. The exact-need pull pattern keeps the rebased
    // position within (-2, 1], so nothing older is ever needed.
    float SampleL(const float* in, long i) const {
        if (i >= 0) return in[i*2];
        if (!m_havePrev) return in[0];
        return i == -1 ? m_hist[2] : m_hist[0];
    }
    float SampleR(const float* in, long i) const {
        if (i >= 0) return in[i*2+1];
        if (!m_havePrev) return in[1];
        return i == -1 ? m_hist[3] : m_hist[1];
    }

    double m_ratio = 1.0;
    // The rates as given, kept so a speed trim can recompute the ratio without
    // the caller having to repeat them.
    double m_inRate = 1.0, m_outRate = 1.0, m_speed = 1.0;
    double m_pos = 0.0;
    bool m_passthrough = true;
    bool m_havePrev = false;
    float m_hist[4] = {0, 0, 0, 0};
};

} // namespace mdxm
