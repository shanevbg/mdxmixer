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
        m_baseCushion = m_mixRate * 3 / 100;   // ~30 ms — the spec's latency budget; adapts upward per consumer
        m_maxMixPull = m_maxStreamPull = m_maxMicPull = 0;

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
            if (!StartChannelCapture(*ch, eps))
                notes += ch->id + L": " + ch->healthMsg + L"; ";
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
                    if (!m_streamCushionDone.load(std::memory_order_acquire)) {
                        if (m_streamRing.Depth() < CushionFor(m_maxStreamPull)) { memset(out, 0, frames * 2 * sizeof(float)); return; }
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
                        if (!m_micCushionDone) {
                            if (m_micRing.Depth() < CushionFor(m_maxMicPull)) { memset(out, 0, frames * 2 * sizeof(float)); return; }
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
    std::wstring cerr;
    ChannelRuntime* chp = &ch;
    // A render endpoint bound as a channel source is tapped via WASAPI loopback —
    // lets a channel mirror any output device, and makes the graph testable
    // without a working third-party cable engine.
    bool ok = ch.capture.Start(ep->id, ep->isRender, [this, chp](const float* f, size_t n) {
        try {
            if (!chp->ready.load(std::memory_order_acquire)) return;
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
        if (frames > m_maxMixPull) m_maxMixPull = frames;
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
                continue;
            }
            if (!ch.cushionDone) {
                if (ch.ring.Depth() < CushionFor(m_maxMixPull)) {
                    // Healthy, and deliberately silent while the cushion fills.
                    // Pushed as a real zero rather than skipped, so a held peak
                    // from before a dropout expires on schedule instead of
                    // being frozen by the gap that follows it.
                    ch.peakHold.Push(0.0f, frames);
                    ch.peakPub.store(ch.peakHold.Value(), std::memory_order_relaxed);
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
            ch.peakHold.Push(BlockPeak(m_chanBuf.data(), frames), frames);
            ch.peakPub.store(ch.peakHold.Value(), std::memory_order_relaxed);
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
    m_baseCushion = rate * 3 / 100;
    m_maxMixPull = m_maxStreamPull = 0;
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
    StartPersonalRender(want);
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
        for (auto& ch : m_channels)
            if (!ch->healthy.load() || ch->capture.Invalidated()) {
                ch->capture.Stop();
                StartChannelCapture(*ch, eps);
            }
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

        // The whole device set reading absent at once is the audio graph being
        // rebuilt, not every device being unplugged in the same tick. Acting
        // on that snapshot moves the route at the one moment no move can
        // succeed, and adds churn to a graph that is already rebuilding
        // (MDropDX12 #410). Hold, and let it lift when the devices come back.
        //
        // The hold is a CEILING. mdx12 drives this from an AUDIODG process
        // watch and releases as soon as the engine is back and steady; the
        // same release happens here the first tick that sees a device again.
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
    for (const auto& c : m_channels)
        out.push_back({ c->id, c->name, c->healthy.load(),
                        c->pvol, c->pmute, c->svol, c->smute, c->eqEnabled,
                        c->peakPub.load(std::memory_order_relaxed) });
    return out;
}

DiagState Engine::GetDiag() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    DiagState d;
    for (const auto& c : m_channels)
        d.rings.push_back({ c->id, c->ring.Depth(), c->ring.Drops(), c->ring.Underruns() });
    if (m_streamEnabled)
        d.rings.push_back({ L"stream", m_streamRing.Depth(), m_streamRing.Drops(), m_streamRing.Underruns() });
    if (m_micEnabled)
        d.rings.push_back({ L"mic", m_micRing.Depth(), m_micRing.Drops(), m_micRing.Underruns() });
    d.personalDevice = m_currentPersonalId;
    d.personalFallback = m_personalFallback.load();
    d.maxMixPull = m_maxMixPull;
    return d;
}

} // namespace mdxm
