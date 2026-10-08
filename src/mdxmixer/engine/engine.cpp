#include "engine.h"
#include "app/log.h"
#include "app/thread_guard.h"
#include "device/device_info.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

namespace mdxm {

namespace {
constexpr size_t kMaxPullFrames = 96000;   // 2 s at 48 kHz — Bluetooth renders pull in very large bursts
}

std::wstring PickPersonalOutput(const std::vector<EndpointInfo>& eps,
                                const DeviceRef& bound,
                                const std::wstring& systemDefaultId,
                                const std::vector<std::wstring>& avoid) {
    auto avoided = [&avoid](const std::wstring& id) {
        for (const auto& a : avoid)
            if (!a.empty() && _wcsicmp(a.c_str(), id.c_str()) == 0) return true;
        return false;
    };
    std::vector<EndpointInfo> renders;
    for (const auto& e : eps)
        if (e.isRender) renders.push_back(e);
    if (const EndpointInfo* m = MatchBinding(renders, bound))
        if (!avoided(m->id)) return m->id;
    // The system default, UNLESS falling back to it would feed the mix into
    // its own source.
    //
    // This is not hypothetical on the target machine: the default output is a
    // SteelSeries Sonar channel, Sonar's own output goes into the cable, and
    // mdxmixer captures that cable. Rendering the personal mix to the default
    // would close that circle -- cable -> mixer -> Sonar -> cable -- at
    // whatever level the headset is sitting at. Silence is the better
    // failure.
    if (!systemDefaultId.empty() && !avoided(systemDefaultId))
        for (const auto& e : renders)
            if (e.id == systemDefaultId) return systemDefaultId;
    return L"";
}

Engine::~Engine() { Stop(); }

// ── The SEH guards ───────────────────────────────────────────────────────
//
// RunGuarded takes a plain function pointer and owns nothing with a
// destructor, because MSVC forbids __try/__except in a function that needs
// C++ unwinding. Hence the context structs rather than lambdas.
namespace {
struct StartCtx { Engine* self; const MixerConfig* cfg; std::wstring* err; bool ok; };
struct DevCtx   { Engine* self; };
} // namespace

bool Engine::Start(const MixerConfig& cfg, std::wstring* err) {
    StartCtx ctx{ this, &cfg, err, false };
    if (!RunGuarded([](void* p) {
            StartCtx* c = (StartCtx*)p;
            c->ok = c->self->StartImpl(*c->cfg, c->err);
        }, &ctx)) {
        // The audio stack faulted while the graph was being built. Say so and
        // leave the rest of the application standing: the window, the tray,
        // and the routing commands all work without an engine, and on a
        // machine whose audio is broken those are the useful parts.
        Log(1, L"engine: the audio stack faulted while starting; running without it");
        if (err) *err = L"the audio stack faulted while the graph was being built";
        Stop();
        return false;
    }
    return ctx.ok;
}

void Engine::OnDeviceSetChanged() {
    DevCtx ctx{ this };
    if (!RunGuarded([](void* p) { ((DevCtx*)p)->self->OnDeviceSetChangedImpl(); }, &ctx))
        Log(1, L"engine: the audio stack faulted while rebinding devices");
}

bool Engine::StartImpl(const MixerConfig& cfg, std::wstring* err) {
    try {
        Stop();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_graphReady.store(false, std::memory_order_release);
        std::wstring notes;
        auto eps = EnumerateEndpoints();

        // ── Personal render: the clock master ─────────────────────────────
        // The "use failover list" sentinel is not an endpoint: it means
        // nothing is bound, which is the state the watcher acts on. Binding
        // it would have the engine hunting for a device called
        // "\x01followFailover" on every tick and never finding one.
        m_boundPersonal = (cfg.personalOutput.id == kFollowFailover)
                              ? DeviceRef{} : cfg.personalOutput;
        // Which endpoints would close a loop if the mix were sent to them.
        // A render endpoint with no ContainerId has no physical device behind
        // it: it belongs to another program, and on this machine those are
        // Sonar's channels, whose mix comes back out of the cable we capture.
        m_loopSuspects.clear();
        for (const auto& e : EnumerateEndpoints()) {
            if (!e.isRender || !e.isActive) continue;
            if (EndpointContainerId(e.id).empty()) m_loopSuspects.push_back(e.id);
        }

        m_failoverCfg = cfg.personalFailover;
        // Clock and presence are injected so the whole machine is testable
        // without a device; here they are the real ones. Presence reads the
        // snapshot TickFailover takes, so one enumeration serves every
        // allowlist entry on that tick.
        m_failover.SetClock([] { return (unsigned)GetTickCount64(); });
        m_failover.SetPresence([this](const std::wstring& id) {
            for (const auto& e : m_epSnapshot)
                if (e.isActive && e.id == id) return true;
            return false;
        });
        // Name -> the id currently carrying it. That direction is the
        // Bluetooth fallback: a re-paired headset comes back under a new
        // endpoint id with the same name, and an id-only allowlist silently
        // stops matching it. Getting this backwards makes the re-pair case
        // dead without any sign of it.
        m_failover.SetNames([this](const std::wstring& name) -> std::wstring {
            for (const auto& e : m_epSnapshot)
                if (e.isActive && e.isRender && _wcsicmp(e.name.c_str(), name.c_str()) == 0)
                    return e.id;
            return std::wstring();
        });
        std::wstring target = PickPersonalOutput(eps, m_boundPersonal,
                                                 DefaultRenderEndpointId(), CaptureSources());
        if (!target.empty()) {
            if (StartPersonalRender(target)) {
                const EndpointInfo* bound = MatchBinding(eps, m_boundPersonal);
                m_personalFallback = !(bound && bound->id == target);
            } else {
                notes += L"personal render failed to start; ";
            }
        } else {
            notes += L"no render device available for the personal mix; ";
        }
        // ALL THREE, not just the floor. The two adaptive terms are what
        // actually govern the latency once the floor is out of the way, and
        // loading only the floor meant a value found by experiment was written
        // to config, honoured until the next restart, and then silently
        // replaced by the defaults -- which read as the setting not working.
        m_cushionMs = cfg.cushionMs;
        m_cushionHeadroom = cfg.cushionHeadroomPercent;
        m_cushionFlatMs = cfg.cushionFlatMs;
        m_baseCushion = m_mixRate * (uint32_t)m_cushionMs / 1000;   // the FLOOR; adapts upward per consumer
        m_maxMixPull = m_maxStreamPull = m_maxMicPull = 0;
        // Sixty seconds at the mix rate, which is what the cushion now adapts
        // over instead of over the life of the process (fj#12).
        m_mixPullWindow.Configure(m_mixRate * kPullWindowSeconds);
        m_streamPullWindow.Configure(m_mixRate * kPullWindowSeconds);
        m_micPullWindow.Configure(m_mixRate * kPullWindowSeconds);

        // Mix scratch, sized once — no allocation on the audio thread. The
        // render thread is already running at this point; m_graphReady (still
        // false) is what keeps MixPull off these buffers until they exist.
        // Two seconds of ring at the mix rate: generous, because the reader
        // is a visualiser that can hitch for a frame or two and must come back
        // to recent audio rather than a lap-old buffer.
        // NOT opened here. The section is created when a client asks for
        // it (SetFeedEnabled, reached by MDXM_FEED over the pipe), so a
        // machine where nothing reads the feed never carries the mapping
        // nor the per-block copy into it.
        //
        // The gain is remembered either way, so turning the feed on later
        // picks up the configured percentage without being told again.
        m_feedGain = (float)cfg.mdx12FeedPercent / 100.0f;

        m_chanBuf.assign(kMaxPullFrames * 2, 0.0f);
        m_pBuf.assign(kMaxPullFrames * 2, 0.0f);
        m_sBuf.assign(kMaxPullFrames * 2, 0.0f);
        m_pSum.assign(kMaxPullFrames * 2, 0.0f);
        m_sSum.assign(kMaxPullFrames * 2, 0.0f);
        m_streamScratch.assign(kMaxPullFrames * 2, 0.0f);
        m_micCapScratch.assign(kMaxPullFrames * 2, 0.0f);

        // ── Channels ──────────────────────────────────────────────────────
        // Built with m_mixRate already known (the render set it above), so no
        // channel is ever configured against a rate it will not run at.
        for (const auto& cc : cfg.channels) {
            auto ch = std::make_unique<ChannelRuntime>();
            ch->id = cc.id;
            ch->name = cc.name.empty() ? cc.id : cc.name;
            ch->captureBinding = cc.cable.capture;
            ch->pvol = cc.personal.vol;   ch->pmute = cc.personal.mute;
            ch->svol = cc.streaming.vol;  ch->smute = cc.streaming.mute;
            ch->eqEnabled = cc.eq.enabled;
            ch->eq.Configure((double)m_mixRate);
            for (size_t b = 0; b < cc.eq.bands.size() && b < ChannelEq::kBands; ++b) {
                ch->eq.SetBand(b, cc.eq.bands[b].freq, cc.eq.bands[b].gainDb, cc.eq.bands[b].q);
                ch->eqBands.push_back(cc.eq.bands[b]);
            }
            ch->eq.SetEnabled(cc.eq.enabled);
            ch->personalGain.Configure((double)m_mixRate);
            ch->streamingGain.Configure((double)m_mixRate);
            ch->personalGain.SnapTo(ch->pmute ? 0.0f : ch->pvol);
            ch->streamingGain.SnapTo(ch->smute ? 0.0f : ch->svol);
            ch->peakHold.Configure(PeakHoldFramesFor(m_mixRate));
            ch->captureScratch.assign(kMaxPullFrames * 2, 0.0f);
            // NOT started here (fj#10). Whether anything is pulling the mix is
            // not yet known at this point -- the streaming render is brought up
            // below -- so the decision is made once, after the whole graph
            // exists, by MatchCapturesToDemand. Idle until then.
            ch->idle.store(true);
            m_channels.push_back(std::move(ch));
        }
        // Everything MixPull touches now exists and will not move again.
        m_graphReady.store(true, std::memory_order_release);

        // ── Streaming mix out ─────────────────────────────────────────────
        const EndpointInfo* streamEp = MatchBinding(eps, cfg.streamingCable.render);
        if (streamEp) {
            std::wstring serr;
            m_streamCushionDone = false;
            m_streamEnabled = m_streamRender.Start(streamEp->id, [this](float* out, size_t frames) {
                try {
                    if (frames > kMaxPullFrames) { memset(out, 0, frames * 2 * sizeof(float)); return; }
                    if (frames > m_maxStreamPull) m_maxStreamPull = frames;
                    m_streamPullWindow.Push(frames, frames);   // see MixPull (fj#12)
                    if (!m_streamCushionDone.load(std::memory_order_acquire)) {
                        if (m_streamRing.Depth() < CushionFor(m_streamPullWindow.Max())) { memset(out, 0, frames * 2 * sizeof(float)); return; }
                        // Rates are re-derived here, so a mix-rate change only has
                        // to clear the cushion flag for this chain to follow it.
                        m_streamResampler.SetRates((double)m_mixRate.load(), (double)m_streamRender.DeviceRate());
                        m_streamCushionDone.store(true, std::memory_order_release);
                    }
                    if (m_streamResampler.IsPassthrough()) {
                        size_t real = m_streamRing.Read(out, frames);
                        if (real < frames) m_streamCushionDone.store(false, std::memory_order_release);
                        return;
                    }
                    // Exact-need pull: NeedInput consumes precisely what k frames require;
                    // any slack would over-consume the ring one frame per call and walk
                    // the resampler position off the buffer (the pull tests pin this).
                    size_t srcNeed = m_streamResampler.NeedInput(frames);
                    if (srcNeed * 2 > m_streamScratch.size()) { memset(out, 0, frames * 2 * sizeof(float)); return; }
                    size_t real = m_streamRing.Read(m_streamScratch.data(), srcNeed);
                    if (real < srcNeed) m_streamCushionDone = false;   // ring zero-fills the tail
                    size_t produced = m_streamResampler.Process(m_streamScratch.data(), srcNeed, out, frames);
                    for (size_t i = produced; i < frames; ++i) {       // cannot happen by construction; belt only
                        out[i*2] = 0.0f;
                        out[i*2+1] = 0.0f;
                    }
                } catch (...) { memset(out, 0, frames * 2 * sizeof(float)); }
            }, &serr);
            if (!m_streamEnabled) notes += L"streaming render: " + serr + L"; ";
        } else if (!cfg.streamingCable.render.id.empty() || !cfg.streamingCable.render.name.empty()) {
            notes += L"streaming cable render endpoint not found: " + cfg.streamingCable.render.name + L"; ";
        }

        // ── Mic chain ─────────────────────────────────────────────────────
        const EndpointInfo* micIn = MatchBinding(eps, cfg.mic.input);
        const EndpointInfo* micOut = MatchBinding(eps, cfg.mic.cable.render);
        if (micIn && micOut) {
            m_micVol = cfg.mic.gain;
            m_micMute = false;
            m_micEq.Configure(48000.0);   // reconfigured to the render rate below
            for (size_t b = 0; b < cfg.mic.eq.bands.size() && b < ChannelEq::kBands; ++b)
                m_micEq.SetBand(b, cfg.mic.eq.bands[b].freq, cfg.mic.eq.bands[b].gainDb, cfg.mic.eq.bands[b].q);
            m_micEq.SetEnabled(cfg.mic.eq.enabled);
            std::wstring merr;
            bool micCapOk = m_micCapture.Start(micIn->id, micIn->isRender /*tap a render endpoint via loopback*/,
                [this](const float* f, size_t n) {
                    try {
                        if (!m_micReady.load(std::memory_order_acquire)) return;
                        if (m_micResampler.IsPassthrough()) { m_micRing.Write(f, n); return; }
                        size_t maxOut = m_micResampler.EstimateOut(n);
                        if (maxOut * 2 > m_micCapScratch.size()) return;
                        size_t produced = m_micResampler.Process(f, n, m_micCapScratch.data(), maxOut);
                        m_micRing.Write(m_micCapScratch.data(), produced);
                    } catch (...) {}
                }, &merr);
            if (micCapOk) {
                m_micCushionDone = false;
                bool micRenOk = m_micRender.Start(micOut->id, [this](float* out, size_t frames) {
                    try {
                        if (frames > kMaxPullFrames) { memset(out, 0, frames * 2 * sizeof(float)); return; }
                        if (frames > m_maxMicPull) m_maxMicPull = frames;
                        m_micPullWindow.Push(frames, frames);   // see MixPull (fj#12)
                        if (!m_micCushionDone) {
                            if (m_micRing.Depth() < CushionFor(m_micPullWindow.Max())) { memset(out, 0, frames * 2 * sizeof(float)); return; }
                            m_micCushionDone = true;
                        }
                        size_t real = m_micRing.Read(out, frames);
                        if (real < frames) m_micCushionDone = false;
                        m_micGain.Process(out, frames);
                        m_micEq.Process(out, frames);
                    } catch (...) { memset(out, 0, frames * 2 * sizeof(float)); }
                }, &merr);
                if (micRenOk) {
                    m_micGain.Configure((double)m_micRender.DeviceRate());
                    m_micGain.SnapTo(m_micMute ? 0.0f : m_micVol);
                    m_micEq.Configure((double)m_micRender.DeviceRate());
                    for (size_t b = 0; b < cfg.mic.eq.bands.size() && b < ChannelEq::kBands; ++b)
                        m_micEq.SetBand(b, cfg.mic.eq.bands[b].freq, cfg.mic.eq.bands[b].gainDb, cfg.mic.eq.bands[b].q);
                    m_micResampler.SetRates((double)m_micCapture.SourceRate(), (double)m_micRender.DeviceRate());
                    m_micReady.store(true, std::memory_order_release);
                    m_micEnabled = true;
                } else {
                    m_micCapture.Stop();
                    notes += L"mic cable render: " + merr + L"; ";
                }
            } else {
                notes += L"mic capture: " + merr + L"; ";
            }
        } else if (!cfg.mic.input.id.empty() || !cfg.mic.input.name.empty()) {
            notes += L"mic input or mic cable endpoint not found; ";
        }

        // The whole graph exists now, so whether anything is pulling the mix
        // is finally knowable: the personal render was attempted above, the
        // streaming render is up or not, and the feed's state is known. Start
        // the captures that are warranted and leave the rest idle (fj#10).
        MatchCapturesToDemand(eps);
        for (const auto& ch : m_channels)
            if (!ch->idle.load() && !ch->healthy.load())
                notes += ch->id + L": " + ch->healthMsg + L"; ";

        if (err) *err = notes;
        return true;   // the app always starts; notes carry the degradations
    } catch (...) {
        if (err) *err = L"engine start: unexpected exception";
        return false;
    }
}

bool Engine::StartChannelCapture(ChannelRuntime& ch, const std::vector<EndpointInfo>& eps) {
    const EndpointInfo* ep = MatchBinding(eps, ch.captureBinding);
    if (!ep) {
        ch.healthy = false;
        ch.healthMsg = L"cable capture endpoint not found: " + ch.captureBinding.name;
        return false;
    }
    ch.ready.store(false, std::memory_order_release);
    // Capacity from the mix rate: 500 ms, which is the worst latency a stall
    // can leave behind (fj#13). Safe here -- the capture is stopped and
    // `ready` is false, so nothing is touching the ring.
    ch.ring.Resize((size_t)m_mixRate.load() * kRingMs / 1000);
    ch.speedTrim.store(1.0, std::memory_order_relaxed);
    ch.appliedSpeed = 1.0;
    std::wstring cerr;
    ChannelRuntime* chp = &ch;
    // A render endpoint bound as a channel source is tapped via WASAPI loopback —
    // lets a channel mirror any output device, and makes the graph testable
    // without a working third-party cable engine.
    bool ok = ch.capture.Start(ep->id, ep->isRender, [this, chp](const float* f, size_t n) {
        try {
            if (!chp->ready.load(std::memory_order_acquire)) return;
            // The drain, applied here because the resampler belongs to this
            // thread (fj#13). Only when it CHANGES: SetSpeed is cheap and
            // allocation-free, but there is no reason to recompute a ratio per
            // block, and comparing against the last applied value keeps the
            // common case -- a speed of exactly 1.0, for ever -- free.
            const double want = chp->speedTrim.load(std::memory_order_relaxed);
            if (want != chp->appliedSpeed) {
                chp->resampler.SetSpeed(want);
                chp->appliedSpeed = want;
            }
            if (chp->resampler.IsPassthrough()) { chp->ring.Write(f, n); return; }
            size_t maxOut = chp->resampler.EstimateOut(n);
            if (maxOut * 2 > chp->captureScratch.size()) return;
            size_t produced = chp->resampler.Process(f, n, chp->captureScratch.data(), maxOut);
            chp->ring.Write(chp->captureScratch.data(), produced);
        } catch (...) {}
    }, &cerr);
    if (!ok) {
        ch.healthy = false;
        ch.healthMsg = L"capture failed: " + cerr;
        return false;
    }
    ch.resampler.SetRates((double)ch.capture.SourceRate(), (double)m_mixRate);
    ch.ring.Clear();
    ch.cushionDone = false;
    ch.ready.store(true, std::memory_order_release);
    ch.healthy = true;
    ch.healthMsg.clear();
    return true;
}

void Engine::MixPull(float* out, size_t frames) {
    try {
        if (frames > kMaxPullFrames) { memset(out, 0, frames * 2 * sizeof(float)); return; }
        if (!m_graphReady.load(std::memory_order_acquire)) {
            memset(out, 0, frames * 2 * sizeof(float));   // graph being built or torn down
            return;
        }
        // The cushion is sized from the largest pull in the LAST MINUTE, not
        // the largest ever seen (fj#12). A high-water mark that only rose meant
        // one Bluetooth hiccup raised the latency for the rest of the session
        // and it never came back down. The window's tick is frames, because the
        // audio thread must not call a clock, and the mix advances by exactly
        // the frames it pulls.
        m_mixPullWindow.Push(frames, frames);
        if (frames > m_maxMixPull) m_maxMixPull = frames;   // all-time, for MDXM_DIAG only
        // Proof of life for the peak meters, before anything can return early
        // for a reason that is not "the mix stopped".
        m_mixFrames.fetch_add(frames, std::memory_order_relaxed);
        std::fill(m_pSum.begin(), m_pSum.begin() + frames * 2, 0.0f);
        std::fill(m_sSum.begin(), m_sSum.begin() + frames * 2, 0.0f);
        for (auto& chp : m_channels) {
            ChannelRuntime& ch = *chp;
            if (!ch.healthy.load(std::memory_order_relaxed)) {
                // No capture running, so there is nothing to meter. "Cannot
                // know", not "silent": a client sorting by what is making
                // sound must not rank an unhealthy channel among the ones that
                // are genuinely quiet.
                ch.peakHold.MarkUnknown();
                ch.peakPub.store(kPeakUnknown, std::memory_order_relaxed);
                ch.peakNowPub.store(kPeakUnknown, std::memory_order_relaxed);
                continue;
            }
            if (!ch.cushionDone) {
                if (ch.ring.Depth() < CushionFor(m_mixPullWindow.Max())) {
                    // Healthy, and deliberately silent while the cushion fills.
                    // Pushed as a real zero rather than skipped, so a held peak
                    // from before a dropout expires on schedule instead of
                    // being frozen by the gap that follows it.
                    ch.peakHold.Push(0.0f, frames);
                    ch.peakPub.store(ch.peakHold.Value(), std::memory_order_relaxed);
                    ch.peakNowPub.store(0.0f, std::memory_order_relaxed);
                    continue;                                      // silent until the cushion fills
                }
                ch.cushionDone = true;
            }
            size_t real = ch.ring.Read(m_chanBuf.data(), frames);
            if (real < frames) ch.cushionDone = false;             // rebuild after a gap
            ch.eq.Process(m_chanBuf.data(), frames);
            // The channel's own signal: post EQ, PRE the two gains. See
            // ChannelState::peak -- one source and two gains means a post-fader
            // peak would have to be two numbers, and "which of these is making
            // sound" stays true of a channel muted on one side.
            const float blockPeak = BlockPeak(m_chanBuf.data(), frames);
            ch.peakHold.Push(blockPeak, frames);
            ch.peakPub.store(ch.peakHold.Value(), std::memory_order_relaxed);
            ch.peakNowPub.store(blockPeak, std::memory_order_relaxed);
            memcpy(m_pBuf.data(), m_chanBuf.data(), frames * 2 * sizeof(float));
            memcpy(m_sBuf.data(), m_chanBuf.data(), frames * 2 * sizeof(float));
            ch.personalGain.Process(m_pBuf.data(), frames);
            ch.streamingGain.Process(m_sBuf.data(), frames);
            for (size_t i = 0; i < frames * 2; ++i) {
                m_pSum[i] += m_pBuf[i];
                m_sSum[i] += m_sBuf[i];
            }
        }
        m_pLimiter.Process(m_pSum.data(), frames);
        m_sLimiter.Process(m_sSum.data(), frames);
        memcpy(out, m_pSum.data(), frames * 2 * sizeof(float));
        if (m_streamEnabled) m_streamRing.Write(m_sSum.data(), frames);
        // The same streaming sum, handed to MDropDX12 without an audio device
        // (ipc/stream_feed.h). Deliberately the STREAMING mix and not the
        // personal one: the personal mix runs at 1-10% here and would give the
        // visualiser a signal 20-40 dB down, and muting a channel personally
        // would blank it. Lock-free and allocation-free, so it is safe on this
        // thread.
        // One relaxed atomic load per block when the feed is off, which is
        // the normal case, and no copy at all.
        if (m_feedOn.load(std::memory_order_relaxed)) m_feed.Write(m_sSum.data(), frames);
        // The VBAN sink (spec §3.1). The PERSONAL sum by default -- the whole
        // point of the feature is to hear what the headphones are hearing,
        // per-channel balance and personal mutes included -- or the streaming
        // sum when configured, which is the programme mix and a different thing.
        //
        // The write is all that happens on this thread: makeup gain, the
        // limiter, the int16 conversion and the socket are the sender thread's
        // work. One relaxed load per block when the sink is off, and no copy.
        if (m_vbanOn.load(std::memory_order_relaxed))
            m_vbanRing.Write(m_vbanStreamingSrc.load(std::memory_order_relaxed)
                                 ? m_sSum.data() : m_pSum.data(), frames);
    } catch (...) {
        memset(out, 0, frames * 2 * sizeof(float));   // no-crash rule
    }
}

bool Engine::StartPersonalRender(const std::wstring& endpointId) {
    std::wstring rerr;
    uint32_t oldRate = m_mixRate.load();
    bool ok = m_personalRender.Start(endpointId, [this](float* out, size_t frames) {
        MixPull(out, frames);
    }, &rerr);
    if (ok) {
        m_currentPersonalId = endpointId;
        m_mixRate.store(m_personalRender.DeviceRate());
        // The clock master changed rate under a graph that is already running:
        // everything rate-dependent has to follow it or the channels starve.
        if (m_mixRate.load() != oldRate && !m_channels.empty()) ReconfigureForMixRate();
    }
    return ok;
}

void Engine::ReconfigureForMixRate() {
    m_graphReady.store(false, std::memory_order_release);   // MixPull renders silence meanwhile
    const uint32_t rate = m_mixRate.load();
    m_baseCushion = rate * (uint32_t)m_cushionMs / 1000;
    m_maxMixPull = m_maxStreamPull = 0;
    // The windows are measured in FRAMES, so a rate change rescales them --
    // and clears them, which is right: pull sizes recorded at the old rate
    // describe a different device.
    m_mixPullWindow.Configure(rate * kPullWindowSeconds);
    m_streamPullWindow.Configure(rate * kPullWindowSeconds);
    m_micPullWindow.Configure(rate * kPullWindowSeconds);
    auto eps = EnumerateEndpoints();
    for (auto& chp : m_channels) {
        ChannelRuntime& ch = *chp;
        ch.ready.store(false, std::memory_order_release);
        ch.capture.Stop();                                  // no producer while we retune
        ch.eq.Configure((double)rate);
        for (size_t b = 0; b < ch.eqBands.size() && b < ChannelEq::kBands; ++b)
            ch.eq.SetBand(b, ch.eqBands[b].freq, ch.eqBands[b].gainDb, ch.eqBands[b].q);
        ch.personalGain.Configure((double)rate);
        ch.streamingGain.Configure((double)rate);
        // The hold is a frame count, so it is rate-dependent like everything
        // else here: left at the old rate it would hold for the wrong duration
        // after a move from 44.1 to 96 kHz.
        ch.peakHold.Configure(PeakHoldFramesFor(rate));
        StartChannelCapture(ch, eps);   // re-derives the resampler rates, clears the ring
    }
    // The stream thread re-derives its own resampler on the next cushion rebuild.
    m_streamRing.Clear();
    // Audio captured at the old rate would be played at the new one: a backlog
    // that is also the wrong speed. The sender re-derives its SR index from the
    // live rate, so the only thing to undo here is the content.
    m_vbanRing.Clear();
    m_streamCushionDone.store(false, std::memory_order_release);
    m_graphReady.store(true, std::memory_order_release);
}

void Engine::Stop() {
    // Gate first: any pull that lands mid-teardown renders silence instead of
    // reading a dissolving channel list.
    m_graphReady.store(false, std::memory_order_release);
    // Streams next (their threads pull at engine state), then state.
    m_personalRender.Stop();
    m_streamRender.Stop();
    m_micCapture.Stop();
    m_micRender.Stop();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& ch : m_channels) ch->capture.Stop();
    m_channels.clear();
    m_streamEnabled = false;
    // Closed only after every render thread has stopped: the write happens on
    // one of them, and unmapping underneath it would fault.
    m_feed.Close();
    m_micEnabled = false;
    m_micReady = false;
    m_currentPersonalId.clear();
    m_personalFallback = false;
}

