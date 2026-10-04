#include "test_framework.h"
#include "dsp/gain_ramp.h"
#include <cmath>
#include <vector>

using mdxm::GainRamp;

MDXM_TEST_CASE(Ramp_ReachesTargetWithin10msAndHolds) {
    GainRamp g;
    g.Configure(48000.0);
    g.SnapTo(0.0f);
    g.SetTarget(1.0f);
    std::vector<float> buf(2 * 480, 1.0f);      // 10 ms of ones
    g.Process(buf.data(), 480);
    CHECK_NEAR(g.Current(), 1.0f, 1e-4);
    CHECK_NEAR(buf[2 * 479], 1.0f, 1e-3);        // last frame at full gain
    std::vector<float> buf2(2 * 48, 1.0f);
    g.Process(buf2.data(), 48);
    for (auto v : buf2) CHECK_NEAR(v, 1.0f, 1e-6); // holds exactly at target
}

MDXM_TEST_CASE(Ramp_NoClicks_BoundedFirstDifference) {
    GainRamp g;
    g.Configure(48000.0);
    g.SnapTo(1.0f);
    g.SetTarget(0.0f);                           // a mute
    std::vector<float> buf(2 * 960, 1.0f);       // DC input exposes gain steps directly
    g.Process(buf.data(), 960);
    double maxStep = 0.0;
    for (size_t i = 1; i < 960; ++i)
        maxStep = std::fmax(maxStep, std::fabs((double)buf[i*2] - (double)buf[(i-1)*2]));
    CHECK(maxStep < 0.005);                      // ~1/480 per frame + slack; a hard mute would be 1.0
    CHECK_NEAR(g.Current(), 0.0f, 1e-4);
}

MDXM_TEST_CASE(Ramp_BothChannelsSameGain) {
    GainRamp g;
    g.Configure(48000.0);
    g.SnapTo(0.5f);
    float buf[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    g.Process(buf, 2);
    CHECK_NEAR(buf[0], -buf[1], 1e-6);
    CHECK_NEAR(buf[2], -buf[3], 1e-6);
}
