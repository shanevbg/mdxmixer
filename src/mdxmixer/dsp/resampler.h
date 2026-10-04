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
        m_ratio = inRate / outRate;      // input frames per output frame
        m_passthrough = (inRate == outRate);
        m_pos = 0.0;
        m_havePrev = false;
    }
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
    double m_pos = 0.0;
    bool m_passthrough = true;
    bool m_havePrev = false;
    float m_hist[4] = {0, 0, 0, 0};
};

} // namespace mdxm
