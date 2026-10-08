// mix_demand.h — is anything consuming the mix, and when should a render that
// would not start be tried again?
//
// Two small decisions, kept pure and out of Engine so they can be tested
// without an audio device, and kept together because they are the two halves
// of one question: whether the graph should be running at all.
//
// fj#10: a channel capture used to run whether or not anything was pulling the
// mix. With no personal render device it filled a ring nobody drained --
// measured at 41.4 million dropped frames, depth at full capacity, zero
// underruns, for fourteen minutes. Shane's framing: "mdxmixer is only
// publishing audio on demand -- if there is no demand for it to publish (and
// there wasn't) it shouldn't run the ring." The rule was already in the
// project for the shared-memory feed, which stays off until a client asks; it
// simply was not applied to capture.
//
// fj#11: a personal render that failed to START was never retried, because
// every repair path keys on a stream that EXISTED -- the device going away,
// coming back, or the stream reading Dead(). A stream that never started is
// none of those, so one failed start meant silence until something else
// happened to shake the engine.
#pragma once

namespace mdxm {

// Who is pulling the mix. All three are real consumers and any one of them is
// enough: the personal render (which is also the clock -- MixPull runs on its
// callback), the streaming render when a streaming cable is bound, and the
// shared-memory feed when a client has turned it on.
struct MixDemand {
    bool personalRender = false;
    bool streamingRender = false;
    bool feed = false;
    // REPORTING ONLY, and deliberately absent from AnyoneListening below.
    //
    // The VBAN sender is clocked by MixPull exactly as the feed is, and MixPull
    // runs only on the personal render's callback -- so in the one case where
    // this bit could change the answer (a phone listening with no render
    // device) there is no mix to capture FOR, and starting the captures would
    // fill rings nobody drains. That is fj#10 above, re-opened from a different
    // direction.
    //
    // Emission is gated in the sender instead (vban::VbanWanted). This field
    // exists so MDXM_VBANSTATE and MDXM_DIAG can say the sink is on. If the
    // timer-paced fallback clock in the spec's future work is ever built, THEN
    // this becomes a real term here and not before.
    bool vban = false;
};

inline bool AnyoneListening(const MixDemand& d) {
    return d.personalRender || d.streamingRender || d.feed;   // NOT d.vban -- see above
}

// When to try starting a personal render that is wanted and is not running.
//
// Backed off, because the reason a device will not open is often not going to
// change in the next second -- it is being installed, it is held exclusively,
// it is a Bluetooth headset mid-handshake. A fixed one-second retry would
// mean a COM round trip and a log line every second for hours. The doubling is
// the shape the failover watcher already uses for the same reason: the first
// attempt is immediate, and a device that never comes back costs a handful of
// attempts rather than a permanent spin.
class RenderRetry {
public:
    // Is an attempt due? True immediately for the first one after a success or
    // a reset, then on the backed-off interval.
    bool Due(unsigned nowMs) const {
        if (m_attempts == 0) return true;
        return (nowMs - m_lastTryMs) >= IntervalMs();
    }

    void Failed(unsigned nowMs) {
        m_lastTryMs = nowMs;
        if (m_attempts < kMaxAttemptsTracked) ++m_attempts;
    }

    // A start that worked, or a state where none is wanted any more: the next
    // genuine need gets an immediate attempt rather than inheriting a back-off
    // earned by a device that is no longer the one being asked for.
    void Reset() {
        m_attempts = 0;
        m_lastTryMs = 0;
    }

    unsigned Attempts() const { return m_attempts; }

    // 1 s, 2, 4, 8, 16, 32, then 60 for ever after. The shift is bounded for
    // its own sake -- a 64-bit uptime would otherwise shift a 32-bit value off
    // the end long before the clamp below ever saw it.
    unsigned IntervalMs() const {
        if (m_attempts == 0) return 0;
        const unsigned shift = m_attempts - 1 > 6 ? 6 : m_attempts - 1;
        const unsigned ms = 1000u << shift;
        return ms > kMaxIntervalMs ? kMaxIntervalMs : ms;
    }

private:
    static const unsigned kMaxIntervalMs = 60000;
    // Stops the counter growing without bound over a long uptime; the interval
    // is capped well before this anyway.
    static const unsigned kMaxAttemptsTracked = 1000;
    unsigned m_attempts = 0;
    unsigned m_lastTryMs = 0;
};

} // namespace mdxm
