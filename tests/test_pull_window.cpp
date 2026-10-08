// test_pull_window.cpp — the rolling 60-second maximum pull size that sizes
// the cushion.
//
// The old high-water mark only ever went UP, and was reset only by an engine
// restart. One oversized pull -- a Bluetooth hiccup, a device change, a stall
// -- raised the cushion for the rest of the session and the latency never came
// back down. Shane: "let the high water mark decay using a 60 second window,
// decaying to the highest over 60 seconds + buffer."
//
// Time is counted in FRAMES, not milliseconds, because this is fed from the
// mix thread and the audio thread must never call a clock (dsp/peak_hold.h
// says the same thing for the same reason).
#include "test_framework.h"
#include "dsp/pull_window.h"

using namespace mdxm;

namespace {
// 48 kHz, so a second is 48000 frames and the window is 60 of them.
//
// Configured in place rather than returned by value: the buckets are atomics,
// which makes the type non-copyable -- correct for something the audio thread
// writes while the control thread reads it.
void Configure(PullWindow& w) { w.Configure(48000 * 60); }
} // namespace

MDXM_TEST_CASE(PullWindow_StartsAtZeroAndTakesTheFirstPull) {
    PullWindow w;
    Configure(w);
    CHECK(w.Max() == 0);
    w.Push(1024, 1024);
    CHECK(w.Max() == 1024);
}

MDXM_TEST_CASE(PullWindow_HoldsTheLargestSeenInsideTheWindow) {
    PullWindow w;
    Configure(w);
    w.Push(480, 480);
    w.Push(9600, 9600);     // the burst
    w.Push(480, 480);
    CHECK(w.Max() == 9600);
    // Thirty seconds of ordinary pulls later it is still in the window, so the
    // cushion must still cover it.
    for (int i = 0; i < 1000; ++i) w.Push(480, 48000 * 30 / 1000);
    CHECK(w.Max() == 9600);
}

// The point of the whole exercise: it comes back down.
MDXM_TEST_CASE(PullWindow_ForgetsABurstOnceItLeavesTheWindow) {
    PullWindow w;
    Configure(w);
    w.Push(9600, 9600);
    CHECK(w.Max() == 9600);
    // Sixty-one seconds of small pulls. The burst is older than the window and
    // must no longer be sizing anything.
    for (int i = 0; i < 61; ++i) w.Push(480, 48000);
    CHECK(w.Max() == 480);
}

// A burst that keeps happening keeps the cushion up -- the window forgets by
// age, never by a fixed count of pushes.
MDXM_TEST_CASE(PullWindow_ARecurringBurstKeepsTheCushionUp) {
    PullWindow w;
    Configure(w);
    for (int minute = 0; minute < 5; ++minute) {
        w.Push(9600, 9600);
        for (int i = 0; i < 30; ++i) w.Push(480, 48000 / 30);   // ~1 s of small pulls
    }
    CHECK(w.Max() == 9600);
}

// Decay is gradual rather than a cliff: with the window divided into buckets,
// an old maximum leaves when its bucket does, so the figure steps down instead
// of vanishing the instant 60 s elapses.
MDXM_TEST_CASE(PullWindow_DecaysInStepsNotAllAtOnce) {
    PullWindow w;
    Configure(w);
    w.Push(9600, 9600);                       // bucket 0
    for (int i = 0; i < 30; ++i) w.Push(4800, 48000);   // 30 s of medium pulls
    CHECK(w.Max() == 9600);                   // still inside the window
    for (int i = 0; i < 31; ++i) w.Push(480, 48000);    // the burst ages out
    CHECK(w.Max() == 4800);                   // the medium pulls are what is left
    for (int i = 0; i < 31; ++i) w.Push(480, 48000);    // they age out too
    CHECK(w.Max() == 480);
}

// A silent stretch -- no pulls at all -- still ages the window when pulls
// resume, rather than resuming with a stale maximum.
MDXM_TEST_CASE(PullWindow_TimeAdvancesOnTheFramesItIsGiven) {
    PullWindow w;
    Configure(w);
    w.Push(9600, 9600);
    w.Push(480, 48000 * 120);   // two minutes in one jump
    CHECK(w.Max() == 480);
}

MDXM_TEST_CASE(PullWindow_UnconfiguredIsHarmless) {
    PullWindow w;               // never Configure()d
    w.Push(4096, 4096);
    CHECK(w.Max() == 4096);     // still reports what it was told
}
