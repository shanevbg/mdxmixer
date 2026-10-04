# mdxmixer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `mdxmixer.exe` — a native Windows tray-app mixer (channels over virtual cables, per-channel EQ, independent personal/streaming volumes, processed virtual mic, named-pipe IPC) that replaces SteelSeries Sonar's flaky GUI/API layers.

**Architecture:** One user-mode C++17 Win32 process. WASAPI shared-mode event-driven streams: per-channel cable capture → SPSC ring → shared EQ → per-mix gains → personal sum (renders to the physical output, which paces the whole graph) and streaming sum (renders into a dedicated cable). Mic chain is the same pattern reversed. Pure DSP/config/protocol modules are headless-unit-tested; COM/WASAPI modules follow the shapes proven in the MDropDX12 passthrough prototype.

**Tech Stack:** C++17, Win32/WASAPI/COM, MSBuild (VS2022) + `build.ps1`, no external dependencies (JSON utils adapted from MDropDX12's self-contained `json_utils`).

**Spec:** `docs/specs/2026-09-22-mdxmixer-design.md` — read it first; this plan argues from it. Also read `docs/notes/2026-09-22-passthrough-monitor-and-wasapi-lessons.md` — it pins the engine traps (pre-fill cushion, no level-based feedback guard, unity render, drop-oldest/zero-fill ring).

## Global Constraints

- C++17, Win32 API, **x64 only**, wide strings (`std::wstring`/`wchar_t`) throughout.
- Namespace `mdxm`; `PascalCase` functions; `m_camelCase` members (MDropDX12 conventions).
- Static CRT in Release (`<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`); `MultiThreadedDebugDLL` in Debug (mirrors MDropDX12's engine.vcxproj).
- **No-crash rule:** top-level SEH + `catch (const std::exception&)` + `catch (...)` on every thread entry point. A failure degrades and reports; it never crashes and never silences already-flowing audio.
- No external libraries. No registry writes except the HKCU Run key, written only when the user ticks autostart.
- Config is one JSON file beside the exe (`mdxmixer.json`); writes are atomic (temp + rename).
- Audio internals: WASAPI shared mode, event-driven, float32 stereo interleaved internally, 48 kHz steady state, linear-interpolation resampler fallback for mismatched rates.
- Every device binding stores **endpoint id + display name**; match id-first, exact-name fallback.
- Build: `powershell -ExecutionPolicy Bypass -File build.ps1 <Debug|Release|Test> x64` from repo root. Test config produces `src/mdxmixer/Test_x64/mdxmixer_test.exe` (console app).
- Commit style: conventional commits (`feat:`, `test:`, `chore:`, `docs:`), matching the repo's existing history.

## Review Focus

Spec-implied input classes no task would otherwise exercise. Each line's test has been added to the owning task, in that task's style.

1. **Corrupt or truncated `mdxmixer.json` at startup** — the app must start with defaults and log, never crash or refuse to launch. → Task 7 test `Config_GarbageFallsBackToDefaults`.
2. **A cable left at 44.1 kHz while the engine runs 48 kHz** — the resampler fallback path must produce a clean tone, not garbage or refusal. → Task 6 test `Resampler_441to48_TonePreserved`.
3. **Malformed or hostile IPC message** (unknown verb, missing fields, `vol=1.7`, out-of-range band index) — error reply and clamping, never a crash, never an unclamped gain reaching the engine. → Task 8 tests `Protocol_MalformedInputsRejected`, `Protocol_VolumeClamped`.
4. **EQ band with extreme parameters** (freq ≥ Nyquist, Q ≤ 0, gain beyond ±24 dB) — parameters clamp and the filter stays stable (impulse response decays). → Task 4 tests `Peaking_ClampsInsaneParams`, `Peaking_StableAtExtremes`.
5. **Personal output device disappears mid-stream** — fall back to the Windows default render device, say so, keep audio flowing. The pure fallback decision is unit-tested; the live path is a scripted manual check. → Task 11 test `FallbackOutput_PicksDefaultWhenBoundGone`; Task 14 Step 6 manual check 5.

## File Structure

```text
mdxmixer.sln
build.ps1
.gitignore
src/mdxmixer/
  mdxmixer.vcxproj              # Debug|Release|Test, all x64
  main.cpp                      # WinMain (guarded #ifndef MDXM_TEST), SEH wrapper, single-instance
  app/app_controller.h/.cpp     # IMixerControl impl: owns config+engine+ipc wiring, one funnel for UI and IPC
  app/log.h/.cpp                # level-gated log to log/mdxmixer.log
  app/autostart.h/.cpp          # HKCU Run key toggle
  config/json_utils.h/.cpp      # adapted from MDropDX12 (namespace mdxm)
  config/config.h/.cpp          # config model structs, from/to JSON, atomic save, deferred flush
  dsp/ring_buffer.h             # SPSC stereo float ring (header-only)
  dsp/gain_ramp.h               # ~10ms linear ramp (header-only)
  dsp/biquad.h/.cpp             # RBJ peaking biquad, DF2T, stereo, param clamping; ChannelEq cascade
  dsp/limiter.h                 # soft limiter (header-only)
  dsp/resampler.h               # linear interpolator with fractional carry (header-only)
  ipc/protocol.h/.cpp           # record grammar parse/build, MDXM_* dispatch against IMixerControl
  ipc/pipe_server.h/.cpp        # \\.\pipe\mdxmixer duplex message-mode server (simplified MDropDX12 shape)
  ipc/mixer_control.h           # IMixerControl pure interface (no Windows types)
  device/endpoints.h/.cpp       # enumeration, EndpointInfo, MatchBinding (pure), ParseMixFormat (pure)
  device/device_watcher.h/.cpp  # IMMNotificationClient → callback
  engine/capture_stream.h/.cpp  # WASAPI capture thread → stereo float callback
  engine/render_stream.h/.cpp   # WASAPI event-driven render, pull callback
  engine/engine.h/.cpp          # graph: channels, mixes, mic chain, diag, fallback logic
  routing/audio_policy_config.h/.cpp  # IAudioPolicyConfigFactory wrapper (Win10+Win11 IIDs)
  routing/sessions.h/.cpp       # IAudioSessionManager2 sweep: what plays where
  ui/tray_icon.h/.cpp           # Shell_NotifyIcon + menu
  ui/main_window.h/.cpp         # tab host: Mixer / Routing / EQ / Devices
  ui/tab_mixer.h/.cpp  ui/tab_routing.h/.cpp  ui/tab_eq.h/.cpp  ui/tab_devices.h/.cpp
tests/
  test_framework.h              # tiny registry + CHECK macros
  test_main.cpp                 # main() (guarded #ifdef MDXM_TEST); --audio gate for cable tests
  test_ring_buffer.cpp  test_gain_ramp.cpp  test_biquad.cpp  test_limiter.cpp
  test_resampler.cpp    test_config.cpp     test_protocol.cpp test_endpoints.cpp
  test_engine_logic.cpp test_pipe.cpp       test_audio_integration.cpp
```

Dependency direction: `dsp/` and `ipc/protocol` and `config/` depend on nothing Windows-specific (protocol sees only `ipc/mixer_control.h`). `engine/` depends on `dsp/` + `device/`. `app/` wires everything. UI talks only to `IMixerControl` + snapshots.

---

### Task 1: Scaffold — solution, project, build script, test harness

**Files:**
- Create: `mdxmixer.sln`, `src/mdxmixer/mdxmixer.vcxproj`, `build.ps1`, `.gitignore`, `src/mdxmixer/main.cpp`, `tests/test_framework.h`, `tests/test_main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: the build (`build.ps1 <Config> x64`), the test macro set (`MDXM_TEST_CASE`, `CHECK`, `CHECK_NEAR`), and the rule that Test config defines `MDXM_TEST`, builds SubSystem Console, TargetName `mdxmixer_test`, output dir `Test_x64\`. Every later task registers tests with `MDXM_TEST_CASE(Name) { ... }` and they run automatically.

- [ ] **Step 1: Copy and trim `build.ps1`** from `C:\Code\Entertainment\MDropDX12\build.ps1` — keep the vswhere/MSBuild discovery verbatim, point it at `mdxmixer.sln`, accept `Test` as a Configuration value. No Scintilla/external fetch stanzas.

- [ ] **Step 2: Write `.gitignore`**

```gitignore
Debug_x64/
Release_x64/
Test_x64/
*.user
.vs/
log/
mdxmixer.json
```

- [ ] **Step 3: Write the test framework** — `tests/test_framework.h`:

```cpp
#pragma once
// Minimal test registry. No external deps. Wide-string project, narrow test names are fine.
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace mdxm { namespace test {

struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); } };
inline int g_failures = 0;
inline const char* g_current = "";

#define MDXM_TEST_CASE(Name) \
    static void Test_##Name(); \
    static mdxm::test::Registrar reg_##Name(#Name, &Test_##Name); \
    static void Test_##Name()

#define CHECK(cond) do { if (!(cond)) { \
    ++mdxm::test::g_failures; \
    std::printf("FAIL %s: %s (%s:%d)\n", mdxm::test::g_current, #cond, __FILE__, __LINE__); } } while (0)

#define CHECK_NEAR(a, b, eps) do { double va=(double)(a), vb=(double)(b); if (std::fabs(va-vb) > (eps)) { \
    ++mdxm::test::g_failures; \
    std::printf("FAIL %s: %s=%g vs %s=%g (%s:%d)\n", mdxm::test::g_current, #a, va, #b, vb, __FILE__, __LINE__); } } while (0)

// Returns process exit code. audioTests: run tests whose name starts with "Audio_" (need cables).
inline int RunAll(bool audioTests) {
    int run = 0;
    for (auto& t : Registry()) {
        bool isAudio = std::string(t.name).rfind("Audio_", 0) == 0;
        if (isAudio != audioTests) continue;
        g_current = t.name;
        int before = g_failures;
        try { t.fn(); }
        catch (const std::exception& e) { ++g_failures; std::printf("FAIL %s: exception %s\n", t.name, e.what()); }
        catch (...) { ++g_failures; std::printf("FAIL %s: unknown exception\n", t.name); }
        ++run;
        if (g_failures == before) std::printf("ok   %s\n", t.name);
    }
    std::printf("%d tests, %d failures\n", run, g_failures);
    return g_failures == 0 ? 0 : 1;
}

}} // namespace mdxm::test
```

- [ ] **Step 4: Write `tests/test_main.cpp`**

```cpp
#ifdef MDXM_TEST
#include "test_framework.h"
#include <cstring>

int main(int argc, char** argv) {
    bool audio = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--audio") == 0) audio = true;
    return mdxm::test::RunAll(audio);
}
#endif
```

- [ ] **Step 5: Write `src/mdxmixer/main.cpp`** — WinMain stub, excluded from Test builds:

```cpp
#ifndef MDXM_TEST
#include <windows.h>

static int AppMain(HINSTANCE hInstance) {
    // Wired up in Task 15. For now prove the exe runs and exits.
    (void)hInstance;
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    __try {
        return AppMain(hInstance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3; // no-crash rule: swallow, exit nonzero. Logging attached in Task 15.
    }
}
#endif
```

- [ ] **Step 6: Write `src/mdxmixer/mdxmixer.vcxproj` and `mdxmixer.sln`.** Three x64 configurations. Use MDropDX12's `engine.vcxproj` as the reference for tool versions and the Release static-CRT setting. The essentials the project MUST have:
  - Debug: `MultiThreadedDebugDLL`, SubSystem Windows, TargetName `mdxmixer`, OutDir `Debug_x64\`.
  - Release: `MultiThreaded` (static CRT), SubSystem Windows, TargetName `mdxmixer`, OutDir `Release_x64\`.
  - Test: `MultiThreadedDebugDLL`, SubSystem **Console**, TargetName `mdxmixer_test`, OutDir `Test_x64\`, PreprocessorDefinitions include `MDXM_TEST`.
  - All: `<LanguageStandard>stdcpp17</LanguageStandard>`, `UNICODE;_UNICODE`, WarningLevel Level4, `AdditionalIncludeDirectories`: `$(ProjectDir);$(ProjectDir)..\..\tests`.
  - ClCompile items: `main.cpp`, `..\..\tests\test_main.cpp` (both compiled in every config; the `#ifdef` guards select which is live).

- [ ] **Step 7: Build all three configs and run the (empty) test exe**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Test x64` then `./src/mdxmixer/Test_x64/mdxmixer_test.exe`
Expected: `0 tests, 0 failures`, exit 0. Then `build.ps1 Debug x64` and `build.ps1 Release x64` both succeed.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "chore: scaffold solution, build script, test harness"
```

---

### Task 2: dsp/ring_buffer — SPSC stereo ring, drop-oldest, zero-fill

**Files:**
- Create: `src/mdxmixer/dsp/ring_buffer.h`
- Test: `tests/test_ring_buffer.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `mdxm::RingBuffer` — `explicit RingBuffer(size_t capacityFrames)`; `void Write(const float* interleaved, size_t frames)` (always accepts all frames, dropping oldest on overflow); `size_t Read(float* out, size_t frames)` (returns frames delivered from real data, zero-fills the rest); `size_t Depth() const`; `uint64_t Drops() const`; `uint64_t Underruns() const`; `void Clear()`. Frames are stereo interleaved (2 floats per frame). Single producer thread, single consumer thread, lock-free (two `std::atomic<size_t>` indices).

- [ ] **Step 1: Write the failing tests** — `tests/test_ring_buffer.cpp`:

```cpp
#include "test_framework.h"
#include "dsp/ring_buffer.h"
#include <vector>

using mdxm::RingBuffer;

MDXM_TEST_CASE(Ring_WriteReadRoundTrip) {
    RingBuffer rb(16);
    float in[8] = {1,2,3,4,5,6,7,8}; // 4 frames
    rb.Write(in, 4);
    CHECK(rb.Depth() == 4);
    float out[8] = {};
    CHECK(rb.Read(out, 4) == 4);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == in[i]);
    CHECK(rb.Depth() == 0);
}

MDXM_TEST_CASE(Ring_UnderflowZeroFillsAndCounts) {
    RingBuffer rb(16);
    float in[4] = {1,2,3,4};
    rb.Write(in, 2);
    float out[12] = {9,9,9,9,9,9,9,9,9,9,9,9};
    CHECK(rb.Read(out, 6) == 2);           // only 2 real frames
    CHECK(out[0] == 1 && out[3] == 4);
    for (int i = 4; i < 12; ++i) CHECK(out[i] == 0.0f); // zero-filled tail
    CHECK(rb.Underruns() == 1);
}

MDXM_TEST_CASE(Ring_OverflowDropsOldestAndCounts) {
    RingBuffer rb(4); // capacity 4 frames
    float a[8] = {1,1,2,2,3,3,4,4};
    rb.Write(a, 4);
    float b[4] = {5,5,6,6};
    rb.Write(b, 2);                        // must drop frames 1 and 2
    CHECK(rb.Drops() >= 2);
    CHECK(rb.Depth() == 4);
    float out[8] = {};
    rb.Read(out, 4);
    CHECK(out[0] == 3.0f);                 // oldest surviving frame is 3
    CHECK(out[6] == 6.0f);
}

MDXM_TEST_CASE(Ring_WriteLargerThanCapacityKeepsNewest) {
    RingBuffer rb(4);
    std::vector<float> big(20); // 10 frames: values 0..19
    for (int i = 0; i < 20; ++i) big[(size_t)i] = (float)i;
    rb.Write(big.data(), 10);
    CHECK(rb.Depth() == 4);
    float out[8] = {};
    rb.Read(out, 4);
    CHECK(out[0] == 12.0f);                // frames 6..9 survive
    CHECK(out[7] == 19.0f);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Test x64`
Expected: compile FAILURE — `dsp/ring_buffer.h` not found. (Add `test_ring_buffer.cpp` to the vcxproj ClCompile items first; every new test/source file in later tasks is added the same way without further mention.)

- [ ] **Step 3: Implement `src/mdxmixer/dsp/ring_buffer.h`**

```cpp
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
    std::atomic<size_t> m_read{0}, m_write{0};   // monotonic frame counters
    std::atomic<uint64_t> m_drops{0}, m_underruns{0};
};

} // namespace mdxm
```

Note the drop-in-Write nuance: when the producer advances `m_read` it races a concurrent `Read`. That race is benign for audio (worst case a frame is read twice or skipped once during an overflow — which is already an error state being counted), and both spec and lessons note accept it. Do not add a mutex.

- [ ] **Step 4: Run tests to verify they pass**

Run: `build.ps1 Test x64` then `./src/mdxmixer/Test_x64/mdxmixer_test.exe`
Expected: all `Ring_*` tests `ok`, 0 failures.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/dsp/ring_buffer.h tests/test_ring_buffer.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: SPSC ring buffer with drop-oldest/zero-fill and drift counters"
```

