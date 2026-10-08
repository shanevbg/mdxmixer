// test_ring_speed.cpp — the varispeed trim that holds a ring at its cushion,
// in BOTH directions (fj#13).
//
// The drain half was Shane's design: "let's do 500 ms with varispeed
// approaching 5% as the number approaches 500ms". The fill half is what his
// walk found missing -- with multipoint connecting and the codec on AAC, the
// ring ran SHORT rather than long, fell from 1097 frames to 191 against a 720
// frame cushion, and took ten underruns. Nothing asked the producer to run
// ahead and refill, because the trim only ever sped it up.
//
// A ring that corrects in one direction only is half a controller: clock drift
// has two signs, and the one it does not cover underruns for ever.
#include "test_framework.h"
#include "dsp/ring_speed.h"

using namespace mdxm;

namespace {
const size_t kCap = 24000;    // 500 ms at 48 kHz
const size_t kTarget = 1200;  // a 25 ms cushion
} // namespace

MDXM_TEST_CASE(RingSpeed_AtTheTargetIsExactlyNormal) {
    CHECK(RingSpeed(kTarget, kTarget, kCap) == 1.0);
}

// ── too full: consume faster, give the latency back ──────────────────────
MDXM_TEST_CASE(RingSpeed_ReachesFivePercentFasterAtCapacity) {
    CHECK_NEAR(RingSpeed(kCap, kTarget, kCap), 1.05, 1e-9);
    CHECK_NEAR(RingSpeed(kCap * 2, kTarget, kCap), 1.05, 1e-9);   // clamped
}

MDXM_TEST_CASE(RingSpeed_ScalesWithTheBacklog) {
    const size_t half = kTarget + (kCap - kTarget) / 2;
    CHECK_NEAR(RingSpeed(half, kTarget, kCap), 1.025, 1e-3);
    // The case that runs constantly -- a few ms of drift -- must be a nudge,
    // not a pitch shift anyone could hear.
    CHECK(RingSpeed(kTarget + 480, kTarget, kCap) < 1.002);
}

// ── too empty: consume slower, let it refill ─────────────────────────────
//
// Speed below 1.0 means fewer input frames per output frame, so the producer
// puts MORE frames into the ring per second than the consumer takes, and the
// depth climbs back to the cushion.
MDXM_TEST_CASE(RingSpeed_SlowsDownWhenTheRingIsShort) {
    CHECK(RingSpeed(kTarget / 2, kTarget, kCap) < 1.0);
    CHECK(RingSpeed(0, kTarget, kCap) < 1.0);
    // Symmetric bound: five percent either way.
    CHECK_NEAR(RingSpeed(0, kTarget, kCap), 0.95, 1e-9);
}

MDXM_TEST_CASE(RingSpeed_ScalesWithTheDeficitToo) {
    // Half empty is half the correction.
    CHECK_NEAR(RingSpeed(kTarget / 2, kTarget, kCap), 0.975, 1e-3);

    // Just under the cushion is a correction small enough to be inaudible --
    // which is the actual requirement, rather than any particular number.
    const double small = RingSpeed(kTarget - 48, kTarget, kCap);   // 1 ms short
    CHECK(small < 1.0);
    CHECK(small > 0.995);        // under half a percent: nobody hears this
}

// THE TWO SIDES ARE DELIBERATELY NOT EQUALLY SENSITIVE, and it is worth
// pinning rather than discovering later and calling it a bug.
//
// A deficit is measured against the cushion; an excess against the much larger
// span from the cushion to capacity. So the same number of frames short earns a
// bigger correction than the same number long -- about nineteen times, at a 25
// ms cushion in a 500 ms ring. That is the right asymmetry: running short is
// audible as a gap within one callback, while running long is only latency, and
// latency can be given back at leisure.
MDXM_TEST_CASE(RingSpeed_ReactsHarderToShortThanToLong) {
    const size_t off = 480;      // 10 ms either way
    const double shortBy = 1.0 - RingSpeed(kTarget - off, kTarget, kCap);
    const double longBy = RingSpeed(kTarget + off, kTarget, kCap) - 1.0;
    CHECK(shortBy > longBy);
    CHECK(shortBy < 0.05);       // still bounded by the same maximum
    CHECK(longBy > 0.0);
}

// The real sequence from 2026-10-05: 1097 frames against a 720 cushion (drain),
// then starving at 191 (fill). The same function has to answer both, and the
// answers must have opposite signs.
MDXM_TEST_CASE(RingSpeed_HandlesTheSequenceThatActuallyHappened) {
    const size_t cushion15ms = 720;
    const double tooFull = RingSpeed(1097, cushion15ms, kCap);
    const double tooEmpty = RingSpeed(191, cushion15ms, kCap);
    CHECK(tooFull > 1.0);
    CHECK(tooEmpty < 1.0);
}

MDXM_TEST_CASE(RingSpeed_DegenerateInputsAreHarmless) {
    CHECK(RingSpeed(5000, 1000, 1000) == 1.0);   // capacity at the target
    CHECK(RingSpeed(5000, 2000, 1000) == 1.0);   // capacity below it
    CHECK(RingSpeed(0, 0, 0) == 1.0);
    CHECK(RingSpeed(0, 0, 24000) == 1.0);        // no target to aim at
}
