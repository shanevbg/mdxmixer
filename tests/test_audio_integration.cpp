#include "test_framework.h"
#include "engine/capture_stream.h"
#include "engine/render_stream.h"
#include "engine/engine.h"
#include "dsp/ring_buffer.h"
#include <windows.h>
#include <cmath>
#include <vector>

using namespace mdxm;

static std::wstring EnvW(const wchar_t* name) {
    wchar_t buf[512] = {};
    GetEnvironmentVariableW(name, buf, 512);
    return buf;
}

// Amplitude of the `freq` component over [startFrame, endFrame) — Goertzel bin.
// Frequency-selective on purpose: live endpoints on a working machine carry
// program audio, so broadband RMS can't isolate the test tone.
static double GoertzelAmp(const float* interleaved, size_t startFrame, size_t endFrame,
                          double freq, double rate) {
    double w = 2.0 * 3.14159265358979 * freq / rate;
    double c = 2.0 * std::cos(w), s1 = 0.0, s2 = 0.0;
    size_t n = endFrame - startFrame;
    for (size_t i = startFrame; i < endFrame; ++i) {
        double s0 = (double)interleaved[i*2] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re*re + im*im) / (double)n;
}

MDXM_TEST_CASE(Audio_ToneThroughCableRoundTrips) {
    // Requires a VB-CABLE (or similar): MDXM_TEST_RENDER = cable render endpoint id,
    // MDXM_TEST_CAPTURE = the same cable's capture endpoint id. Set by the operator;
    // mdxmixer.exe --devices prints endpoint ids.
    // MDXM_TEST_LOOPBACK=1: capture the RENDER endpoint itself via WASAPI loopback
    // (MDXM_TEST_CAPTURE = the render id). Proves the streams end-to-end through the
    // Windows audio engine alone — works on a machine with no cable installed.
    std::wstring renderId = EnvW(L"MDXM_TEST_RENDER"), captureId = EnvW(L"MDXM_TEST_CAPTURE");
    bool loopback = EnvW(L"MDXM_TEST_LOOPBACK") == L"1";
    if (renderId.empty() || captureId.empty()) { std::printf("skip (no cable env)\n"); return; }

    // Capture side first, into a big ring.
    RingBuffer ring(48000 * 5);
    CaptureStream cap;
    std::wstring err;
    CHECK(cap.Start(captureId, loopback, [&](const float* f, size_t n) { ring.Write(f, n); }, &err));

    // Render a 440 Hz tone into the cable for ~2 s.
    RenderStream ren;
    double phase = 0.0;
    CHECK(ren.Start(renderId, [&](float* out, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            float s = 0.5f * (float)std::sin(phase);
            phase += 2.0 * 3.14159265358979 * 440.0 / (double)ren.DeviceRate();
            out[i*2] = s; out[i*2+1] = s;
        }
    }, &err));
    Sleep(2000);
    ren.Stop();
    cap.Stop();

    // Drain and measure the 440 Hz bin: amplitude ~0.5 regardless of whatever
    // else the machine happens to be playing into this endpoint.
    std::vector<float> got(2 * 48000 * 3);
    size_t frames = ring.Read(got.data(), 48000 * 3);
    CHECK(frames > 48000);                                   // at least 1 s made it through
    size_t start = frames / 4, end = frames * 3 / 4;         // steady-state window
    double amp = GoertzelAmp(got.data(), start, end, 440.0, (double)cap.SourceRate());
    CHECK_NEAR(amp, 0.5, 0.1);
}