---

### Task 3: dsp/gain_ramp — click-free gain changes

**Files:**
- Create: `src/mdxmixer/dsp/gain_ramp.h`
- Test: `tests/test_gain_ramp.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `mdxm::GainRamp` — `void Configure(double sampleRate)` (sets per-sample step for a ~10 ms full-scale traverse); `void SetTarget(float gain)` (0..1+, mute = SetTarget(0)); `void SnapTo(float gain)` (no ramp, for stream start); `float Current() const`; `void Process(float* interleaved, size_t frames)` (multiplies in place, advancing toward target per frame — both channels of a frame get the same gain).

- [ ] **Step 1: Write the failing tests** — `tests/test_gain_ramp.cpp`:

```cpp
#include "test_framework.h"
#include "dsp/gain_ramp.h"
#include <cmath>
#include <vector>

using mdxm::GainRamp;

MDXM_TEST_CASE(Ramp_ReachesTargetWithin10msAndHolds) {
    GainRamp g;
    g.Configure(48000.0);
    g.SnapTo(0.0f);
    g.SetTarget(1.0f);
    std::vector<float> buf(2 * 480, 1.0f);      // 10 ms of ones
    g.Process(buf.data(), 480);
    CHECK_NEAR(g.Current(), 1.0f, 1e-4);
    CHECK_NEAR(buf[2 * 479], 1.0f, 1e-3);        // last frame at full gain
    std::vector<float> buf2(2 * 48, 1.0f);
    g.Process(buf2.data(), 48);
    for (auto v : buf2) CHECK_NEAR(v, 1.0f, 1e-6); // holds exactly at target
}

MDXM_TEST_CASE(Ramp_NoClicks_BoundedFirstDifference) {
    GainRamp g;
    g.Configure(48000.0);
    g.SnapTo(1.0f);
    g.SetTarget(0.0f);                           // a mute
    std::vector<float> buf(2 * 960, 1.0f);       // DC input exposes gain steps directly
    g.Process(buf.data(), 960);
    double maxStep = 0.0;
    for (size_t i = 1; i < 960; ++i)
        maxStep = std::fmax(maxStep, std::fabs((double)buf[i*2] - (double)buf[(i-1)*2]));
    CHECK(maxStep < 0.005);                      // ~1/480 per frame + slack; a hard mute would be 1.0
    CHECK_NEAR(g.Current(), 0.0f, 1e-4);
}