bool Engine::SetVolume(const std::wstring& ch, Mix m, float vol01) {
    vol01 = std::clamp(vol01, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ch == L"mic") {
        m_micVol = vol01;
        m_micGain.SetTarget(m_micMute ? 0.0f : m_micVol);
        return true;
    }
    for (auto& c : m_channels) {
        if (c->id != ch) continue;
        if (m == Mix::Personal) { c->pvol = vol01; c->personalGain.SetTarget(c->pmute ? 0.0f : vol01); }
        else                    { c->svol = vol01; c->streamingGain.SetTarget(c->smute ? 0.0f : vol01); }
        return true;
    }
    return false;
}

bool Engine::SetMute(const std::wstring& ch, Mix m, bool mute) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ch == L"mic") {
        m_micMute = mute;
        m_micGain.SetTarget(mute ? 0.0f : m_micVol);
        return true;
    }
    for (auto& c : m_channels) {
        if (c->id != ch) continue;
        if (m == Mix::Personal) { c->pmute = mute; c->personalGain.SetTarget(mute ? 0.0f : c->pvol); }
        else                    { c->smute = mute; c->streamingGain.SetTarget(mute ? 0.0f : c->svol); }
        return true;
    }
    return false;
}

bool Engine::SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ch == L"mic") { m_micEq.SetBand(band, f, g, q); return true; }
    for (auto& c : m_channels)
        if (c->id == ch) {
            c->eq.SetBand(band, f, g, q);
            // Remembered so a later mix-rate change can recompute this band.
            if (band < ChannelEq::kBands) {
                while (c->eqBands.size() <= band) c->eqBands.push_back({});
                c->eqBands[band] = { f, g, q };
            }
            return true;
        }
    return false;
}

