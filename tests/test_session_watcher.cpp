// test_session_watcher.cpp — the IAudioSessionNotification registration that
// makes a stored app assignment true when the app next plays (fj#1).
//
// This is a LIVE test, like Policy_InitWithoutPriorComInit: it registers with
// the real audio stack on this machine. It cannot assert that a notification
// arrives -- that needs an application to start playing, which a test must not
// do on a machine whose owner is listening -- so what it covers is the part
// that goes wrong silently: registering, unregistering and releasing in the
// right order, on the right thread, repeatedly, without leaking or hanging.
//
// The DECISION this watcher triggers is covered by test_reconcile.cpp, which
// already tests the "app that was shut now appears" case it exists for.
#include "test_framework.h"
#include "routing/session_watcher.h"
#include "device/endpoints.h"
#include <atomic>
#include <cstdio>

using namespace mdxm;

MDXM_TEST_CASE(SessionWatcher_IsRegisteredByTheTimeStartReturns) {
    // How many endpoints there are to register on, asked independently.
    int activeRenders = 0;
    for (const auto& e : EnumerateEndpoints())
        if (e.isRender && e.isActive) ++activeRenders;

    std::atomic<int> signals{0};
    SessionWatcher w;
    CHECK(w.Start([&signals] { ++signals; }));
    std::printf("note: %d active render endpoint(s), watcher registered on %d\n",
                activeRenders, w.Registered());
    // Start RETURNS REGISTERED, which is the whole point of waiting for the
    // first pass: otherwise "the watcher is up" would be a claim about a
    // thread that has not reached the audio stack yet, and the one app that
    // started playing in that window would be missed silently. A machine with
    // no active render endpoint registers on nothing, legitimately.
    if (activeRenders > 0) CHECK(w.Registered() > 0);

    w.Stop();
    // Stop must be idempotent: the destructor calls it too, and so does Start.
    w.Stop();
    CHECK(w.Registered() == 0);
}

MDXM_TEST_CASE(SessionWatcher_RestartsAndRebinds) {
    SessionWatcher w;
    CHECK(w.Start([] {}));
    // A second Start replaces the first rather than stacking a second thread
    // and a second set of registrations on the same endpoints.
    CHECK(w.Start([] {}));
    // What a device change does: the endpoints have moved, so the old
    // registrations are dropped and new ones taken. It must be safe to ask
    // for that at any time, including before the first registration has
    // finished, and it must not block the caller.
    w.Rebind();
    w.Rebind();
    w.Stop();
}

MDXM_TEST_CASE(SessionWatcher_RebindAndStopAreSafeWithoutStart) {
    // The app layer calls Rebind on every debounced device change, whether or
    // not the watcher ever came up -- a machine where the registration failed
    // must not crash on the first headset that connects.
    SessionWatcher w;
    w.Rebind();
    w.Stop();
    CHECK(w.Registered() == 0);
}