MDXM_TEST_CASE(Ramp_BothChannelsSameGain) {
    GainRamp g;
    g.Configure(48000.0);
    g.SnapTo(0.5f);
    float buf[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    g.Process(buf, 2);
    CHECK_NEAR(buf[0], -buf[1], 1e-6);
    CHECK_NEAR(buf[2], -buf[3], 1e-6);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `build.ps1 Test x64`
Expected: compile FAILURE — header not found.

- [ ] **Step 3: Implement `src/mdxmixer/dsp/gain_ramp.h`**

```cpp
#pragma once
// Linear per-frame gain ramp, ~10 ms for a full 0->1 traverse. Mute is SetTarget(0).
// Target is atomic: UI/IPC threads set it, the audio thread reads it.
#include <atomic>
#include <cmath>

namespace mdxm {

class GainRamp {
public:
    void Configure(double sampleRate) {
        m_step = (float)(1.0 / (0.010 * sampleRate)); // full-scale traverse in 10 ms
    }
    void SetTarget(float gain) { m_target.store(gain, std::memory_order_relaxed); }
    void SnapTo(float gain) { m_current = gain; m_target.store(gain, std::memory_order_relaxed); }
    float Current() const { return m_current; }

    void Process(float* interleaved, size_t frames) {
        float target = m_target.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) {
            if (m_current < target) m_current = std::fmin(m_current + m_step, target);
            else if (m_current > target) m_current = std::fmax(m_current - m_step, target);
            interleaved[i * 2]     *= m_current;
            interleaved[i * 2 + 1] *= m_current;
        }
    }

private:
    float m_current = 1.0f;
    float m_step = 1.0f / 480.0f;
    std::atomic<float> m_target{1.0f};
};

} // namespace mdxm
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `build.ps1 Test x64` then the test exe. Expected: all `Ramp_*` ok.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/dsp/gain_ramp.h tests/test_gain_ramp.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: click-free gain ramp"
```

---

### Task 4: dsp/biquad — RBJ peaking EQ, DF2T, clamped params, cascade

**Files:**
- Create: `src/mdxmixer/dsp/biquad.h`, `src/mdxmixer/dsp/biquad.cpp`
- Test: `tests/test_biquad.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct BiquadCoeffs { double b0, b1, b2, a1, a2; };` (normalized, a0 divided out)
  - `BiquadCoeffs MakePeaking(double sampleRate, double freqHz, double gainDb, double q);` — clamps freq to [10.0, 0.45*sampleRate], gainDb to [-24, +24], q to [0.1, 18.0] before computing (RBJ Audio EQ Cookbook peaking form).
  - `class StereoBiquad { void SetCoeffs(const BiquadCoeffs&); void Reset(); void Process(float* interleaved, size_t frames); };` — Direct Form II transposed, independent state per channel.
  - `class ChannelEq { void Configure(double sampleRate); void SetBand(size_t band, double freqHz, double gainDb, double q); void SetEnabled(bool); bool Enabled() const; void Process(float* interleaved, size_t frames); static constexpr size_t kBands = 10; };` — cascade of 10 StereoBiquads; bands with 0 dB gain are skipped; Process is a no-op passthrough when disabled.

- [ ] **Step 1: Write the failing tests** — `tests/test_biquad.cpp`. The magnitude probe drives a sine through the filter and compares RMS out/in after discarding a warm-up:

```cpp
#include "test_framework.h"
#include "dsp/biquad.h"
#include <cmath>
#include <cstring>
#include <vector>

using namespace mdxm;

namespace {
// Gain of `eq` at freqHz, measured by sine RMS ratio (mono content on both channels).
double MeasureGainDb(ChannelEq& eq, double freqHz, double fs) {
    const size_t warm = 4800, meas = 48000;
    std::vector<float> buf(2 * (warm + meas));
    for (size_t i = 0; i < warm + meas; ++i) {
        float s = (float)std::sin(2.0 * 3.14159265358979 * freqHz * (double)i / fs);
        buf[i*2] = s; buf[i*2+1] = s;
    }
    eq.Process(buf.data(), warm + meas);
    double sum = 0.0;
    for (size_t i = warm; i < warm + meas; ++i) sum += (double)buf[i*2] * buf[i*2];
    double rmsOut = std::sqrt(sum / meas);
    return 20.0 * std::log10(rmsOut / 0.70710678);
}
} // namespace

MDXM_TEST_CASE(Peaking_BoostsAtCenterOnly) {
    ChannelEq eq;
    eq.Configure(48000.0);
    eq.SetBand(0, 1000.0, 6.0, 1.0);
    eq.SetEnabled(true);
    CHECK_NEAR(MeasureGainDb(eq, 1000.0, 48000.0), 6.0, 0.3);
    ChannelEq eq2; eq2.Configure(48000.0); eq2.SetBand(0, 1000.0, 6.0, 1.0); eq2.SetEnabled(true);
    CHECK_NEAR(MeasureGainDb(eq2, 100.0, 48000.0), 0.0, 0.3);   // a decade away: flat
}

MDXM_TEST_CASE(Peaking_DisabledIsIdentity) {
    ChannelEq eq;
    eq.Configure(48000.0);
    eq.SetBand(0, 1000.0, 12.0, 4.0);
    eq.SetEnabled(false);
    float buf[8] = {0.5f, -0.5f, 0.25f, -0.25f, 1.0f, -1.0f, 0.0f, 0.0f};
    float ref[8]; std::memcpy(ref, buf, sizeof buf);
    eq.Process(buf, 4);
    for (int i = 0; i < 8; ++i) CHECK(buf[i] == ref[i]);         // bit-identical when bypassed
}

MDXM_TEST_CASE(Peaking_ClampsInsaneParams) {
    // Review Focus #4. Nyquist-and-beyond freq, absurd gain, zero Q: coefficients stay finite.
    BiquadCoeffs c = MakePeaking(48000.0, 96000.0, 60.0, 0.0);
    CHECK(std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2));
    CHECK(std::isfinite(c.a1) && std::isfinite(c.a2));
    BiquadCoeffs c2 = MakePeaking(48000.0, -5.0, -60.0, 1000.0);
    CHECK(std::isfinite(c2.b0) && std::isfinite(c2.a2));
}

MDXM_TEST_CASE(Peaking_StableAtExtremes) {
    // Impulse response of a clamped-extreme band must decay, not ring forever or blow up.
    ChannelEq eq;
    eq.Configure(48000.0);
    eq.SetBand(0, 96000.0, 60.0, 0.0);   // gets clamped internally
    eq.SetEnabled(true);
    std::vector<float> buf(2 * 48000, 0.0f);
    buf[0] = buf[1] = 1.0f;
    eq.Process(buf.data(), 48000);
    double tail = 0.0;
    for (size_t i = 47000; i < 48000; ++i) tail = std::fmax(tail, std::fabs((double)buf[i*2]));
    CHECK(tail < 1e-3);
    for (size_t i = 0; i < 48000; ++i) CHECK(std::fabs(buf[i*2]) < 100.0);
}
```

- [ ] **Step 2: Run tests to verify they fail** — compile failure, header not found.

- [ ] **Step 3: Implement.** `biquad.h` declares the interface above; `biquad.cpp` implements `MakePeaking` (RBJ cookbook):

```cpp
#include "biquad.h"
#include <algorithm>
#include <cmath>

namespace mdxm {

BiquadCoeffs MakePeaking(double sampleRate, double freqHz, double gainDb, double q) {
    freqHz = std::clamp(freqHz, 10.0, 0.45 * sampleRate);
    gainDb = std::clamp(gainDb, -24.0, 24.0);
    q      = std::clamp(q, 0.1, 18.0);
    const double A     = std::pow(10.0, gainDb / 40.0);
    const double w0    = 2.0 * 3.14159265358979323846 * freqHz / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha / A;
    BiquadCoeffs c;
    c.b0 = (1.0 + alpha * A) / a0;
    c.b1 = (-2.0 * cosw0) / a0;
    c.b2 = (1.0 - alpha * A) / a0;
    c.a1 = (-2.0 * cosw0) / a0;
    c.a2 = (1.0 - alpha / A) / a0;
    return c;
}

} // namespace mdxm
```

`StereoBiquad::Process` (DF2T, per channel state `z1, z2`):

```cpp
// per sample, per channel:
//   y  = b0*x + z1
//   z1 = b1*x - a1*y + z2
//   z2 = b2*x - a2*y
```

`ChannelEq` holds `std::array<StereoBiquad, kBands>` plus a parallel `std::array<bool, kBands> m_active` (band active when clamped gainDb != 0). `SetBand` computes coeffs via `MakePeaking`, stores them, marks active, and `Reset()`s that biquad. `Process` returns immediately when disabled (`std::atomic<bool> m_enabled`); otherwise runs only active bands in sequence. Cross-thread coefficient updates: a `std::mutex` taken in `SetBand`, `try_lock` in `Process` (on miss, use the previous coefficients this block — never block the audio thread). Comment that choice in the header.

- [ ] **Step 4: Run tests to verify they pass** — all `Peaking_*` ok.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/dsp/biquad.h src/mdxmixer/dsp/biquad.cpp tests/test_biquad.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: parametric peaking EQ (RBJ biquad cascade) with clamped params"
```

---

### Task 5: dsp/limiter — soft ceiling on mix sums

**Files:**
- Create: `src/mdxmixer/dsp/limiter.h`
- Test: `tests/test_limiter.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `mdxm::SoftLimiter` — `void Process(float* interleaved, size_t frames)`. Stateless waveshaper: identity below the knee `0.89125f` (−1 dBFS); above it, `sign(x) * (knee + (1−knee) * tanh((|x|−knee)/(1−knee)))`. Output magnitude strictly < 1.0 for any finite input.

- [ ] **Step 1: Write the failing tests** — `tests/test_limiter.cpp`:

```cpp
#include "test_framework.h"
#include "dsp/limiter.h"
#include <cmath>
#include <cstring>

using mdxm::SoftLimiter;

MDXM_TEST_CASE(Limiter_IdentityBelowKnee) {
    SoftLimiter lim;
    float buf[6] = {0.5f, -0.5f, 0.88f, -0.88f, 0.0f, 0.1f};
    float ref[6]; std::memcpy(ref, buf, sizeof buf);
    lim.Process(buf, 3);
    for (int i = 0; i < 6; ++i) CHECK(buf[i] == ref[i]);
}

MDXM_TEST_CASE(Limiter_CeilingNeverExceeded) {
    SoftLimiter lim;
    float buf[8] = {1.5f, -1.5f, 4.0f, -4.0f, 8.0f, -8.0f, 1.0f, -1.0f};
    lim.Process(buf, 4);
    for (int i = 0; i < 8; ++i) CHECK(std::fabs(buf[i]) < 1.0f);
}

MDXM_TEST_CASE(Limiter_Monotonic) {
    // Louder in must never come out quieter — no inversion artifacts at the knee.
    SoftLimiter lim;
    float prev = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        float buf[2] = {0.05f * i, 0.05f * i};
        lim.Process(buf, 1);
        CHECK(buf[0] >= prev);
        prev = buf[0];
    }
}
```

- [ ] **Step 2: Run tests to verify they fail** — compile failure, header not found.

- [ ] **Step 3: Implement `src/mdxmixer/dsp/limiter.h`**

```cpp
#pragma once
// Soft limiter: summing N channels at unity must not clip the device buffer.
// Waveshaper, not a lookahead compressor — zero latency, good enough at this gain staging.
#include <cmath>

namespace mdxm {

class SoftLimiter {
public:
    void Process(float* interleaved, size_t frames) {
        const float knee = 0.89125f; // -1 dBFS
        const size_t n = frames * 2;
        for (size_t i = 0; i < n; ++i) {
            float x = interleaved[i];
            float ax = std::fabs(x);
            if (ax <= knee) continue;
            float shaped = knee + (1.0f - knee) * std::tanh((ax - knee) / (1.0f - knee));
            interleaved[i] = x < 0 ? -shaped : shaped;
        }
    }
};

} // namespace mdxm
```

- [ ] **Step 4: Run tests to verify they pass** — all `Limiter_*` ok.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/dsp/limiter.h tests/test_limiter.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: soft limiter for mix sums"
```

---

### Task 6: dsp/resampler — linear interpolation with fractional carry

**Files:**
- Create: `src/mdxmixer/dsp/resampler.h`
- Test: `tests/test_resampler.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `mdxm::LinearResampler` — `void SetRates(double inRate, double outRate);` `bool IsPassthrough() const;` `size_t EstimateOut(size_t inFrames) const;` `size_t Process(const float* in, size_t inFrames, float* out, size_t maxOutFrames);` returns frames produced. Keeps fractional position and the last input frame across calls, so block seams are continuous. Same-rate is an exact `memcpy` (bit-identical).

- [ ] **Step 1: Write the failing tests** — `tests/test_resampler.cpp`:

```cpp
#include "test_framework.h"
#include "dsp/resampler.h"
#include <cmath>
#include <vector>

using mdxm::LinearResampler;

MDXM_TEST_CASE(Resampler_SameRateIsBitIdentical) {
    LinearResampler rs;
    rs.SetRates(48000.0, 48000.0);
    CHECK(rs.IsPassthrough());
    std::vector<float> in(2 * 100), out(2 * 104);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (float)std::sin(0.1 * (double)i);
    size_t produced = rs.Process(in.data(), 100, out.data(), 104);
    CHECK(produced == 100);
    for (size_t i = 0; i < 200; ++i) CHECK(out[i] == in[i]);   // exact copy
}

MDXM_TEST_CASE(Resampler_441to48_TonePreserved) {
    // Review Focus #2: a cable left at 44.1 kHz. A 1 kHz sine at 44.1 in must come out
    // a 1 kHz sine at 48 with the same RMS, fed in awkward 441-frame blocks.
    LinearResampler rs;
    rs.SetRates(44100.0, 48000.0);
    std::vector<float> outAll;
    size_t phase = 0;
    for (int block = 0; block < 100; ++block) {
        std::vector<float> in(2 * 441), out(2 * 512);
        for (size_t i = 0; i < 441; ++i) {
            float s = (float)std::sin(2.0 * 3.14159265358979 * 1000.0 * (double)(phase + i) / 44100.0);
            in[i*2] = s; in[i*2+1] = s;
        }
        phase += 441;
        size_t produced = rs.Process(in.data(), 441, out.data(), 512);
        outAll.insert(outAll.end(), out.begin(), out.begin() + produced * 2);
    }
    size_t total = outAll.size() / 2;
    CHECK(total > 47500 && total < 48500);                    // ~1 s at 48 kHz out
    double sum = 0.0;
    for (size_t i = 1000; i < total; ++i) sum += (double)outAll[i*2] * outAll[i*2];
    double rms = std::sqrt(sum / (double)(total - 1000));
    CHECK_NEAR(rms, 0.70710678, 0.02);                        // sine RMS survives resampling
    double maxJump = 0.0;                                     // continuity across block seams
    for (size_t i = 1; i < total; ++i)
        maxJump = std::fmax(maxJump, std::fabs((double)outAll[i*2] - (double)outAll[(i-1)*2]));
    CHECK(maxJump < 0.20);                                    // 1 kHz @48k moves ~0.13/sample max
}
```

- [ ] **Step 2: Run tests to verify they fail** — compile failure, header not found.

- [ ] **Step 3: Implement `src/mdxmixer/dsp/resampler.h`.** Model: output sample t (in input-frame units) interpolates between input frames floor(t) and floor(t)+1, where index −1 refers to the carried last frame of the previous block.

```cpp
#pragma once
// Linear-interpolation resampler with fractional carry across blocks.
// Same-rate is an exact copy, per the passthrough-monitor lessons (bit-identical check in tests).
#include <cmath>
#include <cstring>

namespace mdxm {

class LinearResampler {
public:
    void SetRates(double inRate, double outRate) {
        m_ratio = inRate / outRate;      // input frames per output frame
        m_passthrough = (inRate == outRate);
        m_pos = 0.0;
        m_havePrev = false;
    }
    bool IsPassthrough() const { return m_passthrough; }
    size_t EstimateOut(size_t inFrames) const {
        return m_passthrough ? inFrames : (size_t)std::ceil((double)inFrames / m_ratio) + 2;
    }

    size_t Process(const float* in, size_t inFrames, float* out, size_t maxOutFrames) {
        if (m_passthrough) {
            size_t n = inFrames < maxOutFrames ? inFrames : maxOutFrames;
            std::memcpy(out, in, n * 2 * sizeof(float));
            return n;
        }
        if (inFrames == 0) return 0;
        size_t produced = 0;
        while (produced < maxOutFrames) {
            long i0 = (long)std::floor(m_pos);
            double frac = m_pos - (double)i0;
            if (i0 + 1 >= (long)inFrames) break;             // need next frame; carry to next block
            float l0, r0;
            if (i0 < 0) {                                     // seam: previous block's last frame
                l0 = m_havePrev ? m_prev[0] : in[0];
                r0 = m_havePrev ? m_prev[1] : in[1];
            } else {
                l0 = in[i0*2]; r0 = in[i0*2+1];
            }
            float l1 = in[(i0+1)*2], r1 = in[(i0+1)*2+1];
            out[produced*2]   = (float)(l0 + (l1 - l0) * frac);
            out[produced*2+1] = (float)(r0 + (r1 - r0) * frac);
            ++produced;
            m_pos += m_ratio;
        }
        m_prev[0] = in[(inFrames-1)*2];
        m_prev[1] = in[(inFrames-1)*2+1];
        m_havePrev = true;
        m_pos -= (double)inFrames;                            // rebase for next block
        return produced;
    }

private:
    double m_ratio = 1.0;
    double m_pos = 0.0;
    bool m_passthrough = true;
    bool m_havePrev = false;
    float m_prev[2] = {0, 0};
};

} // namespace mdxm
```

The `i0 < 0` seam branch is what the test's `maxJump` continuity check exercises.

- [ ] **Step 4: Run tests to verify they pass** — both `Resampler_*` ok.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/dsp/resampler.h tests/test_resampler.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: linear resampler with fractional carry"
```

---

### Task 7: config — model, JSON round-trip, atomic save, corrupt-file fallback

**Files:**
- Create: `src/mdxmixer/config/json_utils.h`, `src/mdxmixer/config/json_utils.cpp` (adapted), `src/mdxmixer/config/config.h`, `src/mdxmixer/config/config.cpp`
- Test: `tests/test_config.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (all in `namespace mdxm`):

```cpp
struct DeviceRef   { std::wstring id, name; };
struct CableRef    { DeviceRef render, capture; };
struct MixSetting  { float vol = 1.0f; bool mute = false; };
struct EqBandConfig{ double freq = 1000.0, gainDb = 0.0, q = 1.0; };
struct EqConfig    { bool enabled = false; std::vector<EqBandConfig> bands; };
struct ChannelConfig {
    std::wstring id, name;
    CableRef cable;
    MixSetting personal, streaming;
    EqConfig eq;
    std::vector<std::wstring> apps;      // exe paths assigned to this channel
};
struct MicConfig   { DeviceRef input; CableRef cable; float gain = 1.0f; EqConfig eq; };
struct FailoverConfig { bool armed = false; std::vector<DeviceRef> allow;   // ordered: first present wins
                        int stabilitySec = 5; int dwellSec = 30; };
struct UiConfig    { bool taskbarButton = false; };   // JSON: "ui": { "taskbarButton": … }
struct MixerConfig {
    std::vector<ChannelConfig> channels;
    DeviceRef personalOutput;
    FailoverConfig personalFailover;
    CableRef  streamingCable;
    MicConfig mic;
    UiConfig  ui;
    bool autostart = false;
    int  logLevel = 2;
};
MixerConfig  ConfigFromJson(const JsonValue& root);   // missing/wrong-typed fields -> defaults
std::wstring ConfigToJson(const MixerConfig& c);      // pretty-printed
MixerConfig  LoadConfig(const std::wstring& path, bool* usedDefaults); // never throws
bool         SaveConfigAtomic(const std::wstring& path, const MixerConfig& c); // temp + MoveFileExW
```

Also `class ConfigStore` (owns a MixerConfig + path; `Mutate(fn)` applies a change and schedules a flush; `FlushIfDue()` writes at most once per second, called from the UI timer; `FlushNow()` on exit). Volumes are clamped to [0, 1] and EQ band count to ≤ `ChannelEq::kBands` at load time — bad config never reaches the engine unclamped.

- [ ] **Step 1: Copy `json_utils.h/.cpp`** from `C:\Code\Entertainment\MDropDX12\src\mDropDX12\` into `src/mdxmixer/config/`, change `namespace mdrop` → `namespace mdxm`, drop nothing else. (572+121 lines, already self-contained: JsonValue DOM, JsonParse with `//` comments, JsonWriter, JsonLoadFile/JsonSaveFile.)

- [ ] **Step 2: Write the failing tests** — `tests/test_config.cpp`:

```cpp
#include "test_framework.h"
#include "config/config.h"
#include <windows.h>

using namespace mdxm;

static MixerConfig SampleConfig() {
    MixerConfig c;
    ChannelConfig ch;
    ch.id = L"game"; ch.name = L"Game";
    ch.cable.render  = { L"{render-guid}",  L"CABLE-A Input" };
    ch.cable.capture = { L"{capture-guid}", L"CABLE-A Output" };
    ch.personal  = { 0.85f, false };
    ch.streaming = { 1.0f,  true };
    ch.eq.enabled = true;
    ch.eq.bands.push_back({ 62.5, 3.0, 1.4 });
    ch.apps.push_back(L"C:/Games/game.exe");
    c.channels.push_back(ch);
    c.personalOutput = { L"{hp-guid}", L"Headphones" };
    c.streamingCable.render = { L"{sc-guid}", L"CABLE-B Input" };
    c.mic.input = { L"{mic-guid}", L"Microphone" };
    c.mic.cable.render = { L"{mc-guid}", L"CABLE-C Input" };
    c.mic.gain = 0.9f;
    c.personalFailover.armed = true;
    c.personalFailover.allow.push_back({ L"{spk-guid}", L"Speakers" });
    c.personalFailover.stabilitySec = 7;
    c.personalFailover.dwellSec = 45;
    c.ui.taskbarButton = true;
    c.autostart = true;
    c.logLevel = 3;
    return c;
}

MDXM_TEST_CASE(Config_RoundTripPreservesEverything) {
    MixerConfig a = SampleConfig();
    MixerConfig b = ConfigFromJson(JsonParse(ConfigToJson(a)));
    CHECK(b.channels.size() == 1);
    CHECK(b.channels[0].id == L"game");
    CHECK(b.channels[0].cable.capture.name == L"CABLE-A Output");
    CHECK_NEAR(b.channels[0].personal.vol, 0.85f, 1e-6);
    CHECK(b.channels[0].streaming.mute == true);
    CHECK(b.channels[0].eq.enabled == true);
    CHECK_NEAR(b.channels[0].eq.bands.at(0).q, 1.4, 1e-9);
    CHECK(b.channels[0].apps.at(0) == L"C:/Games/game.exe");
    CHECK(b.personalOutput.name == L"Headphones");
    CHECK_NEAR(b.mic.gain, 0.9f, 1e-6);
    CHECK(b.personalFailover.armed == true);
    CHECK(b.personalFailover.allow.at(0).name == L"Speakers");
    CHECK(b.personalFailover.stabilitySec == 7);
    CHECK(b.personalFailover.dwellSec == 45);
    CHECK(b.ui.taskbarButton == true);
    CHECK(b.autostart == true);
    CHECK(b.logLevel == 3);
}

MDXM_TEST_CASE(Config_GarbageFallsBackToDefaults) {
    // Review Focus #1: corrupt/truncated file must yield defaults, never a crash.
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    std::wstring file = std::wstring(path) + L"mdxm_bad.json";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr; WriteFile(h, "{\"channels\": [{\"id\": \"ga", 24, &wr, nullptr);
    CloseHandle(h);
    bool usedDefaults = false;
    MixerConfig c = LoadConfig(file, &usedDefaults);
    CHECK(usedDefaults);
    CHECK(c.channels.empty());
    CHECK(c.logLevel == 2);
    DeleteFileW(file.c_str());
}

MDXM_TEST_CASE(Config_MissingFileUsesDefaults) {
    bool usedDefaults = false;
    MixerConfig c = LoadConfig(L"Z:/definitely/not/here.json", &usedDefaults);
    CHECK(usedDefaults);
    CHECK(c.autostart == false);
}

MDXM_TEST_CASE(Config_LoadClampsHostileValues) {
    JsonValue root = JsonParse(LR"({"channels":[{"id":"x","personal":{"vol":7.5},"streaming":{"vol":-2.0}}]})");
    MixerConfig c = ConfigFromJson(root);
    CHECK_NEAR(c.channels.at(0).personal.vol, 1.0f, 1e-6);
    CHECK_NEAR(c.channels.at(0).streaming.vol, 0.0f, 1e-6);
}

MDXM_TEST_CASE(Config_AtomicSaveRoundTrips) {
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    std::wstring file = std::wstring(path) + L"mdxm_save.json";
    CHECK(SaveConfigAtomic(file, SampleConfig()));
    bool usedDefaults = true;
    MixerConfig c = LoadConfig(file, &usedDefaults);
    CHECK(!usedDefaults);
    CHECK(c.channels.size() == 1);
    DeleteFileW(file.c_str());
}
```

- [ ] **Step 3: Run tests to verify they fail** — compile failure.

- [ ] **Step 4: Implement `config.cpp`.** `ConfigFromJson` walks the DOM with the accessor-with-default pattern (`root[L"logLevel"].asInt(2)` etc.), clamping `vol` via `std::clamp(v, 0.0f, 1.0f)` and truncating `eq.bands` to `10`. `ConfigToJson` uses `JsonWriter`. `LoadConfig`: `JsonLoadFile`; if the result is not an Object (parse failed / missing file), set `*usedDefaults = true` and return `MixerConfig{}`. `SaveConfigAtomic`: write to `path + L".tmp"` then `MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. `ConfigStore::FlushIfDue` compares `GetTickCount64()` against last-mutate + 1000 ms.

- [ ] **Step 5: Run tests to verify they pass** — all `Config_*` ok.

- [ ] **Step 6: Commit**

```bash
git add src/mdxmixer/config tests/test_config.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: config model with JSON round-trip, atomic save, hostile-input clamping"
```

---

### Task 8: ipc/protocol — record grammar and MDXM_* dispatch (headless)

**Files:**
- Create: `src/mdxmixer/ipc/mixer_control.h`, `src/mdxmixer/ipc/protocol.h`, `src/mdxmixer/ipc/protocol.cpp`
- Test: `tests/test_protocol.cpp`

**Interfaces:**
- Consumes: config structs from Task 7 (for state snapshots).
- Produces:

```cpp
// ipc/mixer_control.h — the seam between protocol/UI and the app. No Windows types.
namespace mdxm {
enum class Mix { Personal, Streaming };
struct ChannelState { std::wstring id, name; bool healthy;
                      float pvol; bool pmute; float svol; bool smute; bool eqOn; };
struct DiagState    { struct Ring { std::wstring id; size_t depth; uint64_t drops, underruns; };
                      std::vector<Ring> rings; std::wstring personalDevice; bool personalFallback; };
class IMixerControl {
public:
    virtual ~IMixerControl() = default;
    virtual std::vector<ChannelState> GetChannels() = 0;
    virtual std::vector<std::pair<std::wstring, std::wstring>> GetRoutes() = 0; // id -> endpointId
    virtual std::vector<std::tuple<std::wstring, std::wstring, bool, bool>> GetDevices() = 0; // id, name, isRender, active
    virtual bool SetVolume(const std::wstring& ch, Mix m, float vol) = 0;       // clamped by callee
    virtual bool SetMute(const std::wstring& ch, Mix m, bool mute) = 0;
    virtual bool SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q) = 0;
    virtual bool EnableEq(const std::wstring& ch, bool on) = 0;
    virtual bool AssignApp(const std::wstring& exePath, const std::wstring& chOrDash) = 0;
    virtual bool SetPersonalRoute(const std::wstring& endpointId) = 0;
    virtual DiagState GetDiag() = 0;
};
} // namespace mdxm

// ipc/protocol.h
namespace mdxm {
struct Record { std::wstring verb; std::vector<std::pair<std::wstring, std::wstring>> fields;
                const std::wstring* Find(const std::wstring& key) const; };
Record       ParseRecord(const std::wstring& msg);       // splits on '|', first token is verb (verb may be VERB=payload)
std::wstring BuildRecord(const std::wstring& verb, std::initializer_list<std::pair<std::wstring, std::wstring>> fields);
// Returns every reply message for one incoming message, in order. Never throws.
std::vector<std::wstring> HandleProtocolMessage(const std::wstring& msg, IMixerControl& ctl, bool* wantSubscribe);
constexpr wchar_t kProtocolVersion[] = L"1";
}
```

Grammar per spec: `VERB|field=value|…`; `MDXM_SET=<ch>|<personal|streaming>|<0..1>` style verbs carry positional payload after `=` then `|`-separated args. Chunked replies: `MDXM_BEGIN`, then records, then `MDXM_END`. Errors: `MDXM_ERR|msg=<text>`.

- [ ] **Step 1: Write the failing tests** — `tests/test_protocol.cpp`, with a scriptable fake:

```cpp
#include "test_framework.h"
#include "ipc/protocol.h"
#include <algorithm>

using namespace mdxm;

namespace {
struct FakeControl : IMixerControl {
    float lastVol = -1; Mix lastMix = Mix::Personal; std::wstring lastCh;
    bool eqOn = false; size_t eqBand = 99; double eqF = 0, eqG = 0, eqQ = 0;
    std::wstring assignedExe, assignedCh, route;
    std::vector<ChannelState> GetChannels() override {
        return {{L"game", L"Game", true, 0.85f, false, 1.0f, false, true}};
    }
    std::vector<std::pair<std::wstring, std::wstring>> GetRoutes() override {
        return {{L"personal", L"{hp-guid}"}};
    }
    std::vector<std::tuple<std::wstring, std::wstring, bool, bool>> GetDevices() override {
        return {{L"{hp-guid}", L"Headphones", true, true}};
    }
    bool SetVolume(const std::wstring& ch, Mix m, float vol) override {
        lastCh = ch; lastMix = m; lastVol = vol; return ch == L"game";
    }
    bool SetMute(const std::wstring&, Mix, bool) override { return true; }
    bool SetEqBand(const std::wstring&, size_t b, double f, double g, double q) override {
        eqBand = b; eqF = f; eqG = g; eqQ = q; return true;
    }
    bool EnableEq(const std::wstring&, bool on) override { eqOn = on; return true; }
    bool AssignApp(const std::wstring& exe, const std::wstring& ch) override {
        assignedExe = exe; assignedCh = ch; return true;
    }
    bool SetPersonalRoute(const std::wstring& ep) override { route = ep; return true; }
    DiagState GetDiag() override {
        DiagState d; d.rings.push_back({L"game", 480, 3, 1}); d.personalDevice = L"{hp-guid}"; return d;
    }
};
bool Contains(const std::vector<std::wstring>& v, const std::wstring& needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::wstring& s) {
        return s.find(needle) != std::wstring::npos; });
}
} // namespace