MDXM_TEST_CASE(Audio_EngineToneGameToStreaming) {
    // Full graph: tone -> channel capture -> ring/EQ/gains -> streaming sum ->
    // streaming cable render; verified by capturing the streaming cable's other side.
    // Env: MDXM_TEST_RENDER/CAPTURE = channel cable pair, MDXM_TEST_RENDER2/CAPTURE2 =
    // streaming cable pair. With MDXM_TEST_LOOPBACK=1 the CAPTURE ids may equal the
    // RENDER ids (loopback taps), so the test runs with no cable installed.
    // MDXM_TEST_PERSONAL optionally names the personal output; defaults to the system
    // default render device (personal volume is 0 — it renders silence).
    std::wstring renderId = EnvW(L"MDXM_TEST_RENDER"), captureId = EnvW(L"MDXM_TEST_CAPTURE");
    std::wstring render2Id = EnvW(L"MDXM_TEST_RENDER2"), capture2Id = EnvW(L"MDXM_TEST_CAPTURE2");
    bool loopback = EnvW(L"MDXM_TEST_LOOPBACK") == L"1";
    if (renderId.empty() || captureId.empty() || render2Id.empty() || capture2Id.empty()) {
        std::printf("skip (no cable env)\n");
        return;
    }
    std::wstring personalId = EnvW(L"MDXM_TEST_PERSONAL");
    if (personalId.empty()) personalId = DefaultRenderEndpointId();

    MixerConfig cfg;
    ChannelConfig ch;
    ch.id = L"game"; ch.name = L"Game";
    ch.cable.capture = { captureId, L"" };
    ch.personal  = { 0.0f, false };            // nothing audible on the personal mix
    ch.streaming = { 1.0f, false };
    cfg.channels.push_back(ch);
    cfg.personalOutput = { personalId, L"" };
    cfg.streamingCable.render = { render2Id, L"" };

    Engine eng;
    std::wstring err;
    CHECK(eng.Start(cfg, &err));
    if (!err.empty()) wprintf(L"engine notes: %s\n", err.c_str());
    auto states = eng.GetChannelStates();
    CHECK(states.size() == 1 && states[0].healthy);

    // A Bluetooth clock master takes seconds to negotiate its link before it
    // starts pulling; let the graph reach steady state before the windows open.
    Sleep(2500);

    // Verification capture of the streaming cable's far side.
    RingBuffer vring(48000 * 6);
    CaptureStream vcap;
    CHECK(vcap.Start(capture2Id, loopback, [&](const float* f, size_t n) { vring.Write(f, n); }, &err));

    // 440 Hz tone into the channel cable.
    RenderStream tone;
    double phase = 0.0;
    CHECK(tone.Start(renderId, [&](float* out, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            float s = 0.5f * (float)std::sin(phase);
            phase += 2.0 * 3.14159265358979 * 440.0 / (double)tone.DeviceRate();
            out[i*2] = s; out[i*2+1] = s;
        }
    }, &err));

    Sleep(3000);
    {   // stage-by-stage visibility when something starves
        DiagState d = eng.GetDiag();
        for (const auto& r : d.rings)
            wprintf(L"diag ring %s: depth=%zu drops=%llu underruns=%llu\n",
                    r.id.c_str(), r.depth, (unsigned long long)r.drops, (unsigned long long)r.underruns);
        wprintf(L"diag personal=%s fallback=%d maxMixPull=%zu\n",
                d.personalDevice.c_str(), d.personalFallback ? 1 : 0, d.maxMixPull);
    }
    eng.SetVolume(L"game", Mix::Streaming, 0.0f);   // ramped mute of the streaming mix
    Sleep(1000);
    tone.Stop();
    vcap.Stop();
    eng.Stop();

    std::vector<float> got(2 * 48000 * 6);
    size_t frames = vring.Read(got.data(), 48000 * 6);
    uint32_t rate = vcap.SourceRate() ? vcap.SourceRate() : 48000;
    CHECK(frames > (size_t)(3 * rate));
    for (size_t i = 0; i < frames; ++i)
        if (std::fabs(got[i*2]) > 0.1f) {
            wprintf(L"tone onset at %.3f s of capture\n", (double)i / rate);
            break;
        }

    // Window A: tone at streaming vol 1.0 (1.0 s .. 2.5 s). The 440 Hz bin
    // isolates the tone from any program audio riding the same endpoints.
    size_t a0 = rate, a1 = (size_t)(2.5 * rate);
    if (a1 > frames) a1 = frames;
    double ampA = GoertzelAmp(got.data(), a0, a1, 440.0, (double)rate);
    CHECK_NEAR(ampA, 0.5, 0.15);                            // unity through the engine, within 30 %

    // Window B: after the streaming mute (last 0.5 s) — the tone is GONE.
    size_t b0 = frames > rate / 2 ? frames - rate / 2 : 0;
    double ampB = GoertzelAmp(got.data(), b0, frames, 440.0, (double)rate);
    wprintf(L"tone bin: A=%.4f B=%.4f\n", ampA, ampB);
    CHECK(ampB < 0.03);
}

MDXM_TEST_CASE(Audio_FailoverCommitCallbackCanReadEngine) {
    // The commit callback is the app layer's hook: it rewrites config and
    // broadcasts the new route to subscribed IPC clients. Invoking it while the
    // engine lock is held makes any read-back from it (GetChannelStates,
    // GetDiag) a same-thread relock — which throws on this STL and hangs on
    // others. The exception then vanishes into TickFailover's catch(...) and
    // the subscription push never happens, exactly when MDropDX12 most needs to
    // hear that the route moved. Renders silence: no channels, no tone.
    std::wstring allowId = EnvW(L"MDXM_TEST_RENDER2");
    if (allowId.empty()) { std::printf("skip (no cable env)\n"); return; }

    MixerConfig cfg;
    cfg.personalOutput = { L"{no-such-device}", L"No Such Device" };   // never present
    cfg.personalFailover.armed = true;
    cfg.personalFailover.stabilitySec = 0;
    cfg.personalFailover.dwellSec = 0;
    cfg.personalFailover.allow.push_back({ allowId, L"" });

    Engine eng;
    std::wstring err;
    CHECK(eng.Start(cfg, &err));

    bool committed = false;
    std::wstring committedTo;
    eng.SetFailoverCommitCallback([&](const DeviceRef& d) {
        eng.GetChannelStates();      // re-enters the engine lock; must not throw
        eng.GetDiag();
        committedTo = d.id;
        committed = true;
    });

    eng.TickFailover();              // bound device missing -> temp fallback
    Sleep(50);
    eng.TickFailover();              // stability 0, candidate present -> commit
    eng.Stop();

    CHECK(committed);
    CHECK(committedTo == allowId);
}

