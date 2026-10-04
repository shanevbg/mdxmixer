#include "test_framework.h"
#include "dsp/biquad.h"
#include <cmath>
#include <cstring>
#include <vector>

using namespace mdxm;

namespace {
// Gain of `eq` at freqHz, measured by sine RMS ratio (mono content on both channels).
double MeasureGainDb(ChannelEq& eq, double freqHz, double fs) {
    const size_t warm = 4800, meas = 48000;
    std::vector<float> buf(2 * (warm + meas));
    for (size_t i = 0; i < warm + meas; ++i) {
        float s = (float)std::sin(2.0 * 3.14159265358979 * freqHz * (double)i / fs);
        buf[i*2] = s; buf[i*2+1] = s;
    }
    eq.Process(buf.data(), warm + meas);
    double sum = 0.0;
    for (size_t i = warm; i < warm + meas; ++i) sum += (double)buf[i*2] * buf[i*2];
    double rmsOut = std::sqrt(sum / meas);
    return 20.0 * std::log10(rmsOut / 0.70710678);
}
} // namespace

MDXM_TEST_CASE(Peaking_BoostsAtCenterOnly) {
    ChannelEq eq;
    eq.Configure(48000.0);
    eq.SetBand(0, 1000.0, 6.0, 1.0);
    eq.SetEnabled(true);
    CHECK_NEAR(MeasureGainDb(eq, 1000.0, 48000.0), 6.0, 0.3);
    ChannelEq eq2; eq2.Configure(48000.0); eq2.SetBand(0, 1000.0, 6.0, 1.0); eq2.SetEnabled(true);
    CHECK_NEAR(MeasureGainDb(eq2, 100.0, 48000.0), 0.0, 0.3);   // a decade away: flat
}

MDXM_TEST_CASE(Peaking_DisabledIsIdentity) {
    ChannelEq eq;
    eq.Configure(48000.0);
    eq.SetBand(0, 1000.0, 12.0, 4.0);
    eq.SetEnabled(false);
    float buf[8] = {0.5f, -0.5f, 0.25f, -0.25f, 1.0f, -1.0f, 0.0f, 0.0f};
    float ref[8]; std::memcpy(ref, buf, sizeof buf);
    eq.Process(buf, 4);
    for (int i = 0; i < 8; ++i) CHECK(buf[i] == ref[i]);         // bit-identical when bypassed
}

MDXM_TEST_CASE(Peaking_ClampsInsaneParams) {
    // Review Focus #4. Nyquist-and-beyond freq, absurd gain, zero Q: coefficients stay finite.
    BiquadCoeffs c = MakePeaking(48000.0, 96000.0, 60.0, 0.0);
    CHECK(std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2));
    CHECK(std::isfinite(c.a1) && std::isfinite(c.a2));
    BiquadCoeffs c2 = MakePeaking(48000.0, -5.0, -60.0, 1000.0);
    CHECK(std::isfinite(c2.b0) && std::isfinite(c2.a2));
}

MDXM_TEST_CASE(Peaking_StableAtExtremes) {
    // Impulse response of a clamped-extreme band must decay, not ring forever or blow up.
    ChannelEq eq;
    eq.Configure(48000.0);
    eq.SetBand(0, 96000.0, 60.0, 0.0);   // gets clamped internally
    eq.SetEnabled(true);
    std::vector<float> buf(2 * 48000, 0.0f);
    buf[0] = buf[1] = 1.0f;
    eq.Process(buf.data(), 48000);
    double tail = 0.0;
    for (size_t i = 47000; i < 48000; ++i) tail = std::fmax(tail, std::fabs((double)buf[i*2]));
    CHECK(tail < 1e-3);
    for (size_t i = 0; i < 48000; ++i) CHECK(std::fabs(buf[i*2]) < 100.0);
}