MDXM_TEST_CASE(Protocol_PingPong) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_PING", f, &sub);
    CHECK(r.size() == 1);
    CHECK(r[0].rfind(L"MDXM_PONG|version=", 0) == 0);
}

MDXM_TEST_CASE(Protocol_StateIsChunkedAndTerminated) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_STATE", f, &sub);
    CHECK(r.front() == L"MDXM_BEGIN");
    CHECK(r.back() == L"MDXM_END");                       // the MDropDX12 chunking contract
    CHECK(Contains(r, L"MDXM_CHAN|id=game|name=Game|health=ok|pvol=0.85|pmute=0|svol=1|smute=0|eq=1"));
    CHECK(Contains(r, L"MDXM_ROUTE|id=personal|device={hp-guid}"));
    CHECK(Contains(r, L"MDXM_DEV|id={hp-guid}"));
}

MDXM_TEST_CASE(Protocol_SetVolumeParsesAndDispatches) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_SET=game|streaming|0.4", f, &sub);
    CHECK(f.lastCh == L"game");
    CHECK(f.lastMix == Mix::Streaming);
    CHECK_NEAR(f.lastVol, 0.4f, 1e-6);
    CHECK(Contains(r, L"MDXM_CHAN"));                     // optimistic echo reply
}

MDXM_TEST_CASE(Protocol_VolumeClamped) {
    // Review Focus #3: out-of-range volume must reach the control clamped.
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_SET=game|personal|1.7", f, &sub);
    CHECK_NEAR(f.lastVol, 1.0f, 1e-6);
    HandleProtocolMessage(L"MDXM_SET=game|personal|-3", f, &sub);
    CHECK_NEAR(f.lastVol, 0.0f, 1e-6);
}

MDXM_TEST_CASE(Protocol_MalformedInputsRejected) {
    // Review Focus #3: junk must produce MDXM_ERR, never a crash or a dispatch.
    FakeControl f; bool sub = false;
    for (const wchar_t* bad : { L"", L"BOGUS_VERB|x=1", L"MDXM_SET=", L"MDXM_SET=game",
                                L"MDXM_SET=game|sideways|0.5", L"MDXM_SET=game|personal|purple",
                                L"MDXM_EQ_SET=game|999|100|0|1", L"MDXM_MUTE=game|personal" }) {
        auto r = HandleProtocolMessage(bad, f, &sub);
        CHECK(!r.empty());
        CHECK(r[0].rfind(L"MDXM_ERR|", 0) == 0);
    }
    CHECK(f.lastVol == -1.0f || f.lastVol == 0.0f);        // the sideways/purple ones never dispatched
}

MDXM_TEST_CASE(Protocol_MuteParsesAndDispatches) {
    FakeControl f; bool sub = false;
    auto r = HandleProtocolMessage(L"MDXM_MUTE=game|personal|1", f, &sub);
    CHECK(f.lastCh.empty());                              // SetMute, not SetVolume, was called
    CHECK(Contains(r, L"MDXM_CHAN"));                     // optimistic echo reply
}

MDXM_TEST_CASE(Protocol_EqSetAndEnable) {
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_EQ_SET=game|3|250|4.5|1.2", f, &sub);
    CHECK(f.eqBand == 3);
    CHECK_NEAR(f.eqF, 250.0, 1e-9);
    CHECK_NEAR(f.eqG, 4.5, 1e-9);
    CHECK_NEAR(f.eqQ, 1.2, 1e-9);
    HandleProtocolMessage(L"MDXM_EQ_ENABLE=game|1", f, &sub);
    CHECK(f.eqOn);
}