MDXM_TEST_CASE(Audio_PersonalRouteChangeKeepsChannelsAlive) {
    // Review Focus #5, the half the PickPersonalOutput unit test cannot reach:
    // the personal render is the clock master, so moving it to a device with a
    // different mix rate changes the rate the whole graph consumes at. If the
    // channel resamplers are not reconfigured, captures keep producing at the
    // old rate while MixPull consumes at the new one — the rings drain (or
    // overflow) and every channel goes permanently silent. "Never silent,
    // never stuck" has to survive a route move, a failover commit, and a
    // Devices-tab Apply alike.
    // MDXM_TEST_PERSONAL and MDXM_TEST_PERSONAL2 must differ in mix rate
    // (mdxmixer.exe --devices prints rates).
    std::wstring renderId = EnvW(L"MDXM_TEST_RENDER"), captureId = EnvW(L"MDXM_TEST_CAPTURE");
    std::wstring render2Id = EnvW(L"MDXM_TEST_RENDER2"), capture2Id = EnvW(L"MDXM_TEST_CAPTURE2");
    std::wstring p1 = EnvW(L"MDXM_TEST_PERSONAL"), p2 = EnvW(L"MDXM_TEST_PERSONAL2");
    bool loopback = EnvW(L"MDXM_TEST_LOOPBACK") == L"1";
    if (renderId.empty() || captureId.empty() || render2Id.empty() || capture2Id.empty() ||
        p1.empty() || p2.empty()) {
        std::printf("skip (no cable env / no second personal endpoint)\n");
        return;
    }
    uint32_t r1 = EndpointMixRate(p1), r2 = EndpointMixRate(p2);
    std::printf("personal rates: %u Hz -> %u Hz\n", r1, r2);
    if (r1 == r2) std::printf("note: rates match, the reconfigure path is not exercised\n");

    MixerConfig cfg;
    ChannelConfig ch;
    ch.id = L"game";
    ch.cable.capture = { captureId, L"" };
    ch.personal  = { 0.0f, false };        // personal renders silence throughout
    ch.streaming = { 1.0f, false };
    cfg.channels.push_back(ch);
    cfg.personalOutput = { p1, L"" };
    cfg.streamingCable.render = { render2Id, L"" };

    Engine eng;
    std::wstring err;
    CHECK(eng.Start(cfg, &err));
    if (!err.empty()) wprintf(L"engine notes: %s\n", err.c_str());

    RenderStream tone;
    double phase = 0.0;
    CHECK(tone.Start(renderId, [&](float* out, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            float s = 0.5f * (float)std::sin(phase);
            phase += 2.0 * 3.14159265358979 * 440.0 / (double)tone.DeviceRate();
            out[i*2] = s; out[i*2+1] = s;
        }
    }, &err));
    Sleep(1500);

    CHECK(eng.SetPersonalOutput(p2));      // the route move under test
    Sleep(500);                            // let the graph settle on the new clock

    RingBuffer vring(48000 * 6);
    CaptureStream vcap;
    CHECK(vcap.Start(capture2Id, loopback, [&](const float* f, size_t n) { vring.Write(f, n); }, &err));
    Sleep(2500);
    tone.Stop();
    vcap.Stop();
    DiagState d = eng.GetDiag();
    eng.Stop();

    std::vector<float> got(2 * 48000 * 6);
    size_t frames = vring.Read(got.data(), 48000 * 6);
    uint32_t rate = vcap.SourceRate() ? vcap.SourceRate() : 48000;
    CHECK(frames > rate);
    size_t a0 = rate / 2, a1 = frames > rate / 4 ? frames - rate / 4 : frames;
    double amp = GoertzelAmp(got.data(), a0, a1, 440.0, (double)rate);
    std::printf("after route change, tone bin = %.4f\n", amp);
    for (const auto& r : d.rings)
        wprintf(L"  ring %s depth=%zu drops=%llu underruns=%llu\n",
                r.id.c_str(), r.depth, (unsigned long long)r.drops, (unsigned long long)r.underruns);
    CHECK(amp > 0.25);                     // channels still feeding the mix, not starved silent
}

