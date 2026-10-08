#include "test_framework.h"
#include "dsp/resampler.h"
#include <cmath>
#include <vector>

using mdxm::LinearResampler;

MDXM_TEST_CASE(Resampler_SameRateIsBitIdentical) {
    LinearResampler rs;
    rs.SetRates(48000.0, 48000.0);
    CHECK(rs.IsPassthrough());
    std::vector<float> in(2 * 100), out(2 * 104);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (float)std::sin(0.1 * (double)i);
    size_t produced = rs.Process(in.data(), 100, out.data(), 104);
    CHECK(produced == 100);
    for (size_t i = 0; i < 200; ++i) CHECK(out[i] == in[i]);   // exact copy
}

MDXM_TEST_CASE(Resampler_PullExactWithNeedInput) {
    // The pull pattern (render callbacks): ask NeedInput(k) for the exact source
    // frames required, feed exactly that, get exactly k out — over thousands of
    // blocks, with no cumulative position drift and a continuous waveform.
    // (The naive ceil(k*ratio)+1 pattern over-consumes one frame per call and
    // walks the resampler position off the front of the buffer.)
    LinearResampler rs;
    rs.SetRates(44100.0, 48000.0);                 // upsample, the worst case for the seam
    std::vector<float> outAll;
    size_t srcPhase = 0;
    for (int block = 0; block < 2000; ++block) {   // 20 s of 10 ms pulls
        const size_t k = 480;
        size_t need = rs.NeedInput(k);
        CHECK(need <= 444);                        // never wildly more than k*ratio
        std::vector<float> in(2 * need), out(2 * k);
        for (size_t i = 0; i < need; ++i) {
            float s = (float)std::sin(2.0 * 3.14159265358979 * 1000.0 * (double)(srcPhase + i) / 44100.0);
            in[i*2] = s; in[i*2+1] = s;
        }
        srcPhase += need;
        size_t produced = rs.Process(in.data(), need, out.data(), k);
        CHECK(produced == k);                      // exact production, every block
        outAll.insert(outAll.end(), out.begin(), out.end());
    }
    size_t total = outAll.size() / 2;
    double sum = 0.0; int crossings = 0;
    for (size_t i = 1000; i < total; ++i) {
        sum += (double)outAll[i*2] * outAll[i*2];
        if ((outAll[i*2] >= 0) != (outAll[(i-1)*2] >= 0)) ++crossings;
    }
    double rms = std::sqrt(sum / (double)(total - 1000));
    CHECK_NEAR(rms, 0.70710678, 0.01);
    double seconds = (double)(total - 1000) / 48000.0;
    CHECK_NEAR(crossings / seconds / 2.0, 1000.0, 5.0);
    double maxJump = 0.0;                          // continuity across every pull seam
    for (size_t i = 1; i < total; ++i)
        maxJump = std::fmax(maxJump, std::fabs((double)outAll[i*2] - (double)outAll[(i-1)*2]));
    CHECK(maxJump < 0.30);                         // 1 kHz @48k moves ~0.13/sample; DC jumps would be ~1.4
}

