// test_mix_demand.cpp — the two decisions behind fj#10 and fj#11.
#include "test_framework.h"
#include "engine/mix_demand.h"

using namespace mdxm;

// The three consumers, named so the list cannot quietly shrink. Each one alone
// is enough to justify capturing: the personal render pulls the mix, the
// streaming render pulls it when a cable is bound, and the feed pulls it when
// a client has asked for it.
MDXM_TEST_CASE(MixDemand_AnyOneConsumerIsEnough) {
    CHECK(!AnyoneListening(MixDemand{}));
    CHECK(AnyoneListening(MixDemand{ true, false, false }));
    CHECK(AnyoneListening(MixDemand{ false, true, false }));
    CHECK(AnyoneListening(MixDemand{ false, false, true }));
    CHECK(AnyoneListening(MixDemand{ true, true, true }));
}

// The state this is all about: nothing is pulling, so nothing should be
// captured. Measured on 2026-10-04 as depth=96000, drops=41401440,
// underruns=0 -- fourteen minutes of audio captured and discarded.
MDXM_TEST_CASE(MixDemand_NobodyListeningIsTheIdleCase) {
    MixDemand d;
    d.personalRender = false;   // the render had failed to start
    d.streamingRender = false;  // no streaming cable bound
    d.feed = false;             // no client had asked for the feed
    CHECK(!AnyoneListening(d));
}

// The VBAN sink is the fourth consumer and the ONLY one AnyoneListening
// ignores, which looks like an oversight and is the whole point (spec §3.1).
// MixPull runs on the personal render's callback, so with no render device
// there is no mix at all -- and a demand bit that started captures in that
// state would fill rings nobody drains, which is fj#10 exactly. The field is
// published for diagnostics; emission is gated in the sender instead. If the
// timer-paced fallback clock is ever built, this test is what has to change.
MDXM_TEST_CASE(MixDemand_VbanIsReportingOnly) {
    MixDemand d;
    d.vban = true;
    CHECK(!AnyoneListening(d));
    d.personalRender = true;
    CHECK(AnyoneListening(d));
}

MDXM_TEST_CASE(RenderRetry_FirstAttemptIsImmediate) {
    RenderRetry r;
    // A render that is wanted and not running is tried at once; waiting a
    // second to make the first attempt would add a second of silence to every
    // ordinary device change.
    CHECK(r.Due(1000));
    CHECK(r.Attempts() == 0);
}

MDXM_TEST_CASE(RenderRetry_BacksOffByDoubling) {
    RenderRetry r;
    r.Failed(1000);
    CHECK(r.IntervalMs() == 1000);
    CHECK(!r.Due(1500));
    CHECK(r.Due(2000));

    r.Failed(2000);
    CHECK(r.IntervalMs() == 2000);
    CHECK(!r.Due(3500));
    CHECK(r.Due(4000));

    r.Failed(4000);
    CHECK(r.IntervalMs() == 4000);
    CHECK(r.Due(8000));
}

// A device that is never going to open must not cost a COM round trip and a
// log line every second for the life of the process.
MDXM_TEST_CASE(RenderRetry_IntervalIsCapped) {
    RenderRetry r;
    for (int i = 0; i < 50; ++i) r.Failed(1000u * (unsigned)i);
    CHECK(r.IntervalMs() == 60000);
    // And the counter does not run away over a long uptime.
    CHECK(r.Attempts() <= 1000);
}

MDXM_TEST_CASE(RenderRetry_SuccessClearsTheBackOff) {
    RenderRetry r;
    r.Failed(1000);
    r.Failed(2000);
    r.Failed(4000);
    CHECK(r.Attempts() == 3);
    r.Reset();
    CHECK(r.Attempts() == 0);
    // The next genuine need is tried at once rather than inheriting a back-off
    // earned by a device that is no longer the one being asked for.
    CHECK(r.Due(4001));
}

// GetTickCount wraps every 49 days and this machine stays up for weeks; the
// arithmetic is unsigned throughout, which is correct across the wrap.
MDXM_TEST_CASE(RenderRetry_SurvivesTheTickCountWrap) {
    RenderRetry r;
    const unsigned nearWrap = 0xFFFFFF00u;
    r.Failed(nearWrap);
    CHECK(!r.Due(nearWrap + 500));
    CHECK(r.Due(nearWrap + 1000));   // wrapped past zero
}