bool Engine::EnableEq(const std::wstring& ch, bool on) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ch == L"mic") { m_micEq.SetEnabled(on); return true; }
    for (auto& c : m_channels)
        if (c->id == ch) { c->eqEnabled = on; c->eq.SetEnabled(on); return true; }
    return false;
}

bool Engine::SetPersonalOutput(const std::wstring& endpointId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_personalRender.Stop();
    if (!StartPersonalRender(endpointId)) return false;
    // An explicit route move rebinds: this is the configured device now.
    auto eps = EnumerateEndpoints();
    for (const auto& e : eps)
        if (e.id == endpointId) { m_boundPersonal = { e.id, e.name }; break; }
    m_personalFallback = false;
    return true;
}

// Every endpoint the graph is CAPTURING from. Rendering the mix to one of
// these feeds it straight back into itself.
std::vector<std::wstring> Engine::CaptureSources() const {
    std::vector<std::wstring> out;
    for (const auto& ch : m_channels)
        if (!ch->captureBinding.id.empty()) out.push_back(ch->captureBinding.id);
    // Sonar's own channels are not captured by us directly, but everything
    // played to one comes back out of the cable that IS captured, so they
    // close the same loop one step further round. A virtual endpoint owned by
    // another program is never a sensible place to send what someone is
    // listening to anyway.
    for (const auto& id : m_loopSuspects) out.push_back(id);
    return out;
}

