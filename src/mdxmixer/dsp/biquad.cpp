#include "biquad.h"
#include <algorithm>
#include <cmath>

namespace mdxm {

BiquadCoeffs MakePeaking(double sampleRate, double freqHz, double gainDb, double q) {
    freqHz = std::clamp(freqHz, 10.0, 0.45 * sampleRate);
    gainDb = std::clamp(gainDb, -24.0, 24.0);
    q      = std::clamp(q, 0.1, 18.0);
    const double A     = std::pow(10.0, gainDb / 40.0);
    const double w0    = 2.0 * 3.14159265358979323846 * freqHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha / A;
    BiquadCoeffs c;
    c.b0 = (1.0 + alpha * A) / a0;
    c.b1 = (-2.0 * cosw0) / a0;
    c.b2 = (1.0 - alpha * A) / a0;
    c.a1 = (-2.0 * cosw0) / a0;
    c.a2 = (1.0 - alpha / A) / a0;
    return c;
}

} // namespace mdxm