MDXM_TEST_CASE(Resampler_441to48_TonePreserved) {
    // Review Focus #2: a cable left at 44.1 kHz. A 1 kHz sine at 44.1 in must come out
    // a 1 kHz sine at 48 with the same RMS, fed in awkward 441-frame blocks.
    LinearResampler rs;
    rs.SetRates(44100.0, 48000.0);
    std::vector<float> outAll;
    size_t phase = 0;
    for (int block = 0; block < 100; ++block) {
        std::vector<float> in(2 * 441), out(2 * 512);
        for (size_t i = 0; i < 441; ++i) {
            float s = (float)std::sin(2.0 * 3.14159265358979 * 1000.0 * (double)(phase + i) / 44100.0);
            in[i*2] = s; in[i*2+1] = s;
        }
        phase += 441;
        size_t produced = rs.Process(in.data(), 441, out.data(), 512);
        outAll.insert(outAll.end(), out.begin(), out.begin() + produced * 2);
    }
    size_t total = outAll.size() / 2;
    CHECK(total > 47500 && total < 48500);                    // ~1 s at 48 kHz out
    double sum = 0.0;
    for (size_t i = 1000; i < total; ++i) sum += (double)outAll[i*2] * outAll[i*2];
    double rms = std::sqrt(sum / (double)(total - 1000));
    CHECK_NEAR(rms, 0.70710678, 0.02);                        // sine RMS survives resampling
    double maxJump = 0.0;                                     // continuity across block seams
    for (size_t i = 1; i < total; ++i)
        maxJump = std::fmax(maxJump, std::fabs((double)outAll[i*2] - (double)outAll[(i-1)*2]));
    CHECK(maxJump < 0.20);                                    // 1 kHz @48k moves ~0.13/sample max
}

// The varispeed trim that drains a backlogged ring (fj#13).
//
// THE TRAP THIS PINS: at equal rates the resampler is in PASSTHROUGH and is
// skipped entirely. On this machine the capture and the mix are both 48 kHz, so
// a speed trim that forgot to switch passthrough off would commit cleanly and
// do absolutely nothing -- the whole feature silently inert on the only
// configuration it was written for.
MDXM_TEST_CASE(Resample_SpeedLeavesPassthroughBehind) {
    mdxm::LinearResampler r;
    r.SetRates(48000.0, 48000.0);
    CHECK(r.IsPassthrough());
    CHECK(r.Speed() == 1.0);

    r.SetSpeed(1.05);
    CHECK(!r.IsPassthrough());             // the trap
    CHECK(r.Speed() == 1.05);

    // Five percent faster means five percent FEWER output frames per input
    // block, which is what makes the ring drain.
    std::vector<float> in(2048 * 2, 0.0f);
    for (size_t i = 0; i < 2048; ++i) in[i * 2] = in[i * 2 + 1] = (float)(i % 100) / 100.0f;
    std::vector<float> out(4096 * 2, 0.0f);
    const size_t produced = r.Process(in.data(), 2048, out.data(), 4096);
    CHECK(produced < 2048);
    CHECK(produced > 1900);                // ~2048/1.05 = 1950
    CHECK_NEAR((double)produced, 2048.0 / 1.05, 8.0);

    // And back to exactly normal, passthrough restored.
    r.SetSpeed(1.0);
    CHECK(r.IsPassthrough());
    CHECK(r.Speed() == 1.0);
}

// It is adjusted while audio is flowing, so unlike SetRates it must not reset
// the interpolation position -- doing so would put a discontinuity into the
// signal the mechanism exists to keep smooth.
MDXM_TEST_CASE(Resample_SpeedChangeDoesNotRestartThePosition) {
    mdxm::LinearResampler a, b;
    a.SetRates(48000.0, 44100.0);
    b.SetRates(48000.0, 44100.0);

    std::vector<float> in(512 * 2, 0.0f);
    for (size_t i = 0; i < 512; ++i) in[i * 2] = in[i * 2 + 1] = (float)i;
    std::vector<float> outA(1024 * 2, 0.0f), outB(1024 * 2, 0.0f);

    a.Process(in.data(), 512, outA.data(), 1024);
    b.Process(in.data(), 512, outB.data(), 1024);

    // Same rates, so a no-op speed change must leave the second block of the
    // nudged one identical to the untouched one.
    b.SetSpeed(1.0);
    const size_t pa = a.Process(in.data(), 512, outA.data(), 1024);
    const size_t pb = b.Process(in.data(), 512, outB.data(), 1024);
    CHECK(pa == pb);
    bool identical = true;
    for (size_t i = 0; i < pa * 2 && identical; ++i)
        if (outA[i] != outB[i]) identical = false;
    CHECK(identical);
}
