#pragma once
// pull_window.h — the largest render pull seen in the last minute.
//
// WHAT IT IS FOR. The mix cushion is sized from the biggest buffer the render
// has asked for, because a pull bigger than the cushion is an underrun. That
// figure used to be a plain high-water mark that only ever rose and was reset
// only by restarting the engine, so a single oversized pull -- one Bluetooth
// hiccup, one device change, one stall -- raised the latency for the rest of
// the session and it never came back down. Shane asked for the obvious fix:
// "let the high water mark decay using a 60 second window, decaying to the
// highest over 60 seconds + buffer".
//
// TIME IS COUNTED IN FRAMES. This is fed from the mix thread, and the audio
// thread must never call a clock -- the same rule dsp/peak_hold.h states, for
// the same reason. The caller already knows how many frames it just moved, so
// that is the tick.
//
// HOW IT FORGETS. The window is divided into buckets, each holding the largest
// pull seen while it was current; the answer is the largest across all of them.
// An old maximum therefore leaves when its bucket is recycled, so the figure
// steps down over the window rather than falling off a cliff at exactly sixty
// seconds -- and a burst that keeps happening keeps the cushion up, because
// ageing is by elapsed frames and never by a count of pushes.
//
// Everything is integers and there is no allocation after Configure, so it is
// safe to call from the audio thread.
//
// THREADING. Push() is the mix thread's alone; Max() is read from the control
// thread for the cushion's diagnostics while that is happening. The buckets are
// therefore relaxed atomics -- a reader wants the latest figure and never a
// consistent sequence of them, and being one bucket stale is invisible in a
// number that describes the last minute. The cursor and the part-bucket count
// are writer-only and stay plain.
#include <atomic>
#include <cstddef>

namespace mdxm {

class PullWindow {
public:
    // `windowFrames` is the whole window -- 60 s at the mix rate. Keeping the
    // caller's units means this object needs to know nothing about rates.
    void Configure(size_t windowFrames) {
        m_bucketFrames = windowFrames / kBuckets;
        if (m_bucketFrames == 0) m_bucketFrames = 1;
        for (size_t i = 0; i < kBuckets; ++i)
            m_bucket[i].store(0, std::memory_order_relaxed);
        m_cursor = 0;
        m_inBucket = 0;
    }

    // One pull of `pullFrames`, `elapsedFrames` after the last one. The two are
    // usually the same number; they are separate so a caller that pulls in
    // bursts, or not at all for a while, still ages the window correctly.
    void Push(size_t pullFrames, size_t elapsedFrames) {
        Advance(elapsedFrames);
        if (pullFrames > m_bucket[m_cursor].load(std::memory_order_relaxed))
            m_bucket[m_cursor].store(pullFrames, std::memory_order_relaxed);
    }

    // The largest pull anywhere in the window.
    size_t Max() const {
        size_t best = 0;
        for (size_t i = 0; i < kBuckets; ++i) {
            const size_t v = m_bucket[i].load(std::memory_order_relaxed);
            if (v > best) best = v;
        }
        return best;
    }

private:
    // Twelve buckets of five seconds each, at the default window. Enough that
    // the decay reads as gradual, few enough that Max() is a trivial scan.
    static const size_t kBuckets = 12;

    void Advance(size_t elapsedFrames) {
        if (m_bucketFrames == 0) return;          // never configured: one bucket, no ageing
        m_inBucket += elapsedFrames;
        // A long silence can skip the whole window; clearing more buckets than
        // there are would just be clearing the same ones twice.
        size_t steps = m_inBucket / m_bucketFrames;
        m_inBucket %= m_bucketFrames;
        if (steps > kBuckets) steps = kBuckets;
        for (size_t i = 0; i < steps; ++i) {
            m_cursor = (m_cursor + 1) % kBuckets;
            // Recycled: its old maximum is forgotten.
            m_bucket[m_cursor].store(0, std::memory_order_relaxed);
        }
    }

    std::atomic<size_t> m_bucket[kBuckets] = {};
    size_t m_bucketFrames = 0;
    size_t m_cursor = 0;
    size_t m_inBucket = 0;
};

} // namespace mdxm