MDXM_TEST_CASE(Protocol_AssignRouteSubscribeDiag) {
    FakeControl f; bool sub = false;
    HandleProtocolMessage(L"MDXM_ASSIGN=C:/Games/game.exe|game", f, &sub);
    CHECK(f.assignedExe == L"C:/Games/game.exe" && f.assignedCh == L"game");
    HandleProtocolMessage(L"MDXM_ROUTE_SET=personal|{usb-guid}", f, &sub);
    CHECK(f.route == L"{usb-guid}");
    HandleProtocolMessage(L"MDXM_SUBSCRIBE=1", f, &sub);
    CHECK(sub);
    auto d = HandleProtocolMessage(L"MDXM_DIAG", f, &sub);
    CHECK(Contains(d, L"MDXM_RING|id=game|depth=480|drops=3|underruns=1"));
}
```

- [ ] **Step 2: Run tests to verify they fail** — compile failure.

- [ ] **Step 3: Implement `protocol.cpp`.** `ParseRecord`: split on `|`; the first token splits once on `=` into verb + first positional arg; remaining tokens are positional args (no `=`) or `key=value` fields. `HandleProtocolMessage` is one dispatcher: look up the verb, validate arity and value ranges (`std::wcstod` with end-pointer checks — a non-numeric volume is malformed, not 0), clamp volume to [0,1] and band index to < 10, call the control, build replies. Unknown verb / bad arity / bad number → `MDXM_ERR|msg=...` naming the problem. Volume formatting in replies: `%g` (0.85 not 0.850000). Wrap the whole handler body in try/catch returning `MDXM_ERR|msg=internal` (no-crash rule).

- [ ] **Step 4: Run tests to verify they pass** — all `Protocol_*` ok.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/ipc tests/test_protocol.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: MDXM pipe protocol - grammar, dispatch, hostile-input rejection"
```

---

### Task 9: device — enumeration, mix-format parsing, binding matcher, watcher

**Files:**
- Create: `src/mdxmixer/device/endpoints.h`, `src/mdxmixer/device/endpoints.cpp`, `src/mdxmixer/device/device_watcher.h`, `src/mdxmixer/device/device_watcher.cpp`
- Test: `tests/test_endpoints.cpp`

**Interfaces:**
- Consumes: `DeviceRef` from Task 7.
- Produces:

```cpp
namespace mdxm {
struct EndpointInfo { std::wstring id, name; bool isRender = false; bool isActive = false; };
std::vector<EndpointInfo> EnumerateEndpoints();  // both flows, DEVICE_STATE_ACTIVE|UNPLUGGED
// Pure. Id match wins; else exact (case-insensitive) name match; else nullptr.
// The Bluetooth re-pair lesson: a re-paired device returns under a NEW id with the SAME name.
const EndpointInfo* MatchBinding(const std::vector<EndpointInfo>& eps, const DeviceRef& want);

struct StreamFormat { uint32_t rate = 0; uint16_t channels = 0;
                      enum class Sample { F32, I16, Unsupported } sample = Sample::Unsupported; };
StreamFormat ParseMixFormat(const WAVEFORMATEX* wfx);   // pure over the struct bytes

class DeviceWatcher {  // IMMNotificationClient wrapper
public:
    using Callback = std::function<void()>;             // "device set changed" — debounced by caller
    bool Start(Callback onChange);                       // registers with the enumerator
    void Stop();
};
} // namespace mdxm
```

- [ ] **Step 1: Write the failing tests** — `tests/test_endpoints.cpp` (the pure parts only; enumeration itself is exercised by the `Audio_` integration tests and the app):

```cpp
#include "test_framework.h"
#include "device/endpoints.h"
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

using namespace mdxm;

MDXM_TEST_CASE(Match_IdWinsOverName) {
    std::vector<EndpointInfo> eps = {
        {L"{id-1}", L"Headphones", true, true},
        {L"{id-2}", L"Headphones", true, true},
    };
    const EndpointInfo* m = MatchBinding(eps, {L"{id-2}", L"Headphones"});
    CHECK(m && m->id == L"{id-2}");
}

MDXM_TEST_CASE(Match_NameFallbackWhenIdGone) {
    // Re-paired Bluetooth: new id, same name.
    std::vector<EndpointInfo> eps = { {L"{id-new}", L"WF-1000XM6", true, true} };
    const EndpointInfo* m = MatchBinding(eps, {L"{id-old}", L"WF-1000XM6"});
    CHECK(m && m->id == L"{id-new}");
    const EndpointInfo* none = MatchBinding(eps, {L"{id-old}", L"Different Name"});
    CHECK(none == nullptr);
}

MDXM_TEST_CASE(Match_NameCompareIsCaseInsensitiveExact) {
    std::vector<EndpointInfo> eps = { {L"{a}", L"CABLE-A Input (VB-Audio)", true, true} };
    CHECK(MatchBinding(eps, {L"{gone}", L"cable-a input (vb-audio)"}) != nullptr);
    CHECK(MatchBinding(eps, {L"{gone}", L"CABLE-A"}) == nullptr);  // substring is NOT a match
}

MDXM_TEST_CASE(Format_ParsesFloatPcmAndRefusesElse) {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 2;
    f.nSamplesPerSec = 48000; f.wBitsPerSample = 32;
    StreamFormat sf = ParseMixFormat(&f);
    CHECK(sf.sample == StreamFormat::Sample::F32 && sf.rate == 48000 && sf.channels == 2);

    f.wFormatTag = WAVE_FORMAT_PCM; f.wBitsPerSample = 16;
    CHECK(ParseMixFormat(&f).sample == StreamFormat::Sample::I16);

    WAVEFORMATEXTENSIBLE e = {};
    e.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; e.Format.nChannels = 2;
    e.Format.nSamplesPerSec = 44100; e.Format.wBitsPerSample = 32;
    e.Format.cbSize = sizeof(e) - sizeof(e.Format);
    e.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    StreamFormat sfe = ParseMixFormat(&e.Format);
    CHECK(sfe.sample == StreamFormat::Sample::F32 && sfe.rate == 44100);

    e.SubFormat = KSDATAFORMAT_SUBTYPE_ALAW;                      // exotic: refuse
    CHECK(ParseMixFormat(&e.Format).sample == StreamFormat::Sample::Unsupported);

    f.wFormatTag = WAVE_FORMAT_PCM; f.wBitsPerSample = 24;        // 24-bit PCM: refuse (spec: float32/int16 only)
    CHECK(ParseMixFormat(&f).sample == StreamFormat::Sample::Unsupported);
}
```

- [ ] **Step 2: Run tests to verify they fail** — compile failure.

- [ ] **Step 3: Implement.**
  - `MatchBinding`: id compare exact; name compare `_wcsicmp == 0`. Prefer active endpoints when several share a name.
  - `ParseMixFormat`: switch on `wFormatTag`; for `WAVE_FORMAT_EXTENSIBLE` check `cbSize >= 22` then compare `SubFormat` against `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` / `KSDATAFORMAT_SUBTYPE_PCM` (16-bit only). Anything else → `Unsupported` (the caller refuses to start that stream and reports, never writes garbage bytes — lessons note).
  - `EnumerateEndpoints`: `CoCreateInstance(__uuidof(MMDeviceEnumerator))` → `EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED)` → per device: `GetId`, `OpenPropertyStore` → `PKEY_Device_FriendlyName`, and flow from `IMMEndpoint::GetDataFlow`. All COM failures skip that device; the function never throws.
  - `DeviceWatcher`: an `IMMNotificationClient` impl whose every callback (`OnDeviceStateChanged`, `OnDeviceAdded`, `OnDeviceRemoved`, `OnDefaultDeviceChanged`) invokes the stored callback. Ref-counted properly (`AddRef/Release/QueryInterface`), registered via `RegisterEndpointNotificationCallback`, unregistered in `Stop`. Spec rule: device arrival/loss comes from these events, never a poll.

- [ ] **Step 4: Run tests to verify they pass** — all `Match_*`/`Format_*` ok.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/device tests/test_endpoints.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: endpoint enumeration, binding matcher, mix-format parsing, device watcher"
```

---

### Task 10: engine streams + the passthrough milestone (first audible audio)

**Files:**
- Create: `src/mdxmixer/engine/capture_stream.h`, `src/mdxmixer/engine/capture_stream.cpp`, `src/mdxmixer/engine/render_stream.h`, `src/mdxmixer/engine/render_stream.cpp`
- Modify: `src/mdxmixer/main.cpp` (add `--monitor` CLI mode)

**Interfaces:**
- Consumes: `RingBuffer`, `LinearResampler`, `ParseMixFormat`, `StreamFormat`.
- Produces:

```cpp
namespace mdxm {
// Capture thread. Decodes every packet to interleaved stereo float at the SOURCE rate
// and hands it to the callback (mono duplicated, >2ch keeps first two — lessons note).
class CaptureStream {
public:
    using OnFrames = std::function<void(const float* interleaved, size_t frames)>;
    // loopback=true captures a render endpoint's output (used by nothing in v1's graph,
    // but the flag exists because the same class serves either endpoint direction).
    bool Start(const std::wstring& endpointId, bool loopback, OnFrames cb, std::wstring* err);
    void Stop();
    uint32_t SourceRate() const;      // valid after Start
};

// Event-driven render. Each event: GetCurrentPadding -> write bufFrames-padding frames
// pulled from the callback -> convert to the device mix format -> ReleaseBuffer.
class RenderStream {
public:
    using PullFrames = std::function<void(float* interleaved, size_t frames)>;
    bool Start(const std::wstring& endpointId, PullFrames cb, std::wstring* err);
    void Stop();
    uint32_t DeviceRate() const;
    bool Invalidated() const;         // set when AUDCLNT_E_DEVICE_INVALIDATED was seen
};
} // namespace mdxm
```

Both classes: own thread (`_beginthreadex`), thread body wrapped in try/catch (no-crash rule), `AvSetMmThreadCharacteristicsW(L"Pro Audio", ...)` on the audio threads, COM initialized per thread (`COINIT_MULTITHREADED`). Shared mode, `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, 10 ms buffer request (`Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, wfx, nullptr)`). Formats come from `GetMixFormat` + `ParseMixFormat`; `Unsupported` → fail Start with a message. On `AUDCLNT_E_DEVICE_INVALIDATED` in the loop: set the invalidated flag, exit the thread cleanly — the engine reacts (Task 11).

- [ ] **Step 1: No headless test exists for WASAPI itself.** Write the audio-gated integration test scaffold now — `tests/test_audio_integration.cpp` with the first `Audio_` test that loops a tone through real endpoints IF the env var `MDXM_TEST_RENDER` and `MDXM_TEST_CAPTURE` name a cable pair (skip cleanly otherwise):

```cpp
#include "test_framework.h"
#include "engine/capture_stream.h"
#include "engine/render_stream.h"
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

MDXM_TEST_CASE(Audio_ToneThroughCableRoundTrips) {
    // Requires a VB-CABLE (or similar): MDXM_TEST_RENDER = cable render endpoint id,
    // MDXM_TEST_CAPTURE = the same cable's capture endpoint id. Set by the operator; see
    // Task 16 for the helper that prints endpoint ids.
    std::wstring renderId = EnvW(L"MDXM_TEST_RENDER"), captureId = EnvW(L"MDXM_TEST_CAPTURE");
    if (renderId.empty() || captureId.empty()) { std::printf("skip (no cable env)\n"); return; }

    // Capture side first, into a big ring.
    RingBuffer ring(48000 * 5);
    CaptureStream cap;
    std::wstring err;
    CHECK(cap.Start(captureId, false, [&](const float* f, size_t n) { ring.Write(f, n); }, &err));

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

    // Drain and measure: RMS ~ 0.5/sqrt(2), dominant zero-crossing rate ~ 440 Hz.
    std::vector<float> got(2 * 48000 * 3);
    size_t frames = ring.Read(got.data(), 48000 * 3);
    CHECK(frames > 48000);                                   // at least 1 s made it through
    size_t start = frames / 4, end = frames * 3 / 4;         // steady-state window
    double sum = 0.0; int crossings = 0;
    for (size_t i = start; i < end; ++i) {
        sum += (double)got[i*2] * got[i*2];
        if (i > start && (got[i*2] >= 0) != (got[(i-1)*2] >= 0)) ++crossings;
    }
    double rms = std::sqrt(sum / (double)(end - start));
    CHECK_NEAR(rms, 0.3535, 0.05);
    double seconds = (double)(end - start) / (double)cap.SourceRate();
    CHECK_NEAR(crossings / seconds / 2.0, 440.0, 10.0);
}
```

- [ ] **Step 2: Verify the scaffold fails to compile** — `build.ps1 Test x64`, expect header-not-found.

