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
#include "dsp/gain_ramp.h"
#include "dsp/limiter.h"
#include "dsp/resampler.h"
#include "dsp/ring_buffer.h"
#include "ipc/stream_feed.h"
#include "engine/capture_stream.h"
#include "engine/failover_watcher.h"
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

struct ChannelRuntime {
    std::wstring id, name;
    std::atomic<bool> healthy{false};   // cable found and capture running
    std::atomic<bool> ready{false};     // resampler rates configured; capture cb may write
    std::wstring healthMsg;
    float pvol = 1.0f, svol = 1.0f;
    bool pmute = false, smute = false;
    bool eqEnabled = false;
    RingBuffer ring{48000 * 2};         // 2 s — headroom for bursty (Bluetooth) clock masters
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

    // Control surface (thread-safe; called from UI and IPC threads).
    // Channel id L"mic" addresses the mic chain (gain via SetVolume/Personal, EQ).
    bool SetVolume(const std::wstring& ch, Mix m, float vol01);
    bool SetMute(const std::wstring& ch, Mix m, bool mute);
    bool SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q);
    bool EnableEq(const std::wstring& ch, bool on);
    bool SetPersonalOutput(const std::wstring& endpointId);  // stops+restarts the personal render
    void TickFailover();         // control thread, ~1 s: drives the FailoverWatcher

    // Installed by the app layer; the engine owns no config. Called on Commit
    // with the new personal output device (id + name).
    void SetFailoverCommitCallback(std::function<void(const DeviceRef&)> cb);

    std::vector<ChannelState> GetChannelStates() const;
    DiagState GetDiag() const;
    bool PersonalOnFallback() const { return m_personalFallback.load(); }
    uint32_t MixRate() const { return m_mixRate; }

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

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<ChannelRuntime>> m_channels;
    // The mix thread must not observe a half-built graph: Start brings the
    // render up before the scratch buffers are sized and the channels pushed,
    // and a vector reallocation under MixPull's range-for would leave it
    // dereferencing moved-from null pointers. MixPull renders silence until
    // this is set, and it is cleared again before any rebuild.
    std::atomic<bool> m_graphReady{false};
    std::atomic<uint32_t> m_mixRate{48000};   // read by the stream render thread

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

    size_t m_maxMixPull = 0;                // mix (personal render) thread only
    size_t m_maxStreamPull = 0;             // streaming render thread only
    size_t m_maxMicPull = 0;                // mic render thread only
    size_t CushionFor(size_t maxPull) const {
        size_t adaptive = maxPull + maxPull / 2 + m_mixRate / 100;
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

    // Streaming mix out
    bool m_streamEnabled = false;
    RingBuffer m_streamRing{48000 * 2};
    RenderStream m_streamRender;
    LinearResampler m_streamResampler;      // stream render thread only
    std::vector<float> m_streamScratch;
    // Cleared by the control thread on a rate change so the stream thread
    // re-derives its resampler rates on the next cushion rebuild.
    std::atomic<bool> m_streamCushionDone{false};

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
