// test_audiodg_watch.cpp — the AUDIODG.EXE restart detector that drives the
// failover hold (fj#2 §4, MDropDX12 #410).
//
// The whole point of the component is that the decision is pure: the pid is
// handed in, so a restart, a disappearance and a recovery are all testable
// without killing the Windows audio engine on the machine this is built on.
#include "test_framework.h"
#include "engine/audiodg_watch.h"

using namespace mdxm;

MDXM_TEST_CASE(Audiodg_FirstSightingIsNotARestart) {
    AudiodgWatch w;
    // It is the state of the world at startup, not a change in it. Counting it
    // would hold failover off for a minute every time mdxmixer started.
    CHECK(w.Observe(1234, 1000) == AudiodgEvent::Appeared);
    CHECK(w.Pid() == 1234);
    CHECK(w.Restarts() == 0);
}

MDXM_TEST_CASE(Audiodg_ANewPidIsARestart) {
    AudiodgWatch w;
    w.Observe(1234, 1000);
    CHECK(w.Observe(5678, 3000) == AudiodgEvent::Restarted);
    CHECK(w.Restarts() == 1);
    CHECK(w.Pid() == 5678);
}

// The usual shape of a real outage: the process is gone for a while, then a
// different one appears. That is ONE restart, and the hold is armed when it
// goes -- which is the moment every endpoint starts reading absent.
MDXM_TEST_CASE(Audiodg_GoingAwayIsTheRestartAndComingBackIsNot) {
    AudiodgWatch w;
    w.Observe(1234, 1000);
    CHECK(w.Observe(0, 3000) == AudiodgEvent::Restarted);
    CHECK(w.Restarts() == 1);
    // Still absent: nothing new to say. The hold is already running and
    // re-arming it on every scan would turn a ceiling into a floor.
    CHECK(w.Observe(0, 5000) == AudiodgEvent::Nothing);
    CHECK(w.Restarts() == 1);
    // Back under a new pid. Worth a log line, but not a second hold.
    CHECK(w.Observe(9999, 7000) == AudiodgEvent::Appeared);
    CHECK(w.Restarts() == 1);
}

// The hold is a CEILING, not a duration: killed cleanly AUDIODG is back in
// under a second, and about a minute after Sonar's APO faults it. Both
// measured. So the caller is told when the engine has been back and unchanged
// long enough to trust, and releases then rather than serving out the ceiling.
MDXM_TEST_CASE(Audiodg_SteadyIsReportedOnlyAfterTheSettleTime) {
    AudiodgWatch w;
    w.Observe(1234, 1000);
    CHECK(w.Observe(1234, 2000) == AudiodgEvent::Nothing);          // 1 s in
    CHECK(w.Observe(1234, 5900) == AudiodgEvent::Nothing);          // 4.9 s in
    CHECK(w.Observe(1234, 6000) == AudiodgEvent::SteadyAgain);      // 5 s in
    // Reported while it stays steady. Releasing a hold that is not running is
    // the caller's no-op, and that is where the guard belongs: this object
    // does not know whether anyone is holding.
    CHECK(w.Observe(1234, 8000) == AudiodgEvent::SteadyAgain);
}

MDXM_TEST_CASE(Audiodg_AbsenceNeverCountsAsSteady) {
    AudiodgWatch w;
    w.Observe(1234, 1000);
    w.Observe(0, 2000);
    // Ten seconds of nothing there is not ten seconds of being settled, and
    // lifting the hold here is exactly the mistake the hold exists to prevent.
    CHECK(w.Observe(0, 12000) == AudiodgEvent::Nothing);
    CHECK(!w.Settled(12000));
}

// A toolhelp snapshot is not free and the thing being watched takes about a
// minute to come back, so the scan is throttled rather than run on every tick.
MDXM_TEST_CASE(Audiodg_ScanIsThrottled) {
    AudiodgWatch w;
    CHECK(w.ShouldScan(1000));          // never scanned: always due
    w.Observe(1234, 1000);
    CHECK(!w.ShouldScan(1500));
    CHECK(!w.ShouldScan(2999));
    CHECK(w.ShouldScan(3000));
}

// GetTickCount wraps every 49 days and this machine stays up for weeks. The
// arithmetic is unsigned throughout, which is correct across the wrap.
MDXM_TEST_CASE(Audiodg_SurvivesTheTickCountWrap) {
    AudiodgWatch w;
    const unsigned nearWrap = 0xFFFFF000u;
    w.Observe(1234, nearWrap);
    CHECK(!w.ShouldScan(nearWrap + 1000));
    CHECK(w.ShouldScan(nearWrap + 2000));          // wrapped past zero
    CHECK(w.Observe(1234, nearWrap + 6000) == AudiodgEvent::SteadyAgain);
}