void Engine::EnsurePersonalRender(const std::vector<EndpointInfo>& eps) {
    // Control thread, m_mutex held. The render must be running on:
    // the bound device when present, else the temp fallback target.
    const EndpointInfo* bound = MatchBinding(eps, m_boundPersonal);
    std::wstring want = bound ? bound->id
                              : PickPersonalOutput(eps, m_boundPersonal,
                                                   DefaultRenderEndpointId(), CaptureSources());
    // Dead(), not Invalidated(). A stalled stream never sets `invalidated`
    // and is exactly how this fails across a resume: the endpoint is still
    // listed and still active, so `bound` matches, `want` equals
    // m_currentPersonalId, and this returned early leaving a dead stream in
    // place. That early return is what made the silence permanent.
    bool renderDead = m_personalRender.Dead();
    if (want.empty()) { if (renderDead) m_personalRender.Stop(); return; }
    if (!renderDead && want == m_currentPersonalId) return;
    m_personalRender.Stop();
    const bool started = StartPersonalRender(want);
    // THE DEVICE RECONNECTED, so start the rings from zero rather than draining
    // them (fj#13). Shane's rule: "the ring should restart from zero if the
    // device reconnects as there was already a drop of some ms". Whatever
    // accumulated while the render was away is stale by definition, and the
    // listener has just heard an interruption anyway -- so there is nothing to
    // protect by easing it out at 5% over the next ten seconds.
    if (started) {
        for (auto& ch : m_channels) {
            if (ch->idle.load()) continue;
            ch->ring.Clear();
            ch->cushionDone = false;
            ch->speedTrim.store(1.0, std::memory_order_relaxed);
        }
    }
}

