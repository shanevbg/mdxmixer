// test_meter_ballistics.cpp — what a meter does on its way DOWN.
//
// The meters mdxmixer draws carried the 1.5 s peak hold that the wire needs,
// so a row stayed lit long after its sound stopped and then dropped in one
// step. Shane: "can we make the system a little more responsive with the sonar
// muting and meters or is the delay unavoidable". The hold is not negotiable
// over the wire; what is drawn is a separate decision, and this is it.
#include "test_framework.h"
#include "dsp/meter_ballistics.h"

using namespace mdxm;

MDXM_TEST_CASE(MeterFall_AttackIsInstant) {
    // The transient is the event. A meter that eases UP has missed it.
    CHECK(MeterFall(0.0f, 0.9f, 100) == 0.9f);
    CHECK(MeterFall(0.2f, 1.0f, 1) == 1.0f);
    CHECK(MeterFall(0.5f, 0.5f, 100) == 0.5f);   // steady: unchanged
}

MDXM_TEST_CASE(MeterFall_ReleaseIsBoundedByTime) {
    // One 100 ms frame cannot empty the bar, however loud it was.
    const float after = MeterFall(1.0f, 0.0f, 100);
    CHECK(after < 1.0f);
    CHECK(after > 0.0f);
    // Twice the time, twice the fall.
    const float twice = MeterFall(1.0f, 0.0f, 200);
    CHECK(twice < after);
}

MDXM_TEST_CASE(MeterFall_ReachesTheReadingAndStops) {
    float v = 1.0f;
    for (int i = 0; i < 20; ++i) v = MeterFall(v, 0.25f, 100);
    CHECK(v == 0.25f);        // settles ON the reading, never below it
}

MDXM_TEST_CASE(MeterFall_EmptiesInAboutAThirdOfASecond) {
    // The requirement is "a fall, not a jump and not a crawl", stated in
    // frames of the 100 ms refresh because that is what the eye gets: more
    // than one, so it reads as a fall; few enough that the bar is describing
    // now rather than a moment ago. 650 ms was the first answer and Shane
    // called it sluggish.
    float v = 1.0f;
    int frames = 0;
    while (v > 0.0f && frames < 1000) { v = MeterFall(v, 0.0f, 100); ++frames; }
    CHECK(v == 0.0f);
    CHECK(frames >= 2);
    CHECK(frames <= 5);
}

// kPeakUnknown is not silence, and the difference is the point of having it:
// an endpoint that cannot be metered must draw NOTHING, not an empty bar that
// claims the device is quiet.
MDXM_TEST_CASE(MeterFall_UnknownPassesStraightThrough) {
    CHECK(MeterFall(0.8f, kPeakUnknown, 100) == kPeakUnknown);
    CHECK(MeterFall(kPeakUnknown, kPeakUnknown, 100) == kPeakUnknown);
}

MDXM_TEST_CASE(MeterFall_FirstReadingAfterUnknownIsTakenWhole) {
    // Coming back from "cannot read" must not crawl up from nowhere.
    CHECK(MeterFall(kPeakUnknown, 0.6f, 100) == 0.6f);
}

MDXM_TEST_CASE(MeterFall_ZeroElapsedChangesNothing) {
    // Two redraws in the same millisecond -- a resize, a theme change -- must
    // not advance the fall, or a busy window drains the meters.
    CHECK(MeterFall(0.7f, 0.0f, 0) == 0.7f);
}
