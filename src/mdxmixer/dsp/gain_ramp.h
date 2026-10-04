#pragma once
// Linear per-frame gain ramp, ~10 ms for a full 0->1 traverse. Mute is SetTarget(0).
// Target is atomic: UI/IPC threads set it, the audio thread reads it.
#include <atomic>
#include <cmath>

namespace mdxm {

class GainRamp {
public:
    void Configure(double sampleRate) {
        m_step = (float)(1.0 / (0.010 * sampleRate)); // full-scale traverse in 10 ms
    }
    void SetTarget(float gain) { m_target.store(gain, std::memory_order_relaxed); }
    void SnapTo(float gain) { m_current = gain; m_target.store(gain, std::memory_order_relaxed); }
    float Current() const { return m_current; }

    void Process(float* interleaved, size_t frames) {
        float target = m_target.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) {
            if (m_current < target) m_current = std::fmin(m_current + m_step, target);
            else if (m_current > target) m_current = std::fmax(m_current - m_step, target);
            interleaved[i * 2]     *= m_current;
            interleaved[i * 2 + 1] *= m_current;
        }
    }

private:
    float m_current = 1.0f;
    float m_step = 1.0f / 480.0f;
    std::atomic<float> m_target{1.0f};
};

} // namespace mdxm