// ── Who is pulling the mix, and what that means for capture (fj#10) ──────
//
// Control thread, m_mutex held.
MixDemand Engine::CurrentDemand() const {
    MixDemand d;
    // A render that exists and is not dead. m_currentPersonalId alone is not
    // enough: it survives a stream that has stalled or been invalidated, and a
    // stalled render pulls nothing.
    d.personalRender = !m_currentPersonalId.empty() && !m_personalRender.Dead();
    d.streamingRender = m_streamEnabled;
    d.feed = m_feedOn.load(std::memory_order_relaxed);
    // Reported, never counted: AnyoneListening ignores it on purpose, because
    // this sink has no clock of its own (mix_demand.h).
    d.vban = m_vbanOn.load(std::memory_order_relaxed);
    return d;
}

// Start or stop the channel captures to match demand.
//
// WHY THIS EXISTS. A capture used to run from engine start to engine stop,
// whatever was or was not pulling the mix. With no personal render device --
// which is an ordinary thing after a failed start or an unplugged headset --
// every channel went on waking per WASAPI buffer, copying into a ring, and
// discarding all of it: measured at 41.4 million dropped frames over fourteen
// minutes, the ring pinned at full depth, underruns at zero because the mix
// thread never ran at all.
//
// The rule was already in this program for the shared-memory feed, which stays
// off until a client asks and whose comment gives the reason -- "a mapping
// nobody reads and a copy per audio block for nothing". It simply had never
// been applied to capture. Shane's framing: "mdxmixer is only publishing audio
// on demand -- if there is no demand for it to publish (and there wasn't) it
// shouldn't run the ring."
//
// STOPPING ALSO FIXES THE STALENESS. A ring left full while nobody listened
// handed the next consumer two seconds of audio recorded in the meantime, and
// because producer and consumer run at the same rate that backlog never
// cleared. StartChannelCapture clears the ring, so a capture that idles comes
// back in sync.
//
// IDLE IS NOT BROKEN. `healthy` is left exactly as it was, and `idle` says why
// there is no stream; a machine with nothing listening must not read as a
// machine with a fault.
void Engine::MatchCapturesToDemand(const std::vector<EndpointInfo>& eps) {
    const bool wanted = AnyoneListening(CurrentDemand());
    for (auto& ch : m_channels) {
        if (wanted && ch->idle.load()) {
            ch->idle.store(false);
            // A capture that cannot start says so through `healthy`, exactly
            // as it does on any other start path.
            StartChannelCapture(*ch, eps);
            Log(2, L"%s: capture started -- something is listening again", ch->id.c_str());
        } else if (!wanted && !ch->idle.load()) {
            // Order matters: stop the callback writing before marking idle, so
            // nothing is still arriving when the ring is declared empty.
            ch->ready.store(false, std::memory_order_release);
            ch->capture.Stop();
            ch->ring.Clear();
            ch->peakPub.store(kPeakUnknown, std::memory_order_relaxed);
            ch->idle.store(true);
            Log(2, L"%s: capture stopped -- nothing is pulling the mix", ch->id.c_str());
        }
    }
}