- [ ] **Step 3: Implement `CaptureStream`.** Thread loop shape (from the passthrough prototype): `Initialize` with event callback (add `AUDCLNT_STREAMFLAGS_LOOPBACK` when `loopback`; loopback capture on some Windows builds doesn't signal the event reliably — use a 5 ms `WaitForSingleObject` timeout and drain regardless); `GetService(IAudioCaptureClient)`; loop: wait event/timeout → `GetNextPacketSize` → while nonzero: `GetBuffer` → convert (source format per `ParseMixFormat`: F32 copy / I16 scale by 1/32768; mono → duplicate; >2 ch → first two; `AUDCLNT_BUFFERFLAGS_SILENT` → zeros) → `cb(stereoFloat, frames)` → `ReleaseBuffer`.

- [ ] **Step 4: Implement `RenderStream`.** `Initialize` event-driven → `SetEventHandle` → `GetService(IAudioRenderClient)` → prefill one full buffer of silence → `Start`. Loop: wait event → `GetCurrentPadding` → `n = bufFrames - padding` → `GetBuffer(n)` → `cb(scratch, n)` into a float scratch, convert scratch to device format (F32 memcpy / I16 clamp-scale; device channels ≠ 2: duplicate L/R across pairs or downmix first two) → `ReleaseBuffer(n, 0)`.

- [ ] **Step 5: Add the `--monitor` milestone mode to `main.cpp`** (console-attached via `AttachConsole`/`AllocConsole` when the flag is present). This is the lessons note's "hear it working before building the graph":

```cpp
// mdxmixer.exe --monitor <captureIdOrName> <renderIdOrName>
// Capture -> 100 ms prefill cushion -> ring -> resample if rates differ -> unity render.
// REFUSES to start when the two ids resolve to the same endpoint (feedback guard; the
// structural one — there is deliberately NO level-based runaway detector).
static int RunMonitor(const std::wstring& capSel, const std::wstring& renSel) {
    auto eps = mdxm::EnumerateEndpoints();
    const mdxm::EndpointInfo* cap = mdxm::MatchBinding(eps, {capSel, capSel});
    const mdxm::EndpointInfo* ren = mdxm::MatchBinding(eps, {renSel, renSel});
    if (!cap || !ren) { wprintf(L"endpoint not found\n"); return 1; }
    if (_wcsicmp(cap->id.c_str(), ren->id.c_str()) == 0) { wprintf(L"refusing: capture == render (feedback)\n"); return 1; }

    mdxm::RingBuffer ring(48000 / 5);                  // ~200 ms
    mdxm::CaptureStream cs; mdxm::RenderStream rs;
    std::wstring err;
    if (!cs.Start(cap->id, false, [&](const float* f, size_t n) { ring.Write(f, n); }, &err)) { wprintf(L"cap: %s\n", err.c_str()); return 1; }
    mdxm::LinearResampler rsmp;
    bool cushionDone = false;
    const size_t cushion = 48000 / 10;                 // ~100 ms prefill before first real audio
    std::vector<float> scratch(2 * 4096);
    if (!rs.Start(ren->id, [&](float* out, size_t frames) {
            if (!cushionDone) {
                if (ring.Depth() < cushion) { memset(out, 0, frames * 2 * sizeof(float)); return; }
                cushionDone = true;
                rsmp.SetRates((double)cs.SourceRate(), (double)rs.DeviceRate());
            }
            if (rsmp.IsPassthrough()) { ring.Read(out, frames); return; }
            size_t srcNeed = (size_t)std::ceil((double)frames * cs.SourceRate() / rs.DeviceRate()) + 1;
            ring.Read(scratch.data(), srcNeed);
            size_t produced = rsmp.Process(scratch.data(), srcNeed, out, frames);
            for (size_t i = produced; i < frames; ++i) {   // pad shortfall with last frame
                out[i*2]   = produced ? out[(produced-1)*2]   : 0.0f;
                out[i*2+1] = produced ? out[(produced-1)*2+1] : 0.0f;
            }
        }, &err)) { wprintf(L"ren: %s\n", err.c_str()); return 1; }
    wprintf(L"monitoring — Ctrl+C to stop. drops/underruns print every 5 s\n");
    for (;;) { Sleep(5000); wprintf(L"depth=%zu drops=%llu under=%llu\n", ring.Depth(), ring.Drops(), ring.Underruns()); }
}
```

The unity-gain rule is absolute: no gain multiply anywhere in this path — listening level is the output device's own volume, the decisive correction in the lessons note.

- [ ] **Step 6: Build and run the milestone by ear**

Run: `build.ps1 Debug x64`, then `./src/mdxmixer/Debug_x64/mdxmixer.exe --monitor "<mic or cable capture name>" "<headphone name>"` on the dev machine.
Expected: clean audio at unity, no crackle (the cushion at work), counters near zero over a minute. **This is a manual gate — do not proceed until it sounds clean.** If no cable is installed yet, monitor the physical mic to the headphones.

- [ ] **Step 7: Run the audio integration test (if a cable exists on this machine)**

Run: set `MDXM_TEST_RENDER`/`MDXM_TEST_CAPTURE` to a cable's endpoint ids (Task 16's `--devices` helper prints them; until then use `--monitor`'s not-found error listing or PowerShell `Get-AudioDevice`), then `mdxmixer_test.exe --audio`.
Expected: `Audio_ToneThroughCableRoundTrips` ok, or a clean printed skip when the env vars are absent.

- [ ] **Step 8: Commit**

```bash
git add src/mdxmixer/engine src/mdxmixer/main.cpp tests/test_audio_integration.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: WASAPI capture/render streams + passthrough monitor milestone"
```

---

### Task 11: engine — the mix graph and failover decider

**Files:**
- Create: `src/mdxmixer/engine/engine.h`, `src/mdxmixer/engine/engine.cpp`, `src/mdxmixer/engine/failover.h`, `src/mdxmixer/engine/failover.cpp`
- Test: `tests/test_engine_logic.cpp` (pure parts), one more `Audio_` case in `tests/test_audio_integration.cpp`

**Interfaces:**
- Consumes: everything from Tasks 2–6, 9, 10; config structs from Task 7.
- Produces:

```cpp
namespace mdxm {
struct ChannelRuntime {                     // one per configured channel
    std::wstring id;
    bool healthy = false;                   // cable found and capture running
    RingBuffer ring{48000 / 5};             // ~200 ms
    LinearResampler resampler;              // capture rate -> 48k
    ChannelEq eq;
    GainRamp personalGain, streamingGain;   // mute = ramp target 0 (engine keeps vol+mute separately
                                            //  and sets target = mute ? 0 : vol)
    CaptureStream capture;
};

class Engine {
public:
    // Builds channels + mic chain from config against the live endpoint list.
    // Missing cable => that channel unhealthy (with a message), engine still starts. Never throws.
    bool Start(const MixerConfig& cfg, std::wstring* err);
    void Stop();

    // Control surface (thread-safe; called from UI and IPC threads):
    bool SetVolume(const std::wstring& ch, Mix m, float vol01);
    bool SetMute(const std::wstring& ch, Mix m, bool mute);
    bool SetEqBand(const std::wstring& ch, size_t band, double f, double g, double q);
    bool EnableEq(const std::wstring& ch, bool on);
    bool SetPersonalOutput(const std::wstring& endpointId);  // stops+restarts the personal render
    void OnDeviceSetChanged();               // from DeviceWatcher: re-match bindings, recover/degrade

    std::vector<ChannelState> GetChannelStates() const;
    DiagState GetDiag() const;
    bool PersonalOnFallback() const;         // true when rendering to default instead of bound device
};

// Pure, unit-tested: which endpoint should the personal mix render to?
// Prefer the bound device (id-first/name-fallback via MatchBinding); if absent,
// the system default render endpoint id; if even that is gone, empty (engine idles the render).
std::wstring PickPersonalOutput(const std::vector<EndpointInfo>& eps,
                                const DeviceRef& bound,
                                const std::wstring& systemDefaultId);

// engine/failover.h — native personal-output failover (spec "Native failover",
// semantics from MDropDX12's mixer_failover, user-directed 2026-09-23):
// opt-in, ordered allow list (first present wins, entries match id OR name),
// stability window (brief dropout cancels on return, nothing commits),
// minimum dwell after a commit (no cascade), fail over NEVER back (a commit
// rewrites the configured personalOutput; the old device returning does nothing).
// Pure state machine: clock and presence are inputs, so every sequence is
// testable without a device. Driven on the CONTROL thread (fj#401: never on
// audio threads, never inside a notification callback).
class FailoverDecider {
public:
    void Configure(bool armed, int stabilitySec, int dwellSec);
    enum class Action {
        None,           // nothing to do
        TempFallback,   // bound device just vanished: hop to default output (NOT a commit)
        ReturnToBound,  // bound device returned within the stability window: cancel, go back
        Commit,         // stability elapsed + allow candidate present + dwell ok: re-home for good
    };
    // boundPresent: is the CURRENTLY CONFIGURED personal output present?
    // firstAllowPresentId: id of the first present allow entry ("" = none/unarmed).
    // On Commit, *commitTargetId is that candidate; caller rewrites config.personalOutput,
    // restarts the render, and calls OnCommitApplied(nowMs) to start the dwell clock.
    Action Tick(uint64_t nowMs, bool boundPresent, const std::wstring& firstAllowPresentId,
                std::wstring* commitTargetId);
    void OnCommitApplied(uint64_t nowMs);
};
} // namespace mdxm
```

**Graph mechanics** (`engine.cpp`), per the spec diagram:
- Every channel's `CaptureStream` writes source-rate stereo float into its `resampler` and then its `ring` (resample-on-capture keeps the mix loop single-rate at 48 kHz).
- The **personal RenderStream is the clock master**: its pull callback runs the whole mix — for each healthy channel: `ring.Read(chanBuf, frames)` → `eq.Process` → copy to `pBuf`/`sBuf` → `personalGain.Process(pBuf)` / `streamingGain.Process(sBuf)` → accumulate into `personalSum` / `streamingSum`. Then mic monitoring is NOT in the personal mix (you don't hear your own mic); `SoftLimiter` on each sum; `personalSum` → the render buffer; `streamingSum` → `m_streamRing` (its own ~200 ms ring).
- The **streaming RenderStream** (into the streaming cable's render endpoint) pulls from `m_streamRing` with the same 100 ms prefill-cushion pattern as `--monitor`. Its clock drift against the personal device is absorbed by that ring and counted.
- The **mic chain** is independent of the mix loop: mic `CaptureStream` → mic ring → mic `RenderStream` (into the mic cable) whose pull applies mic gain ramp + mic EQ. Same cushion.
- **Fallback + failover** (Review Focus #5): when the personal RenderStream reports `Invalidated()` or `OnDeviceSetChanged` finds the bound device gone, `PickPersonalOutput` names the temporary target (default device), the personal render restarts there, and the fallback flag goes up — that is `FailoverDecider::Action::TempFallback`, not a commit. From then on the decider is Ticked (control thread: device-change events + a 1 s timer from the app layer): `ReturnToBound` within the stability window restarts on the bound device and clears the flag; `Commit` rewrites the configured personal output to the allow candidate (via a `std::function<void(const std::wstring&)> onFailoverCommit` callback the app layer installs — the engine owns no config) and clears the flag. Unarmed or no candidate present: the temporary fallback simply persists, exactly the spec's "never silent, never stuck". No auto-return outside the stability window — fail over, never back.
- Mix scratch buffers are member `std::vector<float>` sized once at Start (no allocation on the audio thread).
- Gain semantics: `SetVolume`/`SetMute` store vol and mute per (channel, mix) and set the ramp target to `mute ? 0.0f : vol` — so unmute restores the volume, and both changes ramp.

- [ ] **Step 1: Write the failing pure-logic tests** — `tests/test_engine_logic.cpp`:

```cpp
#include "test_framework.h"
#include "engine/engine.h"

using namespace mdxm;

MDXM_TEST_CASE(FallbackOutput_PicksBoundWhenPresent) {
    std::vector<EndpointInfo> eps = {
        {L"{hp}", L"Headphones", true, true}, {L"{spk}", L"Speakers", true, true} };
    CHECK(PickPersonalOutput(eps, {L"{hp}", L"Headphones"}, L"{spk}") == L"{hp}");
}

MDXM_TEST_CASE(FallbackOutput_PicksDefaultWhenBoundGone) {
    // Review Focus #5: headset disappears mid-stream -> default render device.
    std::vector<EndpointInfo> eps = { {L"{spk}", L"Speakers", true, true} };
    CHECK(PickPersonalOutput(eps, {L"{hp}", L"Headphones"}, L"{spk}") == L"{spk}");
}

MDXM_TEST_CASE(FallbackOutput_EmptyWhenNothingLeft) {
    std::vector<EndpointInfo> eps;
    CHECK(PickPersonalOutput(eps, {L"{hp}", L"Headphones"}, L"").empty());
}

MDXM_TEST_CASE(FallbackOutput_NameFallbackAfterRepair) {
    // Re-paired headset: new id, same name — must find it, not fall back.
    std::vector<EndpointInfo> eps = {
        {L"{hp-new}", L"WF-1000XM6", true, true}, {L"{spk}", L"Speakers", true, true} };
    CHECK(PickPersonalOutput(eps, {L"{hp-old}", L"WF-1000XM6"}, L"{spk}") == L"{hp-new}");
}

MDXM_TEST_CASE(Failover_BriefDropoutCancelsOnReturn) {
    FailoverDecider d;
    d.Configure(true, 5, 30);
    std::wstring target;
    CHECK(d.Tick(1000, true, L"{spk}", &target) == FailoverDecider::Action::None);
    CHECK(d.Tick(2000, false, L"{spk}", &target) == FailoverDecider::Action::TempFallback);
    CHECK(d.Tick(3000, false, L"{spk}", &target) == FailoverDecider::Action::None);   // still waiting
    CHECK(d.Tick(4000, true,  L"{spk}", &target) == FailoverDecider::Action::ReturnToBound);
    CHECK(d.Tick(60000, true, L"{spk}", &target) == FailoverDecider::Action::None);   // no commit ever
}

MDXM_TEST_CASE(Failover_SustainedLossCommitsToFirstAllow) {
    FailoverDecider d;
    d.Configure(true, 5, 30);
    std::wstring target;
    CHECK(d.Tick(1000, false, L"{spk}", &target) == FailoverDecider::Action::TempFallback);
    CHECK(d.Tick(5000, false, L"{spk}", &target) == FailoverDecider::Action::None);   // 4 s < 5 s window
    CHECK(d.Tick(6100, false, L"{spk}", &target) == FailoverDecider::Action::Commit);
    CHECK(target == L"{spk}");
    d.OnCommitApplied(6100);
    // Caller rewrote config; the NEW device is the bound one now. Old one returning: nothing.
    CHECK(d.Tick(7000, true, L"{spk}", &target) == FailoverDecider::Action::None);
}

MDXM_TEST_CASE(Failover_UnarmedNeverCommits) {
    FailoverDecider d;
    d.Configure(false, 5, 30);
    std::wstring target;
    CHECK(d.Tick(1000, false, L"", &target) == FailoverDecider::Action::TempFallback);
    CHECK(d.Tick(500000, false, L"", &target) == FailoverDecider::Action::None);      // stays on temp forever
}

MDXM_TEST_CASE(Failover_NoCandidatePresentStaysOnTemp) {
    FailoverDecider d;
    d.Configure(true, 5, 30);
    std::wstring target;
    d.Tick(1000, false, L"", &target);                                                // loss, no candidate
    CHECK(d.Tick(20000, false, L"", &target) == FailoverDecider::Action::None);
    CHECK(d.Tick(21000, false, L"{spk}", &target) == FailoverDecider::Action::Commit); // candidate appears later
}

MDXM_TEST_CASE(Failover_DwellBlocksCascade) {
    FailoverDecider d;
    d.Configure(true, 5, 30);
    std::wstring target;
    d.Tick(1000, false, L"{spk}", &target);
    CHECK(d.Tick(6100, false, L"{spk}", &target) == FailoverDecider::Action::Commit);
    d.OnCommitApplied(6100);
    // New device dies 1 s after the commit; stability passes at ~12 s but dwell holds until 36.1 s.
    CHECK(d.Tick(7100, false, L"{tv}", &target) == FailoverDecider::Action::TempFallback);
    CHECK(d.Tick(13000, false, L"{tv}", &target) == FailoverDecider::Action::None);   // dwell blocks
    CHECK(d.Tick(36200, false, L"{tv}", &target) == FailoverDecider::Action::Commit);
    CHECK(target == L"{tv}");
}
```

- [ ] **Step 2: Run tests to verify they fail** — compile failure.

- [ ] **Step 3: Implement `PickPersonalOutput`** (thin: `MatchBinding` filtered to render endpoints, else the default id if it still exists in `eps`, else empty), **`FailoverDecider`** (a two-state machine — Idle/Searching — plus `m_lossAtMs` and `m_dwellUntilMs` timestamps; Commit allowed only when `now - lossAt >= stability*1000` AND `now >= dwellUntil`; `OnCommitApplied` sets `m_dwellUntilMs = now + dwell*1000` and returns to Idle), and the full `Engine` per the mechanics above. Wrap every stream callback body in try/catch (no-crash rule). `GetChannelStates`/`GetDiag` snapshot under a mutex the control methods also take; the audio pull path reads only atomics/rings — it takes no engine-wide lock.

- [ ] **Step 4: Run tests to verify they pass** — all `FallbackOutput_*` ok.

- [ ] **Step 5: Add the graph integration test** to `tests/test_audio_integration.cpp` — `Audio_EngineToneGameToStreaming`: requires TWO cable pairs via env (`MDXM_TEST_RENDER/CAPTURE` = channel cable, `MDXM_TEST_RENDER2/CAPTURE2` = streaming cable). Build a one-channel `MixerConfig` in code (channel cable = pair 1, `streamingCable.render` = pair 2's render, `personalOutput` = pair 1's render — anything valid; personal volume 0 so nothing audible), `Engine::Start`, render a 440 Hz tone into the channel cable (same helper as Task 10's test), capture pair 2's capture endpoint for 2 s, assert RMS ≈ tone RMS ± 20 % and zero-crossing frequency 440 ± 10 Hz with streaming vol = 1.0; then `SetVolume(streaming, 0.0)` and assert the captured RMS collapses below 0.01 within 200 ms of audio. Skip cleanly when env vars are absent.

- [ ] **Step 6: Run it on the dev machine (cables required)** — `mdxmixer_test.exe --audio`. Expected: both `Audio_*` ok. On a machine without cables: clean skips.

- [ ] **Step 7: Commit**

```bash
git add src/mdxmixer/engine tests/test_engine_logic.cpp tests/test_audio_integration.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: mix graph - channels, dual mixes, mic chain, device fallback"
```

---

### Task 12: routing — per-app assignment and session listing

**Files:**
- Create: `src/mdxmixer/routing/audio_policy_config.h`, `src/mdxmixer/routing/audio_policy_config.cpp`, `src/mdxmixer/routing/sessions.h`, `src/mdxmixer/routing/sessions.cpp`
- Modify: `src/mdxmixer/main.cpp` (add `--sessions` diagnostic mode)

**Interfaces:**
- Consumes: `EnumerateEndpoints` from Task 9.
- Produces:

```cpp
namespace mdxm {
// The spec's one-file isolation for the undocumented IAudioPolicyConfigFactory.
// Tries the Win11 IID then the Win10 IID. Every method reports success/failure;
// failure means "assignment degrades and says so", never a crash (spec risk posture).
class AudioPolicyConfig {
public:
    bool Init(std::wstring* err);                        // resolves the factory, tries both IIDs
    bool IsAvailable() const;
    // exePath-based persisted default render endpoint for both eRender/eConsole+eMultimedia.
    bool SetAppDefaultRender(const std::wstring& exePath, const std::wstring& endpointId, std::wstring* err);
    bool ClearAppDefaultRender(const std::wstring& exePath, std::wstring* err);
    bool GetAppDefaultRender(const std::wstring& exePath, std::wstring* endpointId);
};

struct SessionInfo { std::wstring exePath; DWORD pid; std::wstring endpointId, endpointName; bool active; };
std::vector<SessionInfo> EnumerateSessions();            // IAudioSessionManager2 across all render endpoints
} // namespace mdxm
```

Implementation notes (this is the EarTrumpet-proven shape):
- Factory: `RoGetActivationFactory(HSTRING L"Windows.Media.Internal.AudioPolicyConfig", IID)` with the Win11 IID `{ab3d4648-e242-459f-b02f-541c70306324}` first, then the Win10 IID `{2a59116d-6c4f-45e0-a74f-707e3fef9258}`. Declare a minimal vtable-compatible interface (`IAudioPolicyConfigFactory`) with only the three methods used: `SetPersistedDefaultAudioEndpoint(pid/exePath…)`, `GetPersistedDefaultAudioEndpoint`, `ClearAllPersistedApplicationDefaultEndpoints` — copy the exact vtable layout EarTrumpet documents (padding slots as `virtual HRESULT Unused1()…`), and comment each slot with its real name.
- Device ids for this API use the MMDevice id wrapped in the `\\?\SWD\MMDEVAPI\` moniker + `{0}.` prefix form; write `MakePolicyDeviceId(endpointId)` + inverse `ParsePolicyDeviceId(policyId)` (render flow only in v1), both pure and covered by `CHECK`s in `tests/test_endpoints.cpp` (string transforms only).
- `EnumerateSessions`: for each active render endpoint: `Activate(IAudioSessionManager2)` → `GetSessionEnumerator` → per session `IAudioSessionControl2`: `GetProcessId`, `QueryFullProcessImageNameW` (skip system sessions / pid 0), state from `GetState`. COM failures skip that session.

- [ ] **Step 1: Write the failing tests** — add to `tests/test_endpoints.cpp`:

```cpp
#include "routing/audio_policy_config.h"

MDXM_TEST_CASE(PolicyId_RoundTrips) {
    std::wstring mm = L"{0.0.0.00000000}.{aaaa-bbbb}";
    std::wstring pol = mdxm::MakePolicyDeviceId(mm);
    CHECK(pol.find(L"MMDEVAPI") != std::wstring::npos);
    CHECK(mdxm::ParsePolicyDeviceId(pol) == mm);
    CHECK(mdxm::ParsePolicyDeviceId(L"garbage") == L"garbage");  // unrecognized passes through
}
```

- [ ] **Step 2: Run to verify failure**, implement the two pure functions + the factory wrapper + sessions sweep as above, re-run to green.

- [ ] **Step 3: Add `--sessions` mode to `main.cpp`**: print `EnumerateSessions()` as a table (exe, pid, endpoint name), then if `AudioPolicyConfig::Init` succeeds print `policy API: available (win11|win10 iid)`, else the error. This is the smoke test for the undocumented API on the running Windows build.

- [ ] **Step 4: Run `mdxmixer.exe --sessions` on the dev machine.** Expected: real sessions listed; policy API available. If the API is NOT available on this build, stop and surface that to your human partner — the spec accepts degradation at runtime but building v1 against a machine where it's already broken is a different conversation.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/routing src/mdxmixer/main.cpp tests/test_endpoints.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: per-app routing via audio policy config + session enumeration"
```

---

### Task 13: ipc/pipe_server — the named pipe

**Files:**
- Create: `src/mdxmixer/ipc/pipe_server.h`, `src/mdxmixer/ipc/pipe_server.cpp`
- Test: `tests/test_pipe.cpp`

**Interfaces:**
- Consumes: `HandleProtocolMessage` (Task 8).
- Produces:

```cpp
namespace mdxm {
// \\.\pipe\mdxmixer — fixed name (single instance), duplex, PIPE_TYPE_MESSAGE |
// PIPE_READMODE_MESSAGE, UTF-16LE null-terminated messages, multi-client.
// A simplified MDropDX12 pipe_server: accept thread + one thread per client with an
// outgoing queue and event; no SIGNAL dispatch, no strict mode, no renames.
class PipeServer {
public:
    // onMessage runs on the client thread; its returned strings are sent back in order.
    // wantSubscribe out-param from the protocol layer flips that client's subscription flag.
    using Handler = std::function<std::vector<std::wstring>(const std::wstring& msg, bool* wantSubscribe)>;
    bool Start(Handler handler, std::wstring* err);
    void Stop();
    void Broadcast(const std::wstring& msg);   // to subscribed clients only (MDXM_SUBSCRIBE=1)
    int  ClientCount() const;
    static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\mdxmixer";
};
} // namespace mdxm
```

- [ ] **Step 1: Write the failing test** — `tests/test_pipe.cpp`, a real loopback over the actual pipe (this is a headless Windows test, no audio hardware — it runs in the default suite):

```cpp
#include "test_framework.h"
#include "ipc/pipe_server.h"
#include <windows.h>

using namespace mdxm;

namespace {
std::wstring PipeRequest(HANDLE h, const std::wstring& msg) {
    DWORD wr = 0;
    WriteFile(h, msg.c_str(), (DWORD)((msg.size() + 1) * sizeof(wchar_t)), &wr, nullptr);
    wchar_t buf[4096];
    DWORD rd = 0;
    if (!ReadFile(h, buf, sizeof buf, &rd, nullptr)) return L"";
    return std::wstring(buf, rd / sizeof(wchar_t) - 1);   // strip trailing null
}
} // namespace

MDXM_TEST_CASE(Pipe_EchoAndBroadcastRoundTrip) {
    PipeServer srv;
    std::wstring err;
    bool started = srv.Start([](const std::wstring& msg, bool* wantSub) {
        if (msg == L"MDXM_SUBSCRIBE=1") { *wantSub = true; return std::vector<std::wstring>{L"MDXM_OK"}; }
        return std::vector<std::wstring>{L"ECHO|" + msg};
    }, &err);
    CHECK(started);

    HANDLE h = CreateFileW(L"\\\\.\\pipe\\mdxmixer", GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    CHECK(PipeRequest(h, L"MDXM_PING") == L"ECHO|MDXM_PING");
    CHECK(PipeRequest(h, L"MDXM_SUBSCRIBE=1") == L"MDXM_OK");

    srv.Broadcast(L"MDXM_CHAN|id=game|pvol=0.5");
    wchar_t buf[4096]; DWORD rd = 0;
    CHECK(ReadFile(h, buf, sizeof buf, &rd, nullptr));      // push arrives without a request
    CHECK(std::wstring(buf, rd / sizeof(wchar_t) - 1) == L"MDXM_CHAN|id=game|pvol=0.5");

    CloseHandle(h);
    Sleep(100);
    srv.Stop();
    CHECK(srv.ClientCount() == 0);
}
```

- [ ] **Step 2: Run to verify it fails** — compile failure.

- [ ] **Step 3: Implement `pipe_server.cpp`** following MDropDX12's `pipe_server.cpp` structure, minus what mdxmixer doesn't need: an accept thread creating instances with `CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 64*1024, 64*1024, 0, nullptr)`; overlapped `ConnectNamedPipe` waiting on {connect event, shutdown event}; per client a `PipeClientContext { HANDLE hPipe; HANDLE hThread; HANDLE hOutEvent; std::queue<std::wstring> outQueue; std::mutex outMutex; bool subscribed; }` and a client thread doing overlapped reads waiting on {read event, out event, shutdown event}: reads dispatch to the handler and queue replies; the out event drains `outQueue` via `WriteFile` (each message with its trailing null). `Broadcast` enqueues to subscribed clients and sets their events. `Stop`: signal shutdown, close instances, join threads, sweep contexts. Every thread body try/catch (no-crash rule).

- [ ] **Step 4: Run to green** — `Pipe_EchoAndBroadcastRoundTrip` ok.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/ipc tests/test_pipe.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: named-pipe IPC server with subscription broadcast"
```

---

### Task 14: ui — tray icon and the four-tab window

**Files:**
- Create: `src/mdxmixer/ui/tray_icon.h/.cpp`, `src/mdxmixer/ui/main_window.h/.cpp`, `src/mdxmixer/ui/tab_mixer.h/.cpp`, `src/mdxmixer/ui/tab_routing.h/.cpp`, `src/mdxmixer/ui/tab_eq.h/.cpp`, `src/mdxmixer/ui/tab_devices.h/.cpp`, `src/mdxmixer/resource.h`, `src/mdxmixer/mdxmixer.rc` (icon + version block)

**Interfaces:**
- Consumes: `IMixerControl` (Task 8), `EnumerateSessions` (Task 12), `EnumerateEndpoints` (Task 9), config structs.
- Produces: `class TrayIcon { bool Add(HWND owner, HICON icon, const wchar_t* tip); void Remove(); /* WM_APP+1 callback message */ };` and `class MainWindow { bool Create(HINSTANCE, IMixerControl& ctl, ConfigStore& store); void Show(); void Hide(); HWND Hwnd() const; void RefreshFromState(); };` — plus the app-level contract that closing the window hides to tray, and Exit lives only in the tray menu with the routed-apps-go-silent warning (spec).

No unit tests — UI is verified by scripted manual checks. Keep ALL mixer logic out of the UI: every user action is one `IMixerControl` call; every displayed value comes from `GetChannels()` / snapshots. That seam is what the protocol tests already cover.

- [ ] **Step 1: Tray icon + empty window shell.** `Shell_NotifyIconW(NIM_ADD, ...)` with `NIF_MESSAGE|NIF_ICON|NIF_TIP`, callback `WM_APP+1`; right-click menu: Open Mixer / Show in taskbar (checkable, = `config.ui.taskbarButton`) / Autostart (checkable) / Exit; double-click opens the window. Window: `CreateWindowExW` with a `WC_TABCONTROLW` filling the client area, four tabs, each tab a child dialog-less window shown/hidden on `TCN_SELCHANGE`. **Taskbar option** (spec Program shape): taskbarButton off → window created with `WS_EX_TOOLWINDOW` while hidden and `WM_CLOSE` hides to tray; on → normal `WS_EX_APPWINDOW` taskbar button and `WM_CLOSE` minimizes instead of hiding (the tray icon stays either way — Exit only lives there). Toggling the menu item flips the style live (`SetWindowLongPtrW(GWL_EXSTYLE, …)` + hide/show to re-register with the shell) and persists via config. Exit shows `MessageBoxW` warning "Apps routed to cables go silent until mdxmixer runs again. Exit anyway?" (spec's stated consequence).

- [ ] **Step 2: Mixer tab.** For each channel (from `GetChannels()`): a static label (name + health dot: "●" green/red via `SetTextColor` in `WM_CTLCOLORSTATIC`), two horizontal `TRACKBAR_CLASSW` sliders (range 0–100) labeled Personal / Streaming, two `BS_AUTOCHECKBOX` mutes. `WM_HSCROLL` → `ctl.SetVolume(id, mix, pos/100.0f)`; checkbox → `SetMute`. A 250 ms `SetTimer` calls `RefreshFromState()` — but only rewrites a control when the value differs AND the user isn't dragging it (`GetCapture() != slider`), so IPC/hotkey changes appear without fighting the mouse.

- [ ] **Step 3: EQ tab.** Channel `CBS_DROPDOWNLIST` combobox; an Enable checkbox (`EnableEq`); a 10-row grid of three `ES_NUMBER`-ish edit controls (freq/gain/Q — plain edits, validated on `EN_KILLFOCUS` with `wcstod`, invalid text reverts to current value) each row applying via `SetEqBand` on change. Values load from the selected channel's config snapshot.

- [ ] **Step 4: Routing tab.** A `WC_LISTVIEWW` (report style) of `EnumerateSessions()`: exe name, pid, current endpoint name, assigned channel (from config `apps` lists). Below: a channel combobox (+ "— unassigned —") and an Assign button → `ctl.AssignApp(exePath, chOrDash)`. A static footer states the spec's known limitation verbatim: "Apps that select a specific output device themselves ignore per-app routing." Refresh button re-enumerates.

- [ ] **Step 5: Devices tab.** Comboboxes (render endpoints from `EnumerateEndpoints()`): Personal output, Streaming cable, Mic input (capture endpoints), Mic cable; per-channel cable pickers (render+capture pair per channel). Selecting fires the corresponding control/config mutation (`SetPersonalRoute` for personal; cable changes mutate config and require engine restart — an Apply button calls a `RestartEngine` callback the app controller provides). A Mic gain trackbar (0–100 → mic gain 0..1, applied through the mic chain's `GainRamp`; mic EQ shares the EQ tab — the mic appears in that tab's channel combobox as "Mic"). A **Failover group**: Armed checkbox, an ordered listbox of allow entries (name shown, id kept), Add (picks from a render-endpoint combobox), Remove, Up, Down — edits mutate `config.personalFailover` (order is preference: first present wins). Shows the personal-fallback banner when `PersonalOnFallback()` ("Bound output missing — using default device").

- [ ] **Step 6: Manual verification (scripted).** Build Debug, run:
  1. Tray icon appears; double-click opens window; close hides it; reopen works.
  2. Mixer tab shows configured channels; dragging Personal slider changes what you hear on a playing channel; Mute ramps to silence without a click.
  3. EQ tab: +6 dB at 1 kHz on a music channel is audible; Enable off returns to flat.
  4. Routing tab lists real sessions; assigning a test app moves its audio to the channel cable (audible through the personal mix).
  5. Devices tab: changing personal output moves playback; unplugging the bound output shows the fallback banner and audio continues on the default device.
  6. Exit shows the warning.
  7. Failover: arm it with Speakers in the allow list, disconnect the bound headset — audio hops to default at once, then commits to Speakers after the stability window (Devices tab shows Speakers as the new personal output); reconnect the headset — nothing moves.
  8. Taskbar toggle: on → window gets a taskbar button and close minimizes; off → button gone, close hides to tray; tray icon present in both.
  Record the outcome of each numbered check in the task's commit message body.

- [ ] **Step 7: Commit**

```bash
git add src/mdxmixer/ui src/mdxmixer/resource.h src/mdxmixer/mdxmixer.rc src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: tray icon and Mixer/Routing/EQ/Devices window"
```

---

### Task 15: app — wiring it all together

**Files:**
- Create: `src/mdxmixer/app/app_controller.h`, `src/mdxmixer/app/app_controller.cpp`, `src/mdxmixer/app/log.h`, `src/mdxmixer/app/log.cpp`, `src/mdxmixer/app/autostart.h`, `src/mdxmixer/app/autostart.cpp`
- Modify: `src/mdxmixer/main.cpp`

**Interfaces:**
- Consumes: everything.
- Produces: `class AppController : public IMixerControl` — owns `ConfigStore`, `Engine`, `PipeServer`, `DeviceWatcher`, `AudioPolicyConfig`; implements every `IMixerControl` method as: clamp → engine call → config mutate → `BroadcastState()` (build `MDXM_CHAN`/`MDXM_ROUTE` records and `PipeServer::Broadcast`). Also `void Log(int level, const wchar_t* fmt, ...)` (level-gated, `log/mdxmixer.log` beside the exe, 5 MB rotate to `.old`); `bool SetAutostart(bool on)` / `bool GetAutostart()` (HKCU `Software\Microsoft\Windows\CurrentVersion\Run`, value `mdxmixer`, quoted exe path; written ONLY from the explicit toggle — spec).

- [ ] **Step 1: Implement `log` and `autostart`** (small, self-contained; autostart via `RegSetValueExW`/`RegDeleteValueW`/`RegQueryValueExW`).

- [ ] **Step 2: Implement `AppController`.** Start order: load config (log if defaults were used) → `AudioPolicyConfig::Init` (log degrade) → `Engine::Start` → `PipeServer::Start` with a handler that calls `HandleProtocolMessage(msg, *this, &sub)` → `DeviceWatcher::Start([this]{ PostMessage(mainWnd, WM_APP+2, 0, 0); })` (debounce: engine's `OnDeviceSetChanged` runs on the UI thread, 500 ms coalesced via `SetTimer` — the callback itself is signal-only per fj#401: no MMDevice calls, no blocking inside it). Failover wiring: a 1 s `SetTimer` on the main window Ticks the engine's `FailoverDecider` (presence via `MatchBinding` over a fresh `EnumerateEndpoints()` — UI thread, never the audio threads); the engine's `onFailoverCommit` callback is installed to rewrite `config.personalOutput` to the committed device, flush config, and `BroadcastState()`. `AssignApp` calls `AudioPolicyConfig::SetAppDefaultRender(exe, channel's cable render endpoint)` — or `Clear` for `-` — then updates config `apps` lists. Stop order (reverse): watcher, pipe, engine, `ConfigStore::FlushNow`.

- [ ] **Step 3: Finish `main.cpp`.** Single-instance: `CreateMutexW(nullptr, TRUE, L"Local\\mdxmixer_single")`; on `ERROR_ALREADY_EXISTS`, connect to the pipe, send `MDXM_SHOW` (add the verb to the protocol: replies `MDXM_OK` and the app shows its window), exit 0. First run (config used defaults): show the window and the Devices tab immediately, and present the autostart question as a checkbox in that window's footer, unticked (spec: presented at first run, written only when ticked). CLI modes stay: `--monitor`, `--sessions`, plus `--devices` (prints `EnumerateEndpoints()` ids/names — Task 16's tone-test setup uses it).

- [ ] **Step 4: Full manual smoke.** `build.ps1 Release x64`. Run through Task 14's checklist once more on the Release build, plus: kill the process from Task Manager and relaunch — config intact (atomic saves), channels come back healthy. Verify `MDXM_STATE` over the pipe from PowerShell:

```powershell
$p = new-object IO.Pipes.NamedPipeClientStream '.','mdxmixer',InOut
$p.Connect(2000); $p.ReadMode='Message'
$w = [Text.Encoding]::Unicode.GetBytes("MDXM_STATE`0"); $p.Write($w,0,$w.Length)
$buf = new-object byte[] 65536
do { $n = $p.Read($buf,0,$buf.Length); [Text.Encoding]::Unicode.GetString($buf,0,$n) } until ($n -eq 0 -or ([Text.Encoding]::Unicode.GetString($buf,0,$n)) -match 'MDXM_END')
```

Expected: `MDXM_BEGIN` … channel records … `MDXM_END`.

- [ ] **Step 5: Commit**

```bash
git add src/mdxmixer/app src/mdxmixer/main.cpp src/mdxmixer/mdxmixer.vcxproj
git commit -m "feat: app controller wiring - config, engine, pipe, tray, autostart"
```

---

### Task 16: soak, docs, and rollout prep

**Files:**
- Create: `README.md`, `docs/rollout.md`
- Modify: `tests/test_audio_integration.cpp` (soak), `docs/specs/2026-09-22-mdxmixer-design.md` (status line only)

- [ ] **Step 1: Add the soak test** — `Audio_DriftCountersStayNearZero`: with the cable env vars set, run the Task 11 engine config for 3 minutes of tone, then assert every ring's `Drops() + Underruns()` counts fewer than 20 events total (generous rings absorb clock drift; a runaway counter means the cushion or ring sizing is wrong — lessons note). Gate the 3-minute duration behind `MDXM_TEST_SOAK=1` so `--audio` stays fast by default; without it the test runs 20 s with a proportional threshold.

- [ ] **Step 2: Write `README.md`** — what it is (three sentences), build commands, test commands (`--audio`, env vars, `--devices` helper), CLI modes, pipe protocol pointer to the spec, VB-CABLE prerequisite.

- [ ] **Step 3: Write `docs/rollout.md`** — the spec's ordered rollout steps 1–6 verbatim as a checklist for the target machine, plus the exact cable → channel table the operator fills in and the 48 kHz default-format instruction (mmsys.cpl → cable properties → Advanced).

- [ ] **Step 4: Update the spec's status line** to `Status: approved design, implemented (v1).` — nothing else in the spec changes.

- [ ] **Step 5: Run the full suite one last time** — `mdxmixer_test.exe` (headless: expect every non-Audio test ok) and `mdxmixer_test.exe --audio` with `MDXM_TEST_SOAK=1` on the dev machine. Both green before the final commit.

- [ ] **Step 6: Commit and push**

```bash
git add README.md docs/rollout.md docs/specs/2026-09-22-mdxmixer-design.md tests/test_audio_integration.cpp
git commit -m "docs: README, rollout runbook; soak test; mark spec implemented"
git push origin main
```

---

## Out of Scope (deliberately)

- The MDropDX12 `mixer_provider_mdxmixer` — lands as a branch in the MDropDX12 repo after this plan ships a working pipe (spec: "separate deliverable, other repo"). File a Forgejo issue there when Task 13 merges.
- Kernel driver, noise suppression, ASIO/exclusive mode, watchdog, Sonar config import — spec non-goals.
- Actual VB-CABLE installation and app migration on the target machine — `docs/rollout.md` is the runbook; a human executes it.
