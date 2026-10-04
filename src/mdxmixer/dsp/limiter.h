#pragma once
// Soft limiter: summing N channels at unity must not clip the device buffer.
// Waveshaper, not a lookahead compressor — zero latency, good enough at this gain staging.
#include <cmath>

namespace mdxm {

class SoftLimiter {
public:
    void Process(float* interleaved, size_t frames) {
        const float knee = 0.89125f; // -1 dBFS
        const size_t n = frames * 2;
        for (size_t i = 0; i < n; ++i) {
            float x = interleaved[i];
            float ax = std::fabs(x);
            if (ax <= knee) continue;
            float shaped = knee + (1.0f - knee) * std::tanh((ax - knee) / (1.0f - knee));
            interleaved[i] = x < 0 ? -shaped : shaped;
        }
    }
};

} // namespace mdxm