// Is the mix actually being pulled? Control thread, ~1 s.
//
// The LAST line of defence, and it is here because the two detectors above it
// can both be clean while there is still silence. RenderStream::Dead() catches
// a stream that noticed; the device notifications catch an endpoint that went
// away. Neither catches a render thread that is alive and clocking while
// MixPull is not actually running -- a graph left not-ready by a rebuild that
// failed halfway, a Stop() that raced a restart. The only honest test is
// whether frames are moving, which is what Engine::MixFrames is for.
//
// It is also the backstop for the power notifications: PBT_APMSUSPEND and
// PBT_APMRESUMEAUTOMATIC are not guaranteed to arrive for every Modern
// Standby transition, and a recovery that depends on a message Windows may
// not send is not a recovery.
bool WatchdogShouldRestart(bool framesMoved, bool haveRender, unsigned stuckMs) {
    if (framesMoved) return false;   // it is working
    if (!haveRender) return false;   // nothing is supposed to be pulling
    return stuckMs >= kWatchdogStuckMs;
}

void Engine::TickWatchdog() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const unsigned now = (unsigned)GetTickCount();
    const uint64_t frames = m_mixFrames.load(std::memory_order_relaxed);
    const bool framesMoved = (frames != m_lastWatchdogFrames) || m_watchdogMovedMs == 0;
    const bool haveRender = !m_currentPersonalId.empty();
    const unsigned stuckMs = now - m_watchdogMovedMs;

    if (framesMoved) {
        m_lastWatchdogFrames = frames;
        m_watchdogMovedMs = now;
        return;
    }
    // Nothing is supposed to be running: no output device, or the engine is
    // stopped. Keep the clock moving so a later start is not instantly
    // "overdue" and restarted the moment it comes up.
    if (!haveRender) { m_watchdogMovedMs = now; return; }
    if (!WatchdogShouldRestart(framesMoved, haveRender, stuckMs)) return;

    Log(1, L"watchdog: no mix pull for %u ms (invalidated=%d stalled=%d) -- restarting the render",
        stuckMs, m_personalRender.Invalidated() ? 1 : 0, m_personalRender.Stalled() ? 1 : 0);
    const std::wstring id = m_currentPersonalId;
    m_personalRender.Stop();
    if (!StartPersonalRender(id))
        Log(1, L"watchdog: %ls would not take a render; leaving it to failover", id.c_str());
    // Reset either way. On success frames start moving again and the next tick
    // records it; on failure this stops us retrying the same dead device four
    // times a second and lets the failover watcher re-home instead.
    m_watchdogMovedMs = (unsigned)GetTickCount();
}

void Engine::OnSuspend() {
    // Stop the render BEFORE the machine goes down, so there is nothing to
    // come back to in an undefined state. Cheap, and it turns the resume from
    // "work out what survived" into "start fresh".
    //
    // Best-effort: Windows allows about two seconds here, and a Modern
    // Standby transition may not deliver this at all. The watchdog is what
    // makes the recovery unconditional.
    Log(2, L"suspend: stopping the personal render");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_personalRender.Stop();
}

void Engine::OnResume() {
    Log(2, L"resume: holding failover, rebinding devices, restarting the render");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // HOLD THE WATCHER FIRST, before anything can tick it.
        //
        // Its stability, dwell and minimum-gap windows are all wall-clock, and
        // a suspend advances the wall clock by hours -- so every one of them is
        // already expired the instant we wake, and the watcher would commit on
        // its first post-resume tick against a device list that is still
        // settling. The log line at 04:46:20 on 2026-10-04, a bare "failover
        // committed" with nothing before it for seven hours, is that happening.
        // Same 15 s ceiling the all-devices-absent case uses, and released
        // early the same way once things look steady.
        m_failover.HoldFor(15000);
        // Unconditionally, rather than asking whether it is dead: a stalled
        // stream takes a second to admit it, and a resume is exactly when one
        // is most likely. Restarting a healthy stream costs a sub-second gap
        // on a machine that was asleep a moment ago, which nobody hears.
        m_personalRender.Stop();
        m_watchdogMovedMs = 0;   // the frame count will not move until it is back
    }
    // Captures first: a channel whose source is a Sonar virtual endpoint has
    // to be re-bound before there is anything worth rendering.
    OnDeviceSetChanged();
    // And then the render, EXPLICITLY. TickFailover will not do it: Stop()
    // frees the stream's impl, so Dead() reads false afterwards, and with the
    // bound device present and no fallback in force none of its three branches
    // fire. Relying on it here would leave the render down until something
    // else happened to change.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        EnsurePersonalRender(EnumerateEndpoints());
    }
}

void Engine::OnDeviceSetChangedImpl() {
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto eps = EnumerateEndpoints();
        for (auto& ch : m_channels) {
            // An idle channel has no stream to be invalid and nothing to
            // repair; whether it should be running at all is decided by
            // demand, below (fj#10).
            if (ch->idle.load()) continue;
            if (!ch->healthy.load() || ch->capture.Invalidated()) {
                ch->capture.Stop();
                StartChannelCapture(*ch, eps);
            }
        }
        MatchCapturesToDemand(eps);
    } catch (...) {
        Log(1, L"device-change sweep: unknown exception");
    }
    TickFailover();
}

bool Engine::SetFeedEnabled(bool on, std::wstring* err) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (on == m_feedOn.load(std::memory_order_relaxed)) return true;

    if (!on) {
        // Stop the audio thread touching it BEFORE the mapping goes, and
        // let a block drain: the flag is read per block without a lock, so
        // closing underneath an in-flight Write would be a use-after-free.
        m_feedOn.store(false, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        m_feed.Close();
        Log(2, L"stream feed: off");
        return true;
    }

    const uint32_t rate = m_mixRate.load();   // already atomic; read it once
    if (rate == 0) {
        if (err) *err = L"the engine is not running";
        return false;
    }
    // Two seconds of ring at the mix rate: generous, because the reader is a
    // visualiser that can hitch for a frame or two and must come back to
    // recent audio rather than a lap-old buffer.
    std::wstring openErr;
    if (!m_feed.Open(rate, 2, rate * 2, &openErr)) {
        if (err) *err = openErr;
        return false;
    }
    m_feed.SetGain(m_feedGain);
    m_feedOn.store(true, std::memory_order_relaxed);
    Log(2, L"stream feed: on (%u Hz, 2 ch)", rate);
    return true;
}

void Engine::SetVbanSink(bool on, bool streamingSource) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // The source is stored whether or not the on/off state is changing: a live
    // switch between the personal and streaming sums is a legitimate thing to
    // do while the stream is running, and it takes effect on the next block.
    m_vbanStreamingSrc.store(streamingSource, std::memory_order_relaxed);
    if (on == m_vbanOn.load(std::memory_order_relaxed)) return;
    // Cleared BEFORE the flag goes up, so the first thing the sender drains is
    // audio from after the subscription rather than whatever was in flight when
    // the last listener left. There is no symmetric wait on the way down (the
    // feed needs one because it unmaps a view; this ring outlives the engine).
    if (on) m_vbanRing.Clear();
    m_vbanOn.store(on, std::memory_order_relaxed);
    Log(2, L"vban sink: %s (%s mix)", on ? L"on" : L"off",
        streamingSource ? L"streaming" : L"personal");
}