MDXM_TEST_CASE(Audio_EngineRestartStress) {
    // Engine::Start must not let the mix render thread observe a half-built
    // graph. The render is started first, so MixPull can run while the scratch
    // buffers are still being assigned (null-range fill) and while channels are
    // being push_back'd (the vector reallocates under the range-for, leaving
    // moved-from null unique_ptrs to dereference). Channel construction does COM
    // device activation, so the window is tens of milliseconds on every Start —
    // every Devices-tab Apply and every failover-armed toggle. Silent test: no
    // tone is rendered, the personal mix sums to nothing.
    std::wstring captureId = EnvW(L"MDXM_TEST_CAPTURE"), render2Id = EnvW(L"MDXM_TEST_RENDER2");
    if (captureId.empty() || render2Id.empty()) { std::printf("skip (no cable env)\n"); return; }
    bool loopback = EnvW(L"MDXM_TEST_LOOPBACK") == L"1";
    (void)loopback;
    std::wstring personalId = EnvW(L"MDXM_TEST_PERSONAL");
    if (personalId.empty()) personalId = DefaultRenderEndpointId();

    MixerConfig cfg;
    for (int i = 0; i < 3; ++i) {          // three channels widens the push_back window
        ChannelConfig ch;
        ch.id = L"ch" + std::to_wstring(i);
        ch.cable.capture = { captureId, L"" };
        ch.personal = { 0.0f, false };
        ch.streaming = { 1.0f, false };
        cfg.channels.push_back(ch);
    }
    cfg.personalOutput = { personalId, L"" };
    cfg.streamingCable.render = { render2Id, L"" };

    for (int round = 0; round < 15; ++round) {
        Engine eng;
        std::wstring err;
        CHECK(eng.Start(cfg, &err));
        auto states = eng.GetChannelStates();
        CHECK(states.size() == 3);
        for (const auto& s : states) CHECK(s.healthy);
        Sleep(40);                          // let the mix thread pull across the window
        DiagState d = eng.GetDiag();
        CHECK(d.rings.size() >= 3);
        eng.Stop();
    }
    std::printf("15 engine start/stop rounds survived\n");
}

MDXM_TEST_CASE(Audio_DriftCountersStayNearZero) {
    // The lessons-note soak: generous rings + cushions must absorb clock drift;
    // a runaway drop/underrun counter means the sizing is wrong. Uses the same
    // env endpoints as the graph test. 20 s by default; MDXM_TEST_SOAK=1 runs
    // the full 3 minutes with the full threshold.
    std::wstring renderId = EnvW(L"MDXM_TEST_RENDER"), captureId = EnvW(L"MDXM_TEST_CAPTURE");
    std::wstring render2Id = EnvW(L"MDXM_TEST_RENDER2");
    if (renderId.empty() || captureId.empty() || render2Id.empty()) {
        std::printf("skip (no cable env)\n");
        return;
    }
    bool fullSoak = EnvW(L"MDXM_TEST_SOAK") == L"1";
    DWORD durationMs = fullSoak ? 180000 : 20000;
    uint64_t limit = fullSoak ? 20 : 5;
    std::wstring personalId = EnvW(L"MDXM_TEST_PERSONAL");
    if (personalId.empty()) personalId = DefaultRenderEndpointId();

    MixerConfig cfg;
    ChannelConfig ch;
    ch.id = L"game";
    ch.cable.capture = { captureId, L"" };
    ch.personal = { 0.0f, false };
    ch.streaming = { 1.0f, false };
    cfg.channels.push_back(ch);
    cfg.personalOutput = { personalId, L"" };
    cfg.streamingCable.render = { render2Id, L"" };

    Engine eng;
    std::wstring err;
    CHECK(eng.Start(cfg, &err));

    RenderStream tone;
    double phase = 0.0;
    CHECK(tone.Start(renderId, [&](float* out, size_t frames) {
        for (size_t i = 0; i < frames; ++i) {
            float s = 0.5f * (float)std::sin(phase);
            phase += 2.0 * 3.14159265358979 * 440.0 / (double)tone.DeviceRate();
            out[i*2] = s; out[i*2+1] = s;
        }
    }, &err));

    Sleep(5000);                                    // warm-up: BT link + cushion adaptation
    DiagState base = eng.GetDiag();
    Sleep(durationMs);
    DiagState endd = eng.GetDiag();
    tone.Stop();
    eng.Stop();

    for (const auto& r : endd.rings) {
        uint64_t baseEvents = 0;
        for (const auto& b : base.rings)
            if (b.id == r.id) { baseEvents = b.drops + b.underruns; break; }
        uint64_t events = (r.drops + r.underruns) - baseEvents;
        wprintf(L"soak ring %s: +%llu events over %lu ms\n",
                r.id.c_str(), (unsigned long long)events, durationMs);
        CHECK(events < limit);
    }
}
