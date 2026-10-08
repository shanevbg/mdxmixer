#pragma once
// The mix graph, per the spec diagram:
//   channel cable capture -> resample-on-capture -> SPSC ring -> shared EQ ->
//   per-mix gain ramps -> personal sum (renders to the physical output, the clock
//   master pacing the whole graph) and streaming sum (-> stream ring -> streaming
//   cable render). Mic chain is independent: mic capture -> ring -> gain+EQ ->
//   mic cable render. Every stream callback body is try/catch (no-crash rule).
//
// Threading: the control surface takes m_mutex; the audio pull path reads only
// atomics and rings and takes no engine-wide lock. All device enumeration and
// stream restarts happen on the CONTROL thread (fj#401).
#include "config/config.h"
#include "device/endpoints.h"
#include "dsp/biquad.h"
#include "dsp/ring_speed.h"
#include "dsp/gain_ramp.h"
#include "dsp/limiter.h"
#include "dsp/peak_hold.h"
#include "dsp/pull_window.h"
#include "dsp/resampler.h"
#include "dsp/ring_buffer.h"
#include "ipc/stream_feed.h"
#include "engine/audiodg_watch.h"
#include "engine/capture_stream.h"
#include "engine/failover_watcher.h"
#include "engine/mix_demand.h"
#include "engine/render_stream.h"
#include "ipc/mixer_control.h"   // ChannelState, DiagState, Mix
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace mdxm {

// Pure, unit-tested: which endpoint should the personal mix render to?
// Prefer the bound device (id-first/name-fallback via MatchBinding, render
// endpoints only); if absent, the system default render endpoint id (when it is
// still in eps); if even that is gone, empty (the engine idles the render).
// `avoid` is the endpoints that must not be chosen because sending the mix
// there would feed it back into its own source. Pure, so the loop it prevents
// is covered by a test rather than by hearing it.
std::wstring PickPersonalOutput(const std::vector<EndpointInfo>& eps,
                                const DeviceRef& bound,
                                const std::wstring& systemDefaultId,
                                const std::vector<std::wstring>& avoid = {});

// How long the mix may go un-pulled before the render is presumed wedged.
//
// Three of the 1 s control ticks. The render device period is about 10 ms, so
// a live stream advances the frame count roughly a hundred times a second;
// three seconds of no movement is not a busy machine, it is a dead stream.
// Long enough that a graph rebuild (which holds m_mutex, so the watchdog waits
// on it rather than racing it) cannot be mistaken for one.
constexpr unsigned kWatchdogStuckMs = 3000;

// Should TickWatchdog restart the personal render?
//
// Pure, and separate from the tick, because the interesting part is the
// three-way condition and not the plumbing: "frames are not moving" alone is
// true of a machine with no output device at all, and of the moment between
// Stop() and Start().
//
//   framesMoved  the mix frame count changed since the last tick
//   haveRender   a personal render is SUPPOSED to be running right now
//   stuckMs      how long since the count last moved
bool WatchdogShouldRestart(bool framesMoved, bool haveRender, unsigned stuckMs);

struct ChannelRuntime {
    std::wstring id, name;
    std::atomic<bool> healthy{false};   // cable found and capture running
    std::atomic<bool> ready{false};     // resampler rates configured; capture cb may write
    std::wstring healthMsg;
    float pvol = 1.0f, svol = 1.0f;
    bool pmute = false, smute = false;
    bool eqEnabled = false;
    // Sized from the mix rate at capture start (fj#13): capacity is the
    // worst-case latency a stall can leave behind, so it is 500 ms rather than
    // the 2 s it used to be. Measured demand on this machine is 480-frame
    // pulls, largest ever 960.
    RingBuffer ring{48000 / 2};
    // How fast the producer should run relative to real time, to hold the ring
    // at its cushion: above 1.0 drains a backlog, below 1.0 refills a ring that
    // is running short. Written by the control thread, read by the capture
    // thread, which applies it to the resampler when it changes -- see
    // dsp/ring_speed.h and StartChannelCapture.
    std::atomic<double> speedTrim{1.0};
    double appliedSpeed = 1.0;          // capture thread only
    LinearResampler resampler;          // capture rate -> mix rate (capture thread only)
    std::vector<float> captureScratch;
    ChannelEq eq;
    // Kept so the EQ can be recomputed when the mix rate changes: biquad
    // coefficients are sample-rate dependent, and the band parameters live in
    // config, which the engine does not own.
    std::vector<EqBandConfig> eqBands;
    GainRamp personalGain, streamingGain;
    CaptureStream capture;
    bool cushionDone = false;           // mix thread only
    DeviceRef captureBinding;
    // What this channel's source is carrying, for anything that wants to find
    // the row making a noise. Measured on the mix thread off the channel's own
    // samples, before the two gains -- see ChannelState::peak for why before.
    //
    // `peakHold` is mix-thread-only state; `peakPub` is the one value a reader
    // on any thread may touch. A relaxed atomic float because a reader wants
    // the latest value and never a sequence of them: a torn read is impossible
    // for a 4-byte aligned load, and being one block stale is invisible at
    // 10 ms blocks.
    PeakHold peakHold;
    std::atomic<float> peakPub{ kPeakUnknown };
    // The same block's peak with no hold applied, for a meter being watched
    // rather than polled (ChannelState::peakNow). One extra store of a number
    // the mix thread has already computed; no extra work and no extra scan.
    std::atomic<float> peakNowPub{ kPeakUnknown };
    // Not capturing, because NOTHING IS PULLING THE MIX -- not because
    // anything is wrong (fj#10). Kept apart from `healthy` for exactly that
    // reason: an idle machine must not read as a faulty one. A channel in this
    // state has no stream open, an empty ring, and `peak` of kPeakUnknown,
    // which is the honest answer rather than a measurement of silence.
    std::atomic<bool> idle{false};
};

class Engine {
public:
    ~Engine();
    // Builds channels + mic chain from config against the live endpoint list.
    // Missing cable => that channel unhealthy (with a message), engine still
    // starts. Never throws. err collects degradation messages (may be non-empty
    // even on success).
    // Both of these build or rebuild the audio graph, and both are wrapped
    // in a REAL SEH guard rather than only a try/catch.
    //
    // Measured on 2026-10-03: against a graph that had survived two AUDIODG
    // kills and a Windows Audio service crash, mdxmixer died about three
    // seconds after launch inside AudioSes.dll -- 0xC0000005, then
    // 0xC000041D. catch(...) under /EHsc does not catch an access violation,
    // so the audio stack faulting underneath us took the whole process down,
    // which is exactly when a mixer is needed most. The guard turns that into
    // "the engine did not start" and leaves the window, the tray icon and the
    // rescue commands alive. MDropDX12 learned the same lesson as its #420.
    bool Start(const MixerConfig& cfg, std::wstring* err);
    void OnDeviceSetChanged();   // control thread: re-match bindings, recover/degrade
    void Stop();

    // Turn the shared-memory feed on or off. Control thread only.
    //
    // Creating the section is what "on" means: when nothing has asked, the
    // mapping does not exist and a reader looking for it finds nothing,
    // which is the honest answer rather than a ring of silence.
    bool SetFeedEnabled(bool on, std::wstring* err);
    bool FeedEnabled() const { return m_feedOn.load(std::memory_order_relaxed); }
    // Sample rate the feed publishes at, so a caller can report it without
    // mapping the section. 0 when the engine is not running.
    uint32_t FeedRate() const { return m_mixRate.load(); }

    // The VBAN sink (spec §3.1): while on, MixPull writes the selected
    // post-limiter sum into m_vbanRing for the sender thread to drain. Control
    // thread. The ring is cleared on enable and on a mix-rate change, so a
    // stream never opens on audio recorded before anyone was listening.
    //
    // Deliberately NOT keyed to config: the app layer turns this on when the
    // server reports a subscriber and off when the last one goes, so an enabled
    // listener with nobody connected costs no copy per block.
    void SetVbanSink(bool on, bool streamingSource);
    bool VbanSinkOn() const { return m_vbanOn.load(std::memory_order_relaxed); }
    RingBuffer& VbanRing() { return m_vbanRing; }

    // Control surface (thread-safe; called from UI and IPC threads).
    // Channel id L"mic" addresses the mic chain (gain via SetVolume/Personal, EQ).
    bool SetVolume(const std::wstring& ch, Mix m, float vol01);
    bool SetMute(const std::wstring& ch, Mix m, bool mute);
    bool SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q);
    bool EnableEq(const std::wstring& ch, bool on);
    bool SetPersonalOutput(const std::wstring& endpointId);  // stops+restarts the personal render
    void TickFailover();         // control thread, ~1 s: drives the FailoverWatcher
    // Control thread, ~1 s. Is the mix actually being pulled? Restarts the
    // personal render when it is not. See the definition for why this exists
    // on top of RenderStream::Dead() and the device notifications.
    void TickWatchdog();
    // The machine is going to sleep / has come back. Control thread.
    //
    // A resume is not a device change and does not arrive as one: across a
    // Modern Standby resume on 2026-10-04 the Bluetooth endpoint stayed
    // listed, stayed active, and kept answering its volume, while its render
    // event stopped firing for good. Nothing in the program could see that,
    // so there was no audio and no log line for seven hours.
    void OnSuspend();
    void OnResume();

    // Installed by the app layer; the engine owns no config. Called on Commit
    // with the new personal output device (id + name).
    void SetFailoverCommitCallback(std::function<void(const DeviceRef&)> cb);

    std::vector<ChannelState> GetChannelStates() const;
    DiagState GetDiag() const;
    // What the failover watcher is doing, for a front-end that shows the rule
    // (fj#2 §3). Everything here was already in the watcher and reachable
    // from nowhere outside this process.
    FailoverStatus GetFailoverStatus() const;
    // The cushion: its floor in ms, and the two adaptive terms. -1 for any of
    // them leaves that one alone. Setting any of them re-cushions the running
    // channels; see the definition.
    bool SetCushion(int ms, int headroomPercent, int flatMs);
    int  CushionMs() const;
    int  CushionHeadroomPercent() const;
    int  CushionFlatMs() const;
    bool PersonalOnFallback() const { return m_personalFallback.load(); }
    uint32_t MixRate() const { return m_mixRate; }
    // Frames the mix has pulled since the engine started. Monotonic, and the
    // only honest way to ask "is the mix actually running" without a clock on
    // the audio thread.
    //
    // It exists for the peak meters. MixPull is driven by the personal render
    // callback, so when there is no render device at all MixPull simply stops
    // being called -- and a held peak would then sit frozen at whatever was
    // playing when the device went away, presented as current. A stale peak is
    // worse than no peak: it points at the wrong row. A caller on the control
    // thread watches this for movement and reports kPeakUnknown when it stops.
    uint64_t MixFrames() const { return m_mixFrames.load(std::memory_order_relaxed); }

private:
    void MixPull(float* out, size_t frames);            // personal render callback
    bool StartImpl(const MixerConfig& cfg, std::wstring* err);
    void OnDeviceSetChangedImpl();
    std::vector<std::wstring> CaptureSources() const;
    // Endpoints that feed our capture indirectly -- a Sonar channel whose mix
    // reaches the cable we record. Filled at Start from the configuration.
    std::vector<std::wstring> m_loopSuspects;
    bool StartPersonalRender(const std::wstring& endpointId);
    // The personal render is the clock master, so moving it to a device with a
    // different mix rate changes the rate the whole graph consumes at. Every
    // per-channel rate-dependent thing has to follow, or captures keep producing
    // at the old rate while MixPull consumes at the new one and the rings drain
    // to permanent silence. Control thread, m_mutex held.
    void ReconfigureForMixRate();
    void EnsurePersonalRender(const std::vector<EndpointInfo>& eps);  // control thread
    bool StartChannelCapture(ChannelRuntime& ch, const std::vector<EndpointInfo>& eps);
    // Who is pulling the mix right now. Control thread, m_mutex held.
    MixDemand CurrentDemand() const;
    // Start or stop the channel captures to match that (fj#10). Control
    // thread, m_mutex held; takes the enumeration the caller already has.
    void MatchCapturesToDemand(const std::vector<EndpointInfo>& eps);

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<ChannelRuntime>> m_channels;
    // The mix thread must not observe a half-built graph: Start brings the
    // render up before the scratch buffers are sized and the channels pushed,
    // and a vector reallocation under MixPull's range-for would leave it
    // dereferencing moved-from null pointers. MixPull renders silence until
    // this is set, and it is cleared again before any rebuild.
    std::atomic<bool> m_graphReady{false};
    std::atomic<uint32_t> m_mixRate{48000};   // read by the stream render thread
    std::atomic<uint64_t> m_mixFrames{0};     // see MixFrames()
    // TickWatchdog's bookkeeping: the frame count it last saw move, and when.
    uint64_t m_lastWatchdogFrames = 0;
    unsigned m_watchdogMovedMs = 0;

    // Cushions adapt to the consumer: a wired device pulls ~10 ms and gets the
    // ~30 ms base (the spec's latency budget); a Bluetooth device pulls in big
    // bursts and the cushion grows to 1.5x the largest burst seen — matching the
    // latency the device itself already imposes. Each max is touched only by its
    // own audio thread.
    size_t m_baseCushion = 1440;            // ~30 ms at 48 kHz; recomputed at Start
    // The streaming mix, published to any other process that wants it -- the
    // visualiser, today. Opened with the graph and closed with it.
    StreamFeed m_feed;
    float m_feedGain = 1.0f;   // remembered while the feed is closed
    // Whether the shared-memory feed is PUBLISHING.
    //
    // Off unless something asked for it. The feed exists for the case where
    // MDropDX12 cannot get the audio any other way -- Sonar wedged, or not
    // installed -- and that is not the normal case any more: "the shared
    // ring is only useful when sonar isn't working or available, and now
    // that am using VB-Audio Virtual Cable and Sonar isn't trying to deal
    // with the bluetooth flapping it seems more stable, so should not run
    // the shared memory ring if not needed".
    //
    // Atomic because the audio thread reads it on every block while the
    // control thread flips it. Relaxed is enough: a block either side of the
    // switch is of no consequence, and the mapping itself is only ever
    // created or destroyed while this is false.
    std::atomic<bool> m_feedOn{ false };

    // ALL-TIME largest pull per path, kept for MDXM_DIAG only -- it is what a
    // person wants when asking "what is the worst this device has ever done".
    // It no longer sizes anything: see the windows below.
    size_t m_maxMixPull = 0;                // mix (personal render) thread only
    size_t m_maxStreamPull = 0;             // streaming render thread only
    size_t m_maxMicPull = 0;                // mic render thread only
    // The largest pull in the LAST MINUTE, which is what the cushion is sized
    // from (fj#12). A monotonic high-water mark meant one oversized pull --
    // a Bluetooth hiccup, a device change, a stall -- raised the latency for
    // the rest of the session with no way back down short of restarting.
    // Written by the audio threads, read by the control thread for diagnostics;
    // see the threading note in pull_window.h.
    PullWindow m_mixPullWindow, m_streamPullWindow, m_micPullWindow;
    static constexpr unsigned kPullWindowSeconds = 60;
    // The configured floor, in ms. Held so a rate change and a live set can
    // both recompute m_baseCushion from the same number.
    int m_cushionMs = 30;
    int m_cushionHeadroom = 50;   // percent of the pull, added as slack
    int m_cushionFlatMs = 10;     // fixed margin for scheduling jitter
    // Ring capacity, and therefore the worst latency a stall can leave behind
    // before the drain starts giving it back (fj#13).
    static constexpr size_t kRingMs = 500;
    // cushion = max( floor , pull + pull*headroom% + flat )
    //
    // The proportional term scales with the pull, so a render that starts
    // asking for bigger buffers gets proportionally more slack; the flat term
    // is for scheduling jitter, which does not scale with buffer size. Both
    // are configurable because the right values belong to the hardware and the
    // radio environment: measured here, a WF-1000XM5 pulls 480 frames and the
    // defaults put the cushion at 25 ms.
    size_t CushionFor(size_t maxPull) const {
        const size_t adaptive = maxPull + maxPull * (size_t)m_cushionHeadroom / 100 +
                                (size_t)m_mixRate * (size_t)m_cushionFlatMs / 1000;
        return adaptive > m_baseCushion ? adaptive : m_baseCushion;
    }

    std::vector<float> m_chanBuf, m_pBuf, m_sBuf, m_pSum, m_sSum;   // mix thread scratch
    SoftLimiter m_pLimiter, m_sLimiter;

    RenderStream m_personalRender;
    DeviceRef m_boundPersonal;              // currently configured personal output
    std::wstring m_currentPersonalId;       // endpoint the render actually runs on
    std::atomic<bool> m_personalFallback{false};
    FailoverWatcher m_failover;
    // One endpoint enumeration per failover tick, read by the watcher's
    // presence and name callbacks. Control thread only, under m_mutex.
    std::vector<EndpointInfo> m_epSnapshot;
    // The one route mdxmixer owns today. Named rather than spelled out at
    // each call site: the watcher keys everything on it.
    static constexpr const wchar_t* kPersonalRoute = L"personal";
    FailoverConfig m_failoverCfg;
    std::function<void(const DeviceRef&)> m_onFailoverCommit;
    // AUDIODG.EXE, watched by process id so the failover hold can be driven by
    // the thing that actually causes the false "every device is gone" reading
    // (fj#2 §4, ported from MDropDX12 #410). Control thread only, under
    // m_mutex, from the same tick as the failover decision.
    AudiodgWatch m_audiodg;
    // Backs off retrying a personal render that is wanted and will not start
    // (fj#11). Control thread only, under m_mutex.
    RenderRetry m_renderRetry;

    // Streaming mix out
    bool m_streamEnabled = false;
    RingBuffer m_streamRing{48000 * 2};
    RenderStream m_streamRender;
    LinearResampler m_streamResampler;      // stream render thread only
    std::vector<float> m_streamScratch;
    // Cleared by the control thread on a rate change so the stream thread
    // re-derives its resampler rates on the next cushion rebuild.
    std::atomic<bool> m_streamCushionDone{false};

    // VBAN sink: written by MixPull, drained by VbanServer's sender thread.
    //
    // Two seconds, and for a different reason than the ring-capacity lesson in
    // ring_buffer.h would suggest. Capacity there is worst-case latency because
    // the consumer is an audio clock; here the consumer is a pacer that refuses
    // to run ahead of real time, so what this capacity absorbs is the
    // PRODUCER'S burst -- a Bluetooth render pulls up to kMaxPullFrames at a
    // time, seconds ahead of what its own radio has played. The depth that
    // leaves behind is the producer's lead, not latency the sender can shed,
    // and it is published as depthms rather than silently corrected.
    RingBuffer m_vbanRing{48000 * 2};
    std::atomic<bool> m_vbanOn{false};
    std::atomic<bool> m_vbanStreamingSrc{false};   // false = personal (the default)

    // Mic chain
    bool m_micEnabled = false;
    CaptureStream m_micCapture;
    std::atomic<bool> m_micReady{false};
    LinearResampler m_micResampler;         // mic capture thread only
    std::vector<float> m_micCapScratch;
    RingBuffer m_micRing{48000 * 2};
    RenderStream m_micRender;
    GainRamp m_micGain;
    ChannelEq m_micEq;
    float m_micVol = 1.0f;
    bool m_micMute = false;
    bool m_micCushionDone = false;          // mic render thread only
};

} // namespace mdxm
