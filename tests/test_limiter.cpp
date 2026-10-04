#include "test_framework.h"
#include "dsp/limiter.h"
#include <cmath>
#include <cstring>

using mdxm::SoftLimiter;

MDXM_TEST_CASE(Limiter_IdentityBelowKnee) {
    SoftLimiter lim;
    float buf[6] = {0.5f, -0.5f, 0.88f, -0.88f, 0.0f, 0.1f};
    float ref[6]; std::memcpy(ref, buf, sizeof buf);
    lim.Process(buf, 3);
    for (int i = 0; i < 6; ++i) CHECK(buf[i] == ref[i]);
}

MDXM_TEST_CASE(Limiter_CeilingNeverExceeded) {
    SoftLimiter lim;
    float buf[8] = {1.5f, -1.5f, 4.0f, -4.0f, 8.0f, -8.0f, 1.0f, -1.0f};
    lim.Process(buf, 4);
    // <= not <: float tanh saturates to exactly 1.0, and the spec's requirement is
    // "must not clip" — magnitude 1.0 does not clip. (Ledgered ruling, Task 5.)
    for (int i = 0; i < 8; ++i) CHECK(std::fabs(buf[i]) <= 1.0f);
}

MDXM_TEST_CASE(Limiter_Monotonic) {
    // Louder in must never come out quieter — no inversion artifacts at the knee.
    SoftLimiter lim;
    float prev = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        float buf[2] = {0.05f * i, 0.05f * i};
        lim.Process(buf, 1);
        CHECK(buf[0] >= prev);
        prev = buf[0];
    }
}
