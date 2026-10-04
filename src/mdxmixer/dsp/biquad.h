#pragma once
// RBJ Audio-EQ-Cookbook peaking biquad, Direct Form II transposed, stereo.
// ChannelEq is a cascade of 10 bands sharing one filter per channel pair.
//
// Cross-thread contract: SetBand/SetEnabled come from UI/IPC threads while the
// audio thread runs Process. Band edits go into a staged copy under m_mutex;
// Process try_locks — on success it adopts the staged coefficients, on a miss
// it keeps the previous coefficients for this block. The audio thread never
// blocks and never reads torn coefficients.
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace mdxm {

struct BiquadCoeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; }; // normalized, a0 divided out

// Clamps freq to [10, 0.45*fs], gainDb to [-24, +24], q to [0.1, 18] before computing.
BiquadCoeffs MakePeaking(double sampleRate, double freqHz, double gainDb, double q);

class StereoBiquad {
public:
    void SetCoeffs(const BiquadCoeffs& c) { m_c = c; }
    void Reset() { m_zL1 = m_zL2 = m_zR1 = m_zR2 = 0.0; }
    void Process(float* interleaved, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            double xL = interleaved[i * 2], xR = interleaved[i * 2 + 1];
            double yL = m_c.b0 * xL + m_zL1;
            m_zL1 = m_c.b1 * xL - m_c.a1 * yL + m_zL2;
            m_zL2 = m_c.b2 * xL - m_c.a2 * yL;
            double yR = m_c.b0 * xR + m_zR1;
            m_zR1 = m_c.b1 * xR - m_c.a1 * yR + m_zR2;
            m_zR2 = m_c.b2 * xR - m_c.a2 * yR;
            interleaved[i * 2]     = (float)yL;
            interleaved[i * 2 + 1] = (float)yR;
        }
    }

private:
    BiquadCoeffs m_c;
    double m_zL1 = 0, m_zL2 = 0, m_zR1 = 0, m_zR2 = 0;
};

class ChannelEq {
public:
    static constexpr size_t kBands = 10;

    void Configure(double sampleRate) { m_sampleRate = sampleRate; }

    void SetBand(size_t band, double freqHz, double gainDb, double q) {
        if (band >= kBands) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_staged[band] = MakePeaking(m_sampleRate, freqHz, gainDb, q);
        // A 0 dB band is identity: skip it entirely instead of burning cycles.
        m_stagedActive[band] = (gainDb > 0.0001 || gainDb < -0.0001);
        m_dirty = true;
    }

    void SetEnabled(bool on) { m_enabled.store(on, std::memory_order_relaxed); }
    bool Enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    void Process(float* interleaved, size_t frames) {
        if (!m_enabled.load(std::memory_order_relaxed)) return;
        if (m_mutex.try_lock()) {
            if (m_dirty) {
                for (size_t b = 0; b < kBands; ++b) {
                    m_filters[b].SetCoeffs(m_staged[b]);
                    if (m_stagedActive[b] != m_active[b]) m_filters[b].Reset();
                    m_active[b] = m_stagedActive[b];
                }
                m_dirty = false;
            }
            m_mutex.unlock();
        }
        for (size_t b = 0; b < kBands; ++b)
            if (m_active[b]) m_filters[b].Process(interleaved, frames);
    }

private:
    double m_sampleRate = 48000.0;
    std::array<StereoBiquad, kBands> m_filters;
    std::array<bool, kBands> m_active{};
    std::atomic<bool> m_enabled{false};

    std::mutex m_mutex;                       // guards the staged copies + dirty flag
    std::array<BiquadCoeffs, kBands> m_staged{};
    std::array<bool, kBands> m_stagedActive{};
    bool m_dirty = false;
};

} // namespace mdxm