void Engine::TickFailover() {
    // What the app layer must be told once the lock is released. Calling back
    // into app code while holding m_mutex is a same-thread relock the moment
    // that code reads engine state (GetChannelStates/GetDiag) — which is
    // exactly what a "the route moved" notification does.
    bool notify = false;
    DeviceRef notifyRef;
    std::function<void(const DeviceRef&)> cb;
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_epSnapshot = EnumerateEndpoints();

        // ── AUDIODG, the cause rather than the symptom (fj#2 §4) ─────────
        //
        // Ported from MDropDX12's MixerAudioEngineTick (#410). The endpoint
        // test below sees the audio graph rebuilding only when it takes every
        // endpoint down with it; AUDIODG restarting while the endpoints stay
        // listed is the same hazard and is invisible to it. The process id is
        // the signal -- a restart always gets a new one -- and it is read
        // BEFORE the decision, so the hold is in place on the same tick.
        //
        // The hold length is a CEILING, not a duration: killed cleanly AUDIODG
        // is back in under a second, and about a minute after Sonar's APO
        // faults it. Both measured. So it is released as soon as the engine
        // has been back and unchanged for a moment rather than served out.
        if (m_audiodg.ShouldScan((unsigned)GetTickCount64())) {
            const unsigned now = (unsigned)GetTickCount64();
            switch (m_audiodg.Observe(FindAudiodgPid(), now)) {
            case AudiodgEvent::Restarted:
                m_failover.HoldFor(60000);
                Log(1, L"Windows audio engine (AUDIODG.EXE) restarted (#%u, now pid %lu); "
                       L"holding failover up to 60s",
                    m_audiodg.Restarts(), (unsigned long)m_audiodg.Pid());
                break;
            case AudiodgEvent::SteadyAgain:
                if (m_failover.HoldRemainingMs() > 0) {
                    m_failover.ReleaseHold();
                    Log(2, L"Windows audio engine is back and steady; resuming failover");
                }
                break;
            case AudiodgEvent::Appeared:
                if (m_audiodg.Restarts() > 0)
                    Log(2, L"Windows audio engine is back (pid %lu)",
                        (unsigned long)m_audiodg.Pid());
                break;
            case AudiodgEvent::Nothing:
                break;
            }
        }

        // The whole device set reading absent at once is the audio graph being
        // rebuilt, not every device being unplugged in the same tick. Acting
        // on that snapshot moves the route at the one moment no move can
        // succeed, and adds churn to a graph that is already rebuilding
        // (MDropDX12 #410). Hold, and let it lift when the devices come back.
        //
        // Kept ALONGSIDE the AUDIODG watch above rather than replaced by it:
        // this one catches an audio graph that has taken the endpoints down
        // without AUDIODG changing pid, and the watch catches a restart that
        // leaves the endpoints listed. Neither sees the other's case.
        bool anyRender = false;
        for (const auto& e : m_epSnapshot)
            if (e.isRender && e.isActive) { anyRender = true; break; }
        if (!anyRender) {
            m_failover.HoldFor(15000);
            m_failover.Tick();          // records the reason; commits nothing
            return;
        }
        if (m_failover.HoldRemainingMs() > 0) m_failover.ReleaseHold();

        // Presence is answered from the snapshot taken above rather than by
        // re-enumerating per call: the watcher asks about every allowlist
        // entry on every tick.
        bool boundPresent = MatchBinding(m_epSnapshot, m_boundPersonal) != nullptr;
        if (boundPresent && m_personalRender.Dead()) boundPresent = false;   // enumeration lag, or a stalled stream

        // ── The temporary fallback is the ENGINE's business, not the
        // watcher's ──────────────────────────────────────────────────────
        //
        // The old decider had a TempFallback action; the ported watcher has
        // no equivalent and should not have one. Hopping to the default
        // output so audio keeps flowing is not a routing DECISION, it is how
        // the render stream copes with its device being gone, and conflating
        // the two is what made the decider single-route. The watcher decides
        // only whether to re-home permanently.
        if (!boundPresent && !m_personalFallback) {
            m_personalFallback = true;
            EnsurePersonalRender(m_epSnapshot);
        } else if (boundPresent && m_personalFallback) {
            m_personalFallback = false;
            EnsurePersonalRender(m_epSnapshot);
        } else if (m_personalRender.Dead()) {
            EnsurePersonalRender(m_epSnapshot);
        }

        // WANTED AND NOT RUNNING (fj#11). The three branches above are all
        // about a stream that EXISTED -- the device went away, it came back,
        // the stream died. A render that never STARTED is none of them:
        // `boundPresent` is true because the device is right there, no
        // fallback is in force, and Dead() reads false because there is no
        // impl to be dead. So one failed start used to mean silence until
        // something else happened to shake the engine, which on 2026-10-04
        // meant fourteen minutes of it.
        //
        // Backed off, because the reason a device will not open is usually not
        // going to change in the next second -- it is being installed, or held
        // exclusively, or mid-handshake. See RenderRetry.
        if (m_currentPersonalId.empty()) {
            const unsigned now = (unsigned)GetTickCount64();
            if (m_renderRetry.Due(now)) {
                const std::wstring want =
                    PickPersonalOutput(m_epSnapshot, m_boundPersonal,
                                       DefaultRenderEndpointId(), CaptureSources());
                if (want.empty()) {
                    // Nothing to render to at all. Not a failure to back off
                    // from -- failover below is the thing that fixes it -- so
                    // the counter is cleared and the next real candidate gets
                    // an immediate attempt.
                    m_renderRetry.Reset();
                } else if (StartPersonalRender(want)) {
                    m_renderRetry.Reset();
                    Log(2, L"personal render started on retry");
                } else {
                    m_renderRetry.Failed(now);
                    Log(1, L"personal render still will not start (attempt %u); "
                           L"next try in %u s",
                        m_renderRetry.Attempts(), m_renderRetry.IntervalMs() / 1000);
                }
            }
        } else {
            m_renderRetry.Reset();
        }

        // Captures follow demand, every tick (fj#10). Placed after the render
        // work on purpose: a render that has just come up is a consumer, and
        // its channels should start in the same tick rather than a second
        // later with the mix pulling from empty rings.
        MatchCapturesToDemand(m_epSnapshot);

        // And the varispeed trim that holds each ring at its cushion (fj#13).
        // Computed here rather than on the audio thread because the depth moves
        // slowly and the decision is arithmetic over three numbers; the capture
        // thread only applies it.
        //
        // BOTH DIRECTIONS. Draining a backlog was the first half. A ring
        // running SHORT needs the producer to run slightly ahead instead, and
        // without that half a clock drifting the other way underruns for ever
        // -- which is exactly what ten underruns on 2026-10-05 were, with the
        // trim sitting at 1.0 throughout because it only knew how to speed up.
        {
            const size_t target = CushionFor(m_mixPullWindow.Max());
            for (auto& ch : m_channels) {
                if (ch->idle.load()) continue;
                const size_t depth = ch->ring.Depth();
                const double speed = RingSpeed(depth, target, ch->ring.Capacity());
                const double was = ch->speedTrim.exchange(speed, std::memory_order_relaxed);
                // Logged only on the way in and out of a correction, not for
                // every adjustment: this runs every second for ever.
                if (was == 1.0 && speed != 1.0)
                    Log(2, L"%s: %s at %.2f%% (%zu frames against a %zu cushion)",
                        ch->id.c_str(), speed > 1.0 ? L"draining" : L"refilling",
                        (speed - 1.0) * 100.0, depth, target);
                else if (was != 1.0 && speed == 1.0)
                    Log(2, L"%s: back to normal speed", ch->id.c_str());
            }
        }

        // The rule, re-sent each tick so a config edit takes effect without a
        // restart. Cheap: it is a map assignment of a handful of strings.
        RouteRule rule;
        rule.routeId = kPersonalRoute;
        rule.armed = m_failoverCfg.armed;
        for (const auto& a : m_failoverCfg.allow) rule.allow.push_back({ a.id, a.name });
        m_failover.SetRule(rule);
        m_failover.SetStabilitySeconds(m_failoverCfg.stabilitySec);
        m_failover.SetMinDwellSeconds(m_failoverCfg.dwellSec);
        m_failover.SetMinGapSeconds(m_failoverCfg.minGapSec);
        m_failover.NoteRouteDevice(kPersonalRoute, boundPresent ? m_boundPersonal.id
                                                                : std::wstring());

        // The commit is RECORDED here and applied below, after Tick returns.
        // Doing engine work inside the callback would re-enter the watcher's
        // own bookkeeping mid-iteration, which is a hang rather than a bug you
        // see — proved by doing exactly that in a test.
        std::wstring commitId;
        m_failover.SetOnCommit([&commitId](const std::wstring&, const std::wstring& dev) {
            commitId = dev;
        });
        m_failover.Tick();

        if (!commitId.empty()) {
            DeviceRef newRef{ commitId, commitId };
            for (const auto& e : m_epSnapshot)
                if (e.id == commitId) { newRef.name = e.name; break; }
            m_boundPersonal = newRef;
            m_personalFallback = false;
            m_personalRender.Stop();
            StartPersonalRender(commitId);
            // Tell the watcher where the route actually ended up, so a move
            // that did not stick is counted as an attempt rather than being
            // mistaken for success (MDropDX12 #410).
            m_failover.NoteRouteDevice(kPersonalRoute, commitId);
            cb = m_onFailoverCommit;   // invoked below, unlocked
            notifyRef = newRef;
            notify = cb != nullptr;
        }
    } catch (const std::exception& e) {
        Log(1, L"failover tick: %S", e.what());
        return;
    } catch (...) {
        Log(1, L"failover tick: unknown exception");
        return;
    }
    if (notify) {
        try { cb(notifyRef); }
        catch (...) { Log(1, L"failover commit callback threw"); }
    }
}

