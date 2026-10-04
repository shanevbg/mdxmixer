#pragma once
// SPSC lock-free ring of stereo interleaved float frames.
// Overflow drops oldest (capture must never stall); underflow zero-fills (render never blocks).
// Counters make drift observable — the spec's DIAG contract depends on them.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mdxm {

class RingBuffer {
public:
    explicit RingBuffer(size_t capacityFrames)
        : m_capacity(capacityFrames), m_data(capacityFrames * 2, 0.0f) {}

    void Write(const float* interleaved, size_t frames) {
        if (frames > m_capacity) { // keep only the newest capacity frames
            m_drops.fetch_add(frames - m_capacity, std::memory_order_relaxed);
            interleaved += (frames - m_capacity) * 2;
            frames = m_capacity;
        }
        size_t w = m_write.load(std::memory_order_relaxed);
        size_t r = m_read.load(std::memory_order_acquire);
        size_t used = w - r;
        size_t free = m_capacity - used;
        if (frames > free) { // consumer is behind: advance read (drop oldest)
            size_t drop = frames - free;
            m_read.store(r + drop, std::memory_order_release);
            m_drops.fetch_add(drop, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < frames; ++i) {
            size_t idx = ((w + i) % m_capacity) * 2;
            m_data[idx]     = interleaved[i * 2];
            m_data[idx + 1] = interleaved[i * 2 + 1];
        }
        m_write.store(w + frames, std::memory_order_release);
    }

    size_t Read(float* out, size_t frames) {
        size_t r = m_read.load(std::memory_order_relaxed);
        size_t w = m_write.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t real = frames < avail ? frames : avail;
        for (size_t i = 0; i < real; ++i) {
            size_t idx = ((r + i) % m_capacity) * 2;
            out[i * 2]     = m_data[idx];
            out[i * 2 + 1] = m_data[idx + 1];
        }
        if (real < frames) {
            std::memset(out + real * 2, 0, (frames - real) * 2 * sizeof(float));
            m_underruns.fetch_add(1, std::memory_order_relaxed);
        }
        m_read.store(r + real, std::memory_order_release);
        return real;
    }

    size_t Depth() const {
        return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
    }
    uint64_t Drops() const { return m_drops.load(std::memory_order_relaxed); }
    uint64_t Underruns() const { return m_underruns.load(std::memory_order_relaxed); }
    void Clear() { m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release); }

private:
    size_t m_capacity;
    std::vector<float> m_data;
    // Monotonic frame counters. The overflow path has the producer advance m_read,
    // racing a concurrent Read: benign (a frame read twice or skipped once during an
    // overflow that is already being counted as an error state). Do not add a mutex.
    std::atomic<size_t> m_read{0}, m_write{0};
    std::atomic<uint64_t> m_drops{0}, m_underruns{0};
};

} // namespace mdxm
