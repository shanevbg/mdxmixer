#pragma once
// When may the next AUDIO packet go out? (spec §3.2 step 3)
//
// The sender drains a ring that the mix thread fills, and the two are paced by
// different things: the mix is paced by the render device's callback, which on
// Bluetooth arrives in large irregular bursts, while the wire wants a steady
// ~188 packets a second. This is the thing in between.
//
// WHAT RING DEPTH IS NOT. Depth is the PRODUCER'S LEAD -- a Bluetooth render
// pulls seconds of audio ahead of what its own radio has played -- so depth is
// not latency this end can remove, and a pacer that "caught up" by skipping
// content would be dropping audio to fix a number. It never skips: it only
// refuses to run ahead of real time, and when it has fallen behind its own
// schedule it sends faster, bounded, until it is level again.
//
// PURE: the caller supplies the clock in microseconds (QueryPerformanceCounter
// on the sender thread), so the decision is testable without a clock and the
// audio thread never touches any of it.
#include <cstdint>
#include <cstddef>

namespace mdxm { namespace vban {

class Pacer {
public:
    void Configure(uint32_t sampleRate, size_t framesPerPacket, unsigned burstPackets) {
        m_usPerPacket = sampleRate
            ? (uint64_t)framesPerPacket * 1000000ull / sampleRate
            : 0;
        m_burst = burstPackets ? burstPackets : 1;
        m_anchored = false;
        m_next = 0;
    }

    // How many packets may be sent now. Advances the schedule by exactly what
    // it grants, so a caller that sends fewer than it was given falls behind by
    // the difference and is granted it back on the next call.
    unsigned Due(uint64_t nowUs) {
        if (m_usPerPacket == 0) return 0;
        // The first call sets the schedule's origin; there is nothing to be
        // late for yet, so it grants one packet and starts the clock.
        if (!m_anchored) {
            m_anchored = true;
            m_next = nowUs + m_usPerPacket;
            return 1;
        }
        if (nowUs < m_next) return 0;
        // Hopelessly behind: a wedged sendto, a lid closed, a machine resumed.
        // Granting the arithmetic answer here would be hundreds of packets in
        // one wake -- a burst no receiver's jitter buffer can hold, which
        // arrives as a dropout rather than as recovery. Re-anchor instead, and
        // COUNT it, because a stream that silently restarts its schedule is
        // indistinguishable from one that is merely working.
        if (nowUs - m_next > kMaxBehindUs) {
            ++m_reanchors;
            m_next = nowUs + m_usPerPacket;
            return m_burst;
        }
        unsigned due = (unsigned)((nowUs - m_next) / m_usPerPacket) + 1;
        if (due > m_burst) due = m_burst;
        m_next += (uint64_t)due * m_usPerPacket;
        return due;
    }

    // How far behind its own schedule the sender is, for MDXM_VBANSTATE: the
    // number that says "the network or this machine is not keeping up", as
    // against ring depth, which says nothing of the sort.
    int64_t BehindUs(uint64_t nowUs) const {
        if (!m_anchored || nowUs <= m_next) return 0;
        return (int64_t)(nowUs - m_next);
    }
    uint64_t Reanchors() const { return m_reanchors; }

private:
    static constexpr uint64_t kMaxBehindUs = 500000;   // 500 ms
    uint64_t m_usPerPacket = 0, m_next = 0, m_reanchors = 0;
    unsigned m_burst = 4;
    bool m_anchored = false;
};

}} // namespace mdxm::vban