void Engine::SetFailoverCommitCallback(std::function<void(const DeviceRef&)> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onFailoverCommit = std::move(cb);
}

std::vector<ChannelState> Engine::GetChannelStates() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ChannelState> out;
    for (const auto& c : m_channels) {
        ChannelState s{ c->id, c->name, c->healthy.load(),
                        c->pvol, c->pmute, c->svol, c->smute, c->eqEnabled,
                        c->peakPub.load(std::memory_order_relaxed) };
        s.idle = c->idle.load();
        s.peakNow = c->peakNowPub.load(std::memory_order_relaxed);
        // An idle channel's cable has not been opened, so nothing is KNOWN to
        // be wrong with it -- and "bad" is a claim about a fault. It reports
        // ok, with `idle` carrying the reason there is no stream (fj#10).
        if (s.idle) s.healthy = true;
        out.push_back(std::move(s));
    }
    return out;
}

DiagState Engine::GetDiag() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    DiagState d;
    for (const auto& c : m_channels)
        d.rings.push_back({ c->id, c->ring.Depth(), c->ring.Drops(), c->ring.Underruns(),
                            c->ring.Capacity(),
                            c->speedTrim.load(std::memory_order_relaxed) });
    // The stream and mic rings report a speed of 1.0 and mean it: only channel
    // captures carry a varispeed trim. Their producers are this process's own
    // threads rather than a second hardware clock, so there is no drift
    // between the ends for a controller to correct.
    if (m_streamEnabled)
        d.rings.push_back({ L"stream", m_streamRing.Depth(), m_streamRing.Drops(),
                            m_streamRing.Underruns(), m_streamRing.Capacity() });
    if (m_micEnabled)
        d.rings.push_back({ L"mic", m_micRing.Depth(), m_micRing.Drops(),
                            m_micRing.Underruns(), m_micRing.Capacity() });
    d.personalDevice = m_currentPersonalId;
    d.personalFallback = m_personalFallback.load();
    d.maxMixPull = m_maxMixPull;
    d.windowPull = m_mixPullWindow.Max();
    d.cushionFrames = CushionFor(d.windowPull);
    d.mixRate = m_mixRate.load();
    return d;
}

// The cushion floor, live (fj#12).
//
// RE-CUSHIONS EVERY CHANNEL, which is the only way the change can mean
// anything. The cushion is the depth the mix waits for before it starts
// draining; once a channel is running, its ring sits at roughly whatever it
// filled to, and producer and consumer then move at the same rate. Lowering
// the number without re-cushioning would change what a FUTURE start does and
// leave today's latency exactly where it was.
//
// The cost is a gap of the new cushion's length -- tens of milliseconds -- in
// each channel while it refills. That is the honest price of changing the
// latency of a running graph, and it is why this is a deliberate command
// rather than something applied on a timer.
bool Engine::SetCushion(int ms, int headroomPercent, int flatMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ms >= 0) {
        if (ms < 5) ms = 5;
        if (ms > 200) ms = 200;
        m_cushionMs = ms;
    }
    if (headroomPercent >= 0)
        m_cushionHeadroom = headroomPercent > 400 ? 400 : headroomPercent;
    if (flatMs >= 0)
        m_cushionFlatMs = flatMs > 100 ? 100 : flatMs;
    m_baseCushion = m_mixRate.load() * (uint32_t)m_cushionMs / 1000;
    for (auto& ch : m_channels) {
        if (ch->idle.load()) continue;
        ch->ring.Clear();
        ch->cushionDone = false;
    }
    m_streamCushionDone = false;
    m_micCushionDone = false;
    Log(2, L"cushion: floor %d ms, headroom %d%%, flat %d ms -> %u frames now",
        m_cushionMs, m_cushionHeadroom, m_cushionFlatMs,
        (unsigned)CushionFor(m_mixPullWindow.Max()));
    return true;
}

int Engine::CushionMs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cushionMs;
}

int Engine::CushionHeadroomPercent() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cushionHeadroom;
}

int Engine::CushionFlatMs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cushionFlatMs;
}

// Everything the watcher already knew and nothing outside this process could
// read (fj#2 §3). Under m_mutex, like GetDiag: the watcher's state is written
// by TickFailover on the control thread, and this can be asked from a pipe
// request marshaled onto a different one.
FailoverStatus Engine::GetFailoverStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    FailoverStatus s;
    s.routeId = kPersonalRoute;
    switch (m_failover.StateOf(kPersonalRoute)) {
    case FailoverState::Searching: s.state = L"searching"; break;
    case FailoverState::Arming:    s.state = L"arming";    break;
    default:                       s.state = L"idle";      break;
    }
    s.reason         = m_failover.ReasonOf(kPersonalRoute);
    s.current        = m_failover.CurrentOf(kPersonalRoute);
    s.target         = m_failover.TargetOf(kPersonalRoute);
    s.attempts       = m_failover.AttemptsOf(kPersonalRoute);
    s.dwellMs        = m_failover.DwellMsOf(kPersonalRoute);
    s.sinceCommitMs  = m_failover.SinceCommitMs(kPersonalRoute);
    s.holdMs         = m_failover.HoldRemainingMs();
    s.audiodgPid     = m_audiodg.Pid();
    s.audiodgRestarts = m_audiodg.Restarts();
    return s;
}

} // namespace mdxm
