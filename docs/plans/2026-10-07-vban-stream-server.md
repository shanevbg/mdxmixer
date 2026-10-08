# VBAN Stream Server (Phases 1–3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** mdxmixer becomes a spec-compliant VBAN device: AUDIO out (the personal monitor mix, PC-side gain), SERVICE/PING0 discovery+subscription, TXT-carried MDXM IPC with PIN/device auth, and FRAME screen captures — phases 1–3 of the design spec (the Android receiver is a separate plan in MDR_Android).

**Architecture:** A pure, Windows-free wire layer (`net/vban_protocol`, `net/vban_peers`, `net/vban_pacer`) under a winsock server (`net/vban_server`) with one receive thread and one paced sender thread; the engine writes the selected post-limiter sum into a dedicated SPSC ring from `MixPull`; control records marshal onto the UI thread through the same hardened `IpcRequest` path the pipe uses, extracted into a shared function.

**Tech Stack:** C++17 / Win32, winsock2 (`ws2_32`), DXGI Desktop Duplication + D3D11 + WIC (phase 3), homegrown test framework, no external libraries.

**Spec:** `docs/specs/2026-10-07-vban-stream-server-design.md` — read it before any task; task text cites its sections as "§N".

## Global Constraints

- x64 only, C++17, wide strings, `/utf-8`, Warning Level 4 — match the vcxproj settings; no new build flags.
- **No external libraries.** Clean-room from the VBAN spec; never copy from GPL implementations (quiniouben/vban, obs-vban, pyVBAN). Link libraries via `#pragma comment(lib, …)` at point of use, never vcxproj `AdditionalDependencies`.
- **Every new `.cpp` — source and test — must be added to the single `<ItemGroup>` of `ClCompile` entries in `src/mdxmixer/mdxmixer.vcxproj`** (sources ~lines 133–180, tests ~181–218). There is no glob; a missing entry silently never compiles.
- No-crash rule: every thread entry wrapped in `try { … } catch (const std::exception&) { … } catch (...) { … }`; the audio callback path takes no locks, allocates nothing, calls no clock.
- `net/vban_protocol.{h,cpp}`, `net/vban_peers.{h,cpp}`, `net/vban_pacer.h` are **pure**: no `<windows.h>`, no sockets — headless-testable like `dsp/`.
- New inbound verbs must appear in `docs/ipc.md` or `tests/test_docs.cpp` fails the suite (it scans `protocol.cpp` for `r.verb == L"…"`).
- Conventional commits (`feat:` / `fix:` / `docs:` / `test:` / `chore:`); never invent a `#N` issue reference.
- Build: `powershell -ExecutionPolicy Bypass -File build.ps1 Test` then run `.\bin\Test\mdxmixer_test.exe` (add `--audio` only for `Audio_`-prefixed tests, which need real devices).
- Version: this work is **v1.2.0**; `src/mdxmixer/version.h`'s three numeric macros are the only place the number changes (no string literals — see the file's warning).
- Constants fixed by the spec: UDP port 6980 default; 28-byte header; 1436-byte max payload; 48 kHz = SR index 3; peer cap 8; ping keepalive ~2 s / expiry 10 s; strikes 3 / lockout 60 s; gain 0–6400 percent default 100; int16 = 256 frames/packet, float32 = 128.

## Review Focus

Failure modes the spec implies but needs pinning in tests — each line's test is added to the owning task below:

1. **Garbage UDP must be inert** — short packets (<28 B), wrong magic, truncated payloads, oversize claims: parse rejects, nothing dereferences past `len`. (Task 1, `Vban_ParseRejectsGarbage`.)
2. **Gain extremes must not corrupt the wire** — gain 0 and 6400 % through float→int16 at full-scale input: limiter then conversion clamps, no integer wrap. (Task 2, `Vban_AudioPacketGainExtremes`.)
3. **A LAN scanner must not evict the phone** — 9 pingers against the 8-slot table: authenticated entries never evicted for unauthenticated pingers; expired evicted first. (Task 6, `VbanPeers_TableFullEviction`.)
4. **A second instance must fail loudly** — bind failure (port taken) surfaces in `MDXM_VBANSTATE` as an error, never a silent swallow. (Task 7, `Vban_BindFailureIsReported`.)
5. **Inbound TXT larger than one packet / invalid UTF-8 must not mis-dispatch** — oversized or malformed records are dropped and counted, never partially parsed into a verb. (Task 10, `VbanTxt_RejectsOversizeAndBadUtf8`.)

## File Structure

| Path | Role |
| --- | --- |
| `src/mdxmixer/net/vban_protocol.h/.cpp` | pure wire layer: header, SR table, parse, AUDIO/PING0/TXT/FRAME builders, UTF-8 codec |
| `src/mdxmixer/net/vban_pacer.h` | pure send-schedule (header-only) |
| `src/mdxmixer/net/vban_peers.h/.cpp` | pure peer/auth/entitlement state machine |
| `src/mdxmixer/net/vban_server.h/.cpp` | winsock socket + receive/sender threads + marshalling glue |
| `src/mdxmixer/net/display_capture.h/.cpp` | DXGI duplication → mip downscale → WIC JPEG (phase 3) |
| `src/mdxmixer/ui/tab_vban.cpp` | the sixth tab (phase 2) |
| `tests/test_vban_protocol.cpp`, `tests/test_vban_peers.cpp`, `tests/test_vban_server.cpp`, `tests/test_vban_config.cpp` | headless tests |
| Modified | `config/config.{h,cpp}`, `engine/{engine.h,engine.cpp,mix_demand.h}`, `ipc/{mixer_control.h,protocol.cpp}`, `app/app_controller.{h,cpp}`, `ui/main_window.{h,cpp}`, `main.cpp`, `mdxmixer.vcxproj`, `docs/ipc.md`, `docs/Changes.md`, `version.h`, `tests/{test_mix_demand.cpp,test_docs.cpp}` |

---

# Phase 1 — AUDIO + SERVICE (listening works via Receptor Lite)

### Task 1: Wire-protocol core — header, SR table, parse

**Files:**
- Create: `src/mdxmixer/net/vban_protocol.h`, `src/mdxmixer/net/vban_protocol.cpp`
- Create: `tests/test_vban_protocol.cpp`
- Modify: `src/mdxmixer/mdxmixer.vcxproj` (two `ClCompile` entries), `src/mdxmixer/version.h` (minor → 2), `docs/Changes.md` (new `## v1.2.0 (unreleased)` section at top: "feat: VBAN stream server — spec docs/specs/2026-10-07-vban-stream-server-design.md")

**Interfaces:**
- Produces (everything later tasks use from this file):
  - `namespace mdxm::vban`, `struct Header` (28 B packed), `kMagic`, `kHeaderSize=28`, `kMaxData=1436`, `kMaxPacket=1464`, `kDefaultPort=6980`, `kStreamNameSize=16`
  - sub-protocol constants `kProtoAudio=0x00, kProtoSerial=0x20, kProtoTxt=0x40, kProtoService=0x60, kProtoFrame=0x80, kProtoMask=0xE0`
  - `int SampleRateIndex(uint32_t rate)` / `uint32_t SampleRateFromIndex(int)`
  - `struct Parsed { bool valid; uint8_t proto; Header hdr; const uint8_t* data; size_t dataLen; };`
  - `Parsed ParsePacket(const uint8_t* buf, size_t len)`
  - `void FillStreamName(char out[16], const std::string& name)`
  - `bool StreamNameIs(const Header& h, const char name[16])`

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/test_vban_protocol.cpp
#include "test_framework.h"
#include "net/vban_protocol.h"
#include <cstring>
using namespace mdxm::vban;

MDXM_TEST_CASE(Vban_HeaderIs28BytesPacked) {
    CHECK(sizeof(Header) == 28);
    CHECK(kHeaderSize == 28);
    CHECK(kMaxData == 1436);
    CHECK(kMaxPacket == 1464);
}

MDXM_TEST_CASE(Vban_SampleRateTable) {
    // Three geometric families, NOT ascending (spec): 48000 is index 3.
    CHECK(SampleRateIndex(48000) == 3);
    CHECK(SampleRateIndex(44100) == 16);
    CHECK(SampleRateIndex(96000) == 4);
    CHECK(SampleRateIndex(192000) == 5);
    CHECK(SampleRateIndex(12345) == -1);
    CHECK(SampleRateFromIndex(3) == 48000);
    CHECK(SampleRateFromIndex(16) == 44100);
    CHECK(SampleRateFromIndex(21) == 0);   // 21..31 undefined
    CHECK(SampleRateFromIndex(-1) == 0);
    // Round-trip every defined index.
    for (int i = 0; i < 21; ++i) CHECK(SampleRateIndex(SampleRateFromIndex(i)) == i);
}

MDXM_TEST_CASE(Vban_ParseRoundTrip) {
    uint8_t buf[64] = {};
    Header h = {};
    h.vban = kMagic;
    h.format_SR = (uint8_t)(kProtoAudio | SampleRateIndex(48000));
    h.format_nbs = 255;             // 256 samples, stored minus one
    h.format_nbc = 1;               // stereo
    h.format_bit = 0x01;            // int16, PCM
    FillStreamName(h.streamname, "mdxmixer");
    h.nuFrame = 42;
    memcpy(buf, &h, sizeof h);
    buf[sizeof h] = 0xAB;           // 1 data byte
    Parsed p = ParsePacket(buf, sizeof h + 1);
    CHECK(p.valid);
    CHECK(p.proto == kProtoAudio);
    CHECK(p.hdr.nuFrame == 42);
    CHECK(p.dataLen == 1);
    CHECK(p.data[0] == 0xAB);
    CHECK(StreamNameIs(p.hdr, h.streamname));
}

MDXM_TEST_CASE(Vban_ParseRejectsGarbage) {
    uint8_t buf[2000] = {};
    CHECK(!ParsePacket(buf, 0).valid);
    CHECK(!ParsePacket(buf, 27).valid);          // one byte short of a header
    CHECK(!ParsePacket(buf, 28).valid);          // zero magic
    Header h = {}; h.vban = 0x12345678;          // wrong magic
    memcpy(buf, &h, sizeof h);
    CHECK(!ParsePacket(buf, 100).valid);
    h.vban = kMagic; memcpy(buf, &h, sizeof h);
    CHECK(ParsePacket(buf, 28).valid);           // header-only is a legal packet
    CHECK(ParsePacket(buf, 28).dataLen == 0);
    CHECK(!ParsePacket(buf, 2000).valid);        // over kMaxPacket: refuse, don't trust
    CHECK(!ParsePacket(nullptr, 100).valid);
}

MDXM_TEST_CASE(Vban_StreamNameFillAndCompare) {
    char a[16], b[16];
    FillStreamName(a, "mdxmixer");
    FillStreamName(b, "mdxmixer");
    CHECK(memcmp(a, b, 16) == 0);
    CHECK(a[8] == 0 && a[15] == 0);              // NUL-padded to 16
    FillStreamName(b, "a-very-long-name-that-overflows");
    CHECK(b[15] != 0 || b[0] == 'a');            // truncated at 16, never past
    Header h = {}; memcpy(h.streamname, a, 16);
    CHECK(StreamNameIs(h, a));
    CHECK(!StreamNameIs(h, b));
}
```

- [ ] **Step 2: Add the vcxproj entries and run to verify failure**

Add to the `ClCompile` ItemGroup in `src/mdxmixer/mdxmixer.vcxproj`, beside the existing entries:

```xml
<ClCompile Include="net\vban_protocol.cpp" />
<ClCompile Include="..\..\tests\test_vban_protocol.cpp" />
```

Run: `powershell -ExecutionPolicy Bypass -File build.ps1 Test`
Expected: FAIL to compile — `net/vban_protocol.h` does not exist.

- [ ] **Step 3: Write the implementation**

```cpp
// src/mdxmixer/net/vban_protocol.h
#pragma once
// VBAN wire protocol (rev 13, SEP 2025), clean-room from the published spec
// (docs/specs/2026-10-07-vban-stream-server-design.md §2). PURE: no Windows
// headers, no sockets -- headless-testable like dsp/. net/vban_server.cpp is
// the only place these bytes meet a socket.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace mdxm { namespace vban {

constexpr uint16_t kDefaultPort = 6980;
constexpr size_t   kHeaderSize = 28;
constexpr size_t   kMaxData = 1436;                 // spec: 1464 total - 28 header
constexpr size_t   kMaxPacket = kHeaderSize + kMaxData;
constexpr size_t   kStreamNameSize = 16;
constexpr uint32_t kMagic = 0x4E414256;             // "VBAN" read as LE uint32

// Sub-protocol, bits 5-7 of format_SR.
constexpr uint8_t kProtoAudio = 0x00, kProtoSerial = 0x20, kProtoTxt = 0x40,
                  kProtoService = 0x60, kProtoFrame = 0x80, kProtoMask = 0xE0;

#pragma pack(push, 1)
struct Header {
    uint32_t vban;
    uint8_t  format_SR;    // bits 5-7 sub-protocol; bits 0-4 SR index (audio),
                           // informational rate (txt/frame), MUST be 0 (service)
    uint8_t  format_nbs;   // audio: samples-1; service: function (bit7=reply); frame: index LSB
    uint8_t  format_nbc;   // audio: channels-1; service: service type; frame: index MSB
    uint8_t  format_bit;   // audio: bits0-2 type + bits4-7 codec; txt: bits4-7 charset;
                           // frame: bits0-2 packet type + bits4-7 frame type
    char     streamname[kStreamNameSize];   // ASCII, NUL-padded
    uint32_t nuFrame;
};
#pragma pack(pop)
static_assert(sizeof(Header) == kHeaderSize, "VBAN header must pack to 28 bytes");

int      SampleRateIndex(uint32_t rate);     // -1 when not in the table
uint32_t SampleRateFromIndex(int index);     // 0 when undefined/out of range

struct Parsed {
    bool valid = false;
    uint8_t proto = 0;
    Header hdr = {};
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
};
// Validates length (28..kMaxPacket) and magic; copies the header out so the
// caller may reuse its receive buffer; `data` still aliases buf+28.
Parsed ParsePacket(const uint8_t* buf, size_t len);

void FillStreamName(char out[kStreamNameSize], const std::string& name);
bool StreamNameIs(const Header& h, const char name[kStreamNameSize]);

}} // namespace mdxm::vban
```

```cpp
// src/mdxmixer/net/vban_protocol.cpp
#include "net/vban_protocol.h"
#include <cstring>

namespace mdxm { namespace vban {

namespace {
// Spec table: three geometric families, in spec order. Index == position.
constexpr uint32_t kRates[21] = {
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    11025, 22050, 44100, 88200, 176400, 352800, 705600 };
} // namespace

int SampleRateIndex(uint32_t rate) {
    for (int i = 0; i < 21; ++i)
        if (kRates[i] == rate) return i;
    return -1;
}

uint32_t SampleRateFromIndex(int index) {
    if (index < 0 || index >= 21) return 0;
    return kRates[index];
}

Parsed ParsePacket(const uint8_t* buf, size_t len) {
    Parsed p;
    if (!buf || len < kHeaderSize || len > kMaxPacket) return p;
    std::memcpy(&p.hdr, buf, kHeaderSize);
    if (p.hdr.vban != kMagic) return p;
    p.proto = (uint8_t)(p.hdr.format_SR & kProtoMask);
    p.data = buf + kHeaderSize;
    p.dataLen = len - kHeaderSize;
    p.valid = true;
    return p;
}

void FillStreamName(char out[kStreamNameSize], const std::string& name) {
    std::memset(out, 0, kStreamNameSize);
    std::memcpy(out, name.data(),
                name.size() < kStreamNameSize ? name.size() : kStreamNameSize);
}

bool StreamNameIs(const Header& h, const char name[kStreamNameSize]) {
    return std::memcmp(h.streamname, name, kStreamNameSize) == 0;
}

}} // namespace mdxm::vban
```

- [ ] **Step 4: Build Test config and run; all `Vban_*` tests pass**

Run: `build.ps1 Test` then `.\bin\Test\mdxmixer_test.exe`
Expected: `ok` for the five new tests, zero failures overall.

- [ ] **Step 5: Bump version + changelog, commit**

`version.h`: `#define MDXM_VERSION_MINOR 2` (only that line). `docs/Changes.md`: add the `## v1.2.0 (unreleased)` section above v1.1.0.

```bash
git add src/mdxmixer/net/vban_protocol.h src/mdxmixer/net/vban_protocol.cpp \
        tests/test_vban_protocol.cpp src/mdxmixer/mdxmixer.vcxproj \
        src/mdxmixer/version.h docs/Changes.md
git commit -m "feat: VBAN wire-protocol core -- header, SR table, packet parse"
```

---

### Task 2: AUDIO packet builder + send pacer

**Files:**
- Modify: `src/mdxmixer/net/vban_protocol.h/.cpp`
- Create: `src/mdxmixer/net/vban_pacer.h`
- Modify: `tests/test_vban_protocol.cpp`

**Interfaces:**
- Produces:
  - `constexpr uint8_t kBitInt16 = 0x01, kBitFloat32 = 0x04;` (format_bit data types, codec PCM = high nibble 0)
  - `constexpr size_t kFramesPerPacketI16 = 256, kFramesPerPacketF32 = 128;`
  - `size_t AudioFramesPerPacket(uint8_t bitType)` → 256 / 128
  - `size_t BuildAudioPacket(uint8_t out[kMaxPacket], const char name[16], uint32_t nuFrame, int srIndex, uint8_t bitType, const float* interleavedStereo, size_t frames)` → total bytes (0 on bad args). Converts float→wire; **does not** apply gain/limiting (sender does that on the float block first).
  - `class Pacer` (vban_pacer.h): `void Configure(uint32_t rate, size_t framesPerPacket, unsigned burstPackets)`, `unsigned Due(uint64_t nowUs)`, `int64_t BehindUs(uint64_t nowUs) const`, `uint64_t Reanchors() const`

- [ ] **Step 1: Write the failing tests**

```cpp
MDXM_TEST_CASE(Vban_AudioPacketInt16) {
    float frames[256 * 2];
    for (int i = 0; i < 256; ++i) { frames[i*2] = 0.5f; frames[i*2+1] = -0.5f; }
    uint8_t out[kMaxPacket];
    size_t n = BuildAudioPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 7,
                                SampleRateIndex(48000), kBitInt16, frames, 256);
    CHECK(n == kHeaderSize + 256 * 2 * 2);       // 1024 B payload, 5.33 ms
    Parsed p = ParsePacket(out, n);
    CHECK(p.valid && p.proto == kProtoAudio);
    CHECK((p.hdr.format_SR & 0x1F) == 3);
    CHECK(p.hdr.format_nbs == 255);              // 256 stored minus one
    CHECK(p.hdr.format_nbc == 1);                // stereo stored minus one
    CHECK(p.hdr.format_bit == kBitInt16);        // PCM codec = high nibble 0
    CHECK(p.hdr.nuFrame == 7);
    int16_t s0; memcpy(&s0, p.data, 2);
    CHECK(s0 == 16383 || s0 == 16384);           // 0.5f scaled
}

MDXM_TEST_CASE(Vban_AudioPacketFloat32CapsAt128) {
    float frames[128 * 2] = {};
    uint8_t out[kMaxPacket];
    size_t n = BuildAudioPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 0,
                                3, kBitFloat32, frames, 128);
    CHECK(n == kHeaderSize + 128 * 2 * 4);       // 1024 B: fits; 179 is the hard max
    CHECK(AudioFramesPerPacket(kBitFloat32) == 128);
    CHECK(AudioFramesPerPacket(kBitInt16) == 256);
    // Refuse a frame count whose payload would exceed kMaxData.
    CHECK(BuildAudioPacket(out, "x\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 0, 3,
                           kBitFloat32, frames, 180) == 0);
}

MDXM_TEST_CASE(Vban_AudioPacketGainExtremes) {
    // Review Focus #2: full-scale input through the sender's gain+limit chain
    // then conversion must clamp, never wrap. The chain is: ApplyGain (sender),
    // SoftLimiter (sender), BuildAudioPacket (here). Simulate the chain.
    float frames[4 * 2];
    for (int i = 0; i < 8; ++i) frames[i] = 1.0f;         // already at full scale
    for (int i = 0; i < 8; ++i) frames[i] *= 64.0f;       // gain 6400%
    mdxm::SoftLimiter lim; lim.Process(frames, 4);        // what the sender runs
    // The limiter is a tanh shaper: output is < 1.0 but conversion must ALSO
    // clamp on its own -- the limiter is a sender policy, not a wire guarantee.
    uint8_t out[kMaxPacket];
    size_t n = BuildAudioPacket(out, "g\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 0, 3,
                                kBitInt16, frames, 4);
    Parsed p = ParsePacket(out, n);
    int16_t s; memcpy(&s, p.data, 2);
    CHECK(s <= 32767 && s >= -32768);
    // And a raw 64x overload WITHOUT the limiter still clamps (no wrap):
    float hot[2] = { 64.0f, -64.0f };
    n = BuildAudioPacket(out, "g\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 0, 3, kBitInt16, hot, 1);
    p = ParsePacket(out, n);
    int16_t l, r; memcpy(&l, p.data, 2); memcpy(&r, p.data + 2, 2);
    CHECK(l == 32767 && r == -32768);
}

MDXM_TEST_CASE(Vban_PacerPacesAndCatchesUp) {
    Pacer p;
    p.Configure(48000, 256, 4);                  // 256 frames = 5333 us/packet
    CHECK(p.Due(1000000) == 1);                  // first call anchors + grants one
    CHECK(p.Due(1000000) == 0);                  // nothing more due yet
    CHECK(p.Due(1005333) == 1);                  // one packet-time later
    CHECK(p.Due(1026665) == 4);                  // 4 packets behind: full burst
    CHECK(p.Due(1026665) == 0);                  // caught up
    // A huge stall (sendto wedged): re-anchor rather than granting hundreds.
    unsigned g = p.Due(3000000);                 // ~2 s behind
    CHECK(g <= 4);
    CHECK(p.Reanchors() == 1);
    CHECK(p.BehindUs(3000000) == 0);             // schedule reset to now
}
```

- [ ] **Step 2: Run to verify failure** — `build.ps1 Test`; expect compile failure (missing functions / vban_pacer.h).

- [ ] **Step 3: Implement**

Append to `vban_protocol.h` (inside the namespace):

```cpp
constexpr uint8_t kBitInt16 = 0x01, kBitFloat32 = 0x04;   // codec PCM = 0x00 high nibble
constexpr size_t  kFramesPerPacketI16 = 256, kFramesPerPacketF32 = 128;
size_t AudioFramesPerPacket(uint8_t bitType);
size_t BuildAudioPacket(uint8_t* out, const char name[kStreamNameSize],
                        uint32_t nuFrame, int srIndex, uint8_t bitType,
                        const float* interleavedStereo, size_t frames);
```

In `vban_protocol.cpp`:

```cpp
size_t AudioFramesPerPacket(uint8_t bitType) {
    return bitType == kBitFloat32 ? kFramesPerPacketF32 : kFramesPerPacketI16;
}

size_t BuildAudioPacket(uint8_t* out, const char name[kStreamNameSize],
                        uint32_t nuFrame, int srIndex, uint8_t bitType,
                        const float* interleavedStereo, size_t frames) {
    if (!out || !interleavedStereo || frames == 0 || frames > 256) return 0;
    if (srIndex < 0 || srIndex > 20) return 0;
    const size_t bytesPer = (bitType == kBitFloat32) ? 4 : 2;
    const size_t payload = frames * 2 * bytesPer;
    if (payload > kMaxData) return 0;
    Header h = {};
    h.vban = kMagic;
    h.format_SR  = (uint8_t)(kProtoAudio | (uint8_t)srIndex);
    h.format_nbs = (uint8_t)(frames - 1);
    h.format_nbc = 1;                       // stereo, stored minus one
    h.format_bit = bitType;                 // PCM codec: high nibble 0
    std::memcpy(h.streamname, name, kStreamNameSize);
    h.nuFrame = nuFrame;
    std::memcpy(out, &h, kHeaderSize);
    uint8_t* d = out + kHeaderSize;
    if (bitType == kBitFloat32) {
        std::memcpy(d, interleavedStereo, payload);
    } else {
        for (size_t i = 0; i < frames * 2; ++i) {
            float v = interleavedStereo[i];
            // Clamp HERE as well as in the limiter: the limiter is sender
            // policy; the wire must never wrap regardless.
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            const int s = (int)(v * 32767.0f);
            const int16_t w = (int16_t)(s < -32768 ? -32768 : s);
            std::memcpy(d + i * 2, &w, 2);
        }
    }
    return kHeaderSize + payload;
}
```

```cpp
// src/mdxmixer/net/vban_pacer.h
#pragma once
// When may the next AUDIO packet go out? (spec §3.2 step 3)
//
// A QPC-anchored schedule: one packet-duration per grant, a bounded catch-up
// burst when behind, and a re-anchor when hopelessly behind. Ring depth is
// PRODUCER LEAD (a Bluetooth personal render writes seconds ahead of its own
// playback), not latency -- so the pacer never skips content to shed depth; it
// only refuses to run ahead of real time. PURE: caller supplies the clock in
// microseconds; the audio thread never touches this.
#include <cstdint>

namespace mdxm { namespace vban {

class Pacer {
public:
    void Configure(uint32_t sampleRate, size_t framesPerPacket, unsigned burstPackets) {
        m_usPerPacket = (uint64_t)framesPerPacket * 1000000ull / sampleRate;
        m_burst = burstPackets ? burstPackets : 1;
        m_anchored = false;
    }
    // Packets that may be sent now; advances the schedule by what it grants.
    unsigned Due(uint64_t nowUs) {
        if (m_usPerPacket == 0) return 0;
        if (!m_anchored) { m_anchored = true; m_next = nowUs + m_usPerPacket; return 1; }
        if (nowUs < m_next) return 0;
        // Hopelessly behind (a wedged sendto, a laptop lid): re-anchor and
        // count it rather than granting hundreds of packets in one wake.
        if (nowUs - m_next > kMaxBehindUs) {
            ++m_reanchors;
            m_next = nowUs + m_usPerPacket;
            return m_burst;   // one burst to refill the receiver, then on schedule
        }
        unsigned due = (unsigned)((nowUs - m_next) / m_usPerPacket) + 1;
        if (due > m_burst) due = m_burst;
        m_next += (uint64_t)due * m_usPerPacket;
        return due;
    }
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
```

Add `#include "dsp/limiter.h"` to the test file for the gain test.

- [ ] **Step 4: Build + run; all tests pass.**

- [ ] **Step 5: Commit** — `feat: VBAN audio packetizer and send pacer`

---

### Task 3: SERVICE/PING0 — identification request/reply

**Files:**
- Modify: `src/mdxmixer/net/vban_protocol.h/.cpp`, `tests/test_vban_protocol.cpp`

**Interfaces:**
- Produces:
  - `constexpr uint8_t kServiceIdentification = 0;` (format_nbc), `kServiceFnPing0 = 0;` (format_nbs), `kServiceReplyBit = 0x80;`
  - `struct Ping0` — the 676-byte identification payload (packed), `static_assert(sizeof(Ping0) == 676)`
  - device/feature bits: `kDeviceTransmitter = 0x2, kDeviceVirtualMixer = 0x20, kFeatureAudio = 0x1, kFeatureFrame = 0x1000, kFeatureTxt = 0x10000`
  - `bool IsPing0Request(const Parsed& p)`
  - `size_t BuildPing0Reply(uint8_t* out /*>= 28+676*/, const Header& request, const Ping0& id)` — echoes the request's `nuFrame` (transaction id) and stream name, sets the reply bit, `format_SR = kProtoService` exactly (low 5 bits 0, spec p.27)

- [ ] **Step 1: Write the failing tests**

```cpp
MDXM_TEST_CASE(Vban_Ping0StructIs676Bytes) {
    CHECK(sizeof(Ping0) == 676);
}

MDXM_TEST_CASE(Vban_Ping0ReplyEchoesAndFlags) {
    Header req = {};
    req.vban = kMagic;
    req.format_SR = kProtoService;
    req.format_nbs = kServiceFnPing0;
    req.format_nbc = kServiceIdentification;
    FillStreamName(req.streamname, "VBAN Service");
    req.nuFrame = 0xDEADBEEF;
    uint8_t rbuf[64] = {};
    memcpy(rbuf, &req, sizeof req);
    Parsed preq = ParsePacket(rbuf, sizeof req);
    CHECK(IsPing0Request(preq));

    Ping0 id = {};
    id.bitType = kDeviceTransmitter | kDeviceVirtualMixer;
    id.bitFeature = kFeatureAudio | kFeatureTxt | kFeatureFrame;
    id.preferredRate = 48000;
    uint8_t out[kHeaderSize + sizeof(Ping0)];
    size_t n = BuildPing0Reply(out, req, id);
    CHECK(n == kHeaderSize + 676);
    Parsed p = ParsePacket(out, n);
    CHECK(p.valid && p.proto == kProtoService);
    CHECK(p.hdr.format_SR == kProtoService);          // low 5 bits MUST be 0 (spec p.27)
    CHECK(p.hdr.format_nbs == (kServiceFnPing0 | kServiceReplyBit));
    CHECK(p.hdr.format_nbc == kServiceIdentification);
    CHECK(p.hdr.nuFrame == 0xDEADBEEF);               // transaction id echoed
    CHECK(StreamNameIs(p.hdr, req.streamname));       // name echoed (spec §2.3)
    Ping0 got; memcpy(&got, p.data, sizeof got);
    CHECK(got.bitFeature == id.bitFeature);
}

MDXM_TEST_CASE(Vban_Ping0RequestDetection) {
    Header h = {}; h.vban = kMagic;
    h.format_SR = kProtoService; h.format_nbs = kServiceFnPing0;
    h.format_nbc = kServiceIdentification;
    uint8_t b[28]; memcpy(b, &h, 28);
    CHECK(IsPing0Request(ParsePacket(b, 28)));
    h.format_nbs = kServiceFnPing0 | kServiceReplyBit;      // a REPLY is not a request
    memcpy(b, &h, 28);
    CHECK(!IsPing0Request(ParsePacket(b, 28)));
    h.format_nbs = kServiceFnPing0; h.format_nbc = 32;      // RT-register: not ours
    memcpy(b, &h, 28);
    CHECK(!IsPing0Request(ParsePacket(b, 28)));
}
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement**

Header additions:

```cpp
constexpr uint8_t kServiceIdentification = 0;   // format_nbc
constexpr uint8_t kServiceFnPing0 = 0;          // format_nbs
constexpr uint8_t kServiceReplyBit = 0x80;

constexpr uint32_t kDeviceTransmitter = 0x2, kDeviceVirtualMixer = 0x20;
constexpr uint32_t kFeatureAudio = 0x1, kFeatureFrame = 0x1000, kFeatureTxt = 0x10000;

#pragma pack(push, 1)
// The spec's identification payload (rev 13 "SERVICE" chapter), 676 bytes.
struct Ping0 {
    uint32_t bitType, bitFeature, bitFeatureEx;
    uint32_t preferredRate, minRate, maxRate;
    uint32_t colorRgb;
    uint8_t  nVersion[4];
    char     gpsPosition[8], userPosition[8], langCode[8], reservedAscii[8];
    char     reservedEx[64];
    char     distantIp[32];
    uint16_t distantPort, distantReserved;
    char     deviceName[64], manufacturerName[64], applicationName[64], hostName[64];
    char     userName[128], userComment[128];
};
#pragma pack(pop)
static_assert(sizeof(Ping0) == 676, "spec's identification payload is 676 bytes");

bool   IsPing0Request(const Parsed& p);
size_t BuildPing0Reply(uint8_t* out, const Header& request, const Ping0& id);
```

Implementation:

```cpp
bool IsPing0Request(const Parsed& p) {
    return p.valid && p.proto == kProtoService &&
           p.hdr.format_nbc == kServiceIdentification &&
           p.hdr.format_nbs == kServiceFnPing0;        // reply bit clear
}

size_t BuildPing0Reply(uint8_t* out, const Header& request, const Ping0& id) {
    Header h = {};
    h.vban = kMagic;
    h.format_SR  = kProtoService;                      // low 5 bits 0, spec p.27
    h.format_nbs = (uint8_t)(kServiceFnPing0 | kServiceReplyBit);
    h.format_nbc = kServiceIdentification;
    h.format_bit = 0;
    std::memcpy(h.streamname, request.streamname, kStreamNameSize);
    h.nuFrame = request.nuFrame;                       // transaction id echoed
    std::memcpy(out, &h, kHeaderSize);
    std::memcpy(out + kHeaderSize, &id, sizeof(Ping0));
    return kHeaderSize + sizeof(Ping0);
}
```

- [ ] **Step 4: Build + run; pass.**
- [ ] **Step 5: Commit** — `feat: VBAN SERVICE/PING0 identification request and reply`

---

### Task 4: `VbanConfig` — config struct, JSON round-trip, clamps

**Files:**
- Modify: `src/mdxmixer/config/config.h` (struct + member in `MixerConfig`), `src/mdxmixer/config/config.cpp` (read/write)
- Create: `tests/test_vban_config.cpp`
- Modify: `src/mdxmixer/mdxmixer.vcxproj` (test entry)

**Interfaces:**
- Produces (spec §6.1; exact member names later tasks use):

```cpp
// config.h, above MixerConfig:
struct VbanAuthorizedDevice { std::wstring id, name; std::wstring lastSeen; };
struct VbanFramesConfig { double fps = 2.0; int quality = 60; int maxEdge = 480; };
struct VbanConfig {
    bool enabled = false;
    int  port = 6980;
    std::wstring bindAddress;                 // empty = any
    std::wstring streamName = L"mdxmixer";    // <= 16 ASCII on the wire
    bool sourceStreaming = false;             // false = personal (default)
    bool formatFloat32 = false;               // false = int16 (default)
    int  gainPercent = 100;                   // 0..6400
    std::wstring pin;                         // empty = TXT control disabled
    std::vector<VbanAuthorizedDevice> authorizedDevices;
    bool openSubscribe = false;
    bool alwaysStream = false;
    bool alwaysFrames = false;
    std::wstring alwaysStreamTarget;          // "ip:port"; empty = inert
    VbanFramesConfig frames;
};
// MixerConfig gains:  VbanConfig vban;
```

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/test_vban_config.cpp
#include "test_framework.h"
#include "config/config.h"
using namespace mdxm;

MDXM_TEST_CASE(VbanConfig_Defaults) {
    MixerConfig c;
    CHECK(!c.vban.enabled);
    CHECK(c.vban.port == 6980);
    CHECK(c.vban.streamName == L"mdxmixer");
    CHECK(!c.vban.sourceStreaming && !c.vban.formatFloat32);
    CHECK(c.vban.gainPercent == 100);
    CHECK(c.vban.pin.empty() && c.vban.authorizedDevices.empty());
    CHECK(!c.vban.openSubscribe && !c.vban.alwaysStream && !c.vban.alwaysFrames);
    CHECK(c.vban.frames.fps == 2.0 && c.vban.frames.quality == 60 &&
          c.vban.frames.maxEdge == 480);
}

MDXM_TEST_CASE(VbanConfig_RoundTrip) {
    MixerConfig c;
    c.vban.enabled = true;
    c.vban.port = 7000;
    c.vban.bindAddress = L"192.168.0.10";
    c.vban.streamName = L"rig";
    c.vban.sourceStreaming = true;
    c.vban.formatFloat32 = true;
    c.vban.gainPercent = 1600;
    c.vban.pin = L"4242";
    c.vban.authorizedDevices.push_back({ L"androidid1", L"Pixel 9", L"" });
    c.vban.openSubscribe = true;
    c.vban.alwaysStream = true;
    c.vban.alwaysFrames = true;
    c.vban.alwaysStreamTarget = L"192.168.0.77:6980";
    c.vban.frames = { 5.0, 80, 320 };
    MixerConfig back = ConfigFromJson(JsonParse(ConfigToJson(c)));
    CHECK(back.vban.enabled && back.vban.port == 7000);
    CHECK(back.vban.bindAddress == L"192.168.0.10");
    CHECK(back.vban.streamName == L"rig");
    CHECK(back.vban.sourceStreaming && back.vban.formatFloat32);
    CHECK(back.vban.gainPercent == 1600);
    CHECK(back.vban.pin == L"4242");
    CHECK(back.vban.authorizedDevices.size() == 1);
    CHECK(back.vban.authorizedDevices[0].id == L"androidid1");
    CHECK(back.vban.authorizedDevices[0].name == L"Pixel 9");
    CHECK(back.vban.openSubscribe && back.vban.alwaysStream && back.vban.alwaysFrames);
    CHECK(back.vban.alwaysStreamTarget == L"192.168.0.77:6980");
    CHECK(back.vban.frames.fps == 5.0 && back.vban.frames.quality == 80 &&
          back.vban.frames.maxEdge == 320);
}

MDXM_TEST_CASE(VbanConfig_Clamps) {
    MixerConfig c;
    c.vban.gainPercent = 999999;
    c.vban.port = -5;
    c.vban.frames.fps = 100.0;
    c.vban.frames.quality = 999;
    c.vban.frames.maxEdge = 9999;
    MixerConfig back = ConfigFromJson(JsonParse(ConfigToJson(c)));
    CHECK(back.vban.gainPercent == 6400);           // 0..6400 (spec §6.1)
    CHECK(back.vban.port == 6980);                  // nonsense port -> default
    CHECK(back.vban.frames.fps == 10.0);            // 0.2..10
    CHECK(back.vban.frames.quality == 95);          // 10..95 (WIC range sanity)
    CHECK(back.vban.frames.maxEdge == 1024);        // 64..1024
}
```

(If `JsonParse` is spelled differently in `json_utils.h` — check the header; `JsonLoadFile` exists, and the parse-from-string entry point is what `test_config.cpp`-style tests already use. Use the same call the existing config tests use.)

- [ ] **Step 2: Add vcxproj entry for `tests/test_vban_config.cpp`; build; expect compile failure (no `vban` member).**

- [ ] **Step 3: Implement**

`config.h`: add the three structs above `MixerConfig` (verbatim from Interfaces) and `VbanConfig vban;` as a member after `int logLevel = 2;`, with a one-line comment: `// VBAN stream server (spec docs/specs/2026-10-07-vban-stream-server-design.md §6.1)`.

`config.cpp` — in `ConfigToJson`, before the `complete` marker:

```cpp
w.BeginObject(L"vban");
w.Bool(L"enabled", c.vban.enabled);
w.Int(L"port", c.vban.port);
w.String(L"bindAddress", c.vban.bindAddress);
w.String(L"streamName", c.vban.streamName);
w.String(L"source", c.vban.sourceStreaming ? L"streaming" : L"personal");
w.String(L"format", c.vban.formatFloat32 ? L"f32" : L"i16");
w.Int(L"gainPercent", c.vban.gainPercent);
w.String(L"pin", c.vban.pin);
w.BeginArray(L"authorizedDevices");
for (const auto& d : c.vban.authorizedDevices) {
    w.BeginObject();
    w.String(L"id", d.id);
    w.String(L"name", d.name);
    w.String(L"lastSeen", d.lastSeen);
    w.EndObject();
}
w.EndArray();
w.Bool(L"openSubscribe", c.vban.openSubscribe);
w.Bool(L"alwaysStream", c.vban.alwaysStream);
w.Bool(L"alwaysFrames", c.vban.alwaysFrames);
w.String(L"alwaysStreamTarget", c.vban.alwaysStreamTarget);
w.BeginObject(L"frames");
Dbl(w, L"fps", c.vban.frames.fps);
w.Int(L"quality", c.vban.frames.quality);
w.Int(L"maxEdge", c.vban.frames.maxEdge);
w.EndObject();
w.EndObject();
```

In `ConfigFromJson`, before the return:

```cpp
{
    const JsonValue& v = root[L"vban"];
    c.vban.enabled = v[L"enabled"].asBool(false);
    c.vban.port = v[L"port"].asInt(6980);
    if (c.vban.port < 1 || c.vban.port > 65535) c.vban.port = 6980;
    c.vban.bindAddress = v[L"bindAddress"].asString();
    c.vban.streamName = v[L"streamName"].asString(L"mdxmixer");
    if (c.vban.streamName.empty()) c.vban.streamName = L"mdxmixer";
    c.vban.sourceStreaming = v[L"source"].asString(L"personal") == L"streaming";
    c.vban.formatFloat32 = v[L"format"].asString(L"i16") == L"f32";
    c.vban.gainPercent = std::clamp(v[L"gainPercent"].asInt(100), 0, 6400);
    c.vban.pin = v[L"pin"].asString();
    const JsonValue& devs = v[L"authorizedDevices"];
    for (size_t i = 0; i < devs.size(); ++i) {
        const JsonValue& d = devs.at(i);
        VbanAuthorizedDevice a{ d[L"id"].asString(), d[L"name"].asString(),
                                d[L"lastSeen"].asString() };
        if (!a.id.empty()) c.vban.authorizedDevices.push_back(a);
    }
    c.vban.openSubscribe = v[L"openSubscribe"].asBool(false);
    c.vban.alwaysStream = v[L"alwaysStream"].asBool(false);
    c.vban.alwaysFrames = v[L"alwaysFrames"].asBool(false);
    c.vban.alwaysStreamTarget = v[L"alwaysStreamTarget"].asString();
    const JsonValue& fr = v[L"frames"];
    c.vban.frames.fps = std::clamp(fr[L"fps"].asNumber(2.0), 0.2, 10.0);
    c.vban.frames.quality = std::clamp(fr[L"quality"].asInt(60), 10, 95);
    c.vban.frames.maxEdge = std::clamp(fr[L"maxEdge"].asInt(480), 64, 1024);
}
```

- [ ] **Step 4: Build + run; pass (existing config tests must stay green too).**
- [ ] **Step 5: Commit** — `feat: VbanConfig in mdxmixer.json with clamps and round-trip tests`

---

### Task 5: Engine tap — `m_vbanRing`, source select, reporting-only demand field

**Files:**
- Modify: `src/mdxmixer/engine/engine.h`, `src/mdxmixer/engine/engine.cpp`, `src/mdxmixer/engine/mix_demand.h`
- Modify: `tests/test_mix_demand.cpp`

**Interfaces:**
- Produces:
  - `Engine::SetVbanSink(bool on, bool streamingSource)` — control thread; clears the ring on enable
  - `RingBuffer& Engine::VbanRing()` — the sender thread's read side
  - `MixDemand` gains `bool vban = false;` — **excluded from `AnyoneListening`**

- [ ] **Step 1: Write the failing test**

Append to `tests/test_mix_demand.cpp`:

```cpp
MDXM_TEST_CASE(MixDemand_VbanIsReportingOnly) {
    // Spec §3.1: without a personal render MixPull never runs, so a vban bit
    // that AnyoneListening honoured would start captures nobody drains -- the
    // fj#10 failure (41.4M dropped frames) this field must never re-open.
    MixDemand d;
    d.vban = true;
    CHECK(!AnyoneListening(d));
    d.personalRender = true;
    CHECK(AnyoneListening(d));
}
```

- [ ] **Step 2: Run; FAIL — `MixDemand` has no `vban` member.**

- [ ] **Step 3: Implement**

`mix_demand.h` — add to the struct (after `bool feed = false;`):

```cpp
    // REPORTING ONLY -- deliberately NOT considered by AnyoneListening.
    // The VBAN sender is clocked by MixPull like the feed is, and MixPull runs
    // only on the personal render callback: with no personal render there is
    // no mix, and a demand bit here would start captures that fill rings
    // nobody drains -- the exact fj#10 failure above. When a timer-paced
    // fallback clock exists (spec §3.2 future work), this becomes a real
    // AnyoneListening term; not before.
    bool vban = false;
```

`engine.h` — public, after `FeedRate()`:

```cpp
    // The VBAN sink (spec §3.1): while on, MixPull writes the selected
    // post-limiter sum into m_vbanRing for the sender thread to drain.
    // Control thread. The ring is cleared on enable and on a mix-rate change.
    void SetVbanSink(bool on, bool streamingSource);
    bool VbanSinkOn() const { return m_vbanOn.load(std::memory_order_relaxed); }
    RingBuffer& VbanRing() { return m_vbanRing; }
```

private, beside the stream ring members:

```cpp
    // VBAN sink: ring written by MixPull, drained by VbanServer's sender
    // thread. 2 s capacity ABSORBS THE PRODUCER'S BURSTS (kMaxPullFrames, a
    // Bluetooth render writing seconds ahead of its own playback) -- it is not
    // a latency budget; the pacer never runs ahead of real time (vban_pacer.h).
    RingBuffer m_vbanRing{48000 * 2};
    std::atomic<bool> m_vbanOn{false};
    std::atomic<bool> m_vbanStreamingSrc{false};   // false = personal (default)
```

`engine.cpp` — in `MixPull`, directly after the `m_feedOn` line (461):

```cpp
        // The VBAN sink (spec §3.1): the monitor clone by default (m_pSum --
        // what the headphones get), or the streaming sum by config. Ring write
        // only: gain, conversion and the socket all live on the sender thread.
        if (m_vbanOn.load(std::memory_order_relaxed))
            m_vbanRing.Write(m_vbanStreamingSrc.load(std::memory_order_relaxed)
                                 ? m_sSum.data() : m_pSum.data(), frames);
```

In `ReconfigureForMixRate`, beside `m_streamRing.Clear();`:

```cpp
    m_vbanRing.Clear();   // content at the old rate describes a different clock
```

New method (near `SetFeedEnabled`):

```cpp
void Engine::SetVbanSink(bool on, bool streamingSource) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vbanStreamingSrc.store(streamingSource, std::memory_order_relaxed);
    if (on == m_vbanOn.load(std::memory_order_relaxed)) return;
    if (on) m_vbanRing.Clear();
    m_vbanOn.store(on, std::memory_order_relaxed);
    Log(2, L"vban sink: %s (%s mix)", on ? L"on" : L"off",
        streamingSource ? L"streaming" : L"personal");
}
```

In `CurrentDemand()` add `d.vban = m_vbanOn.load(std::memory_order_relaxed);`.

- [ ] **Step 4: Build Test + run; all pass (notably every existing `mix_demand`/engine test).**
- [ ] **Step 5: Commit** — `feat: engine VBAN sink ring with reporting-only demand field`

---

### Task 6: Peer table + phase-1 entitlement (pure)

**Files:**
- Create: `src/mdxmixer/net/vban_peers.h`, `src/mdxmixer/net/vban_peers.cpp`
- Create: `tests/test_vban_peers.cpp`
- Modify: `src/mdxmixer/mdxmixer.vcxproj` (two entries)

**Interfaces:**
- Produces (phase 2 extends this file with auth; define the enum now so the shape is stable):

```cpp
// net/vban_peers.h  -- PURE: opaque keys + tick ms, no winsock types.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mdxm { namespace vban {

struct PeerKey {
    uint32_t ip = 0; uint16_t port = 0;
    bool operator==(const PeerKey& o) const { return ip == o.ip && port == o.port; }
};

enum class AuthState { None, Pending, Authorized };

struct Peer {
    PeerKey key;
    uint64_t lastPingMs = 0;
    AuthState auth = AuthState::None;
    std::wstring deviceId, deviceName;
    bool audioOn = true;        // per-peer audio=0|1 (spec §4), default 1
    bool framesOn = false;      // per-peer frames=0|1, default 0
    uint32_t txTxtNuFrame = 0;  // our TXT sequence to this peer
};

class PeerTable {
public:
    static constexpr size_t kMaxPeers = 8;
    static constexpr uint64_t kExpiryMs = 10000;
    // PING0 seen from k: create or renew. Returns the peer, or nullptr when
    // the table is full and nothing may be evicted for an unauthenticated
    // newcomer (spec §3.3: expired first, then oldest unauthenticated; an
    // authenticated entry is NEVER evicted for an unauthenticated pinger).
    Peer* Ping(PeerKey k, uint64_t nowMs);
    void  Expire(uint64_t nowMs);           // drop stale peers; auth dies with them
    Peer* Find(PeerKey k);
    const std::vector<Peer>& All() const { return m_peers; }
    size_t CountAudioEntitled(bool pinSet, bool openSubscribe) const;

private:
    std::vector<Peer> m_peers;   // <= kMaxPeers; vector for testable iteration
};

// Entitlement, straight from the spec's §4 matrix. Pure predicates so the
// matrix is a table of test cases rather than scattered ifs.
bool AudioEntitled(const Peer& p, bool pinSet, bool openSubscribe);
bool FramesEntitled(const Peer& p, bool pinSet);
// §3.1: the sender-side demand predicate.
bool VbanWanted(bool enabled, size_t audioEntitledPeers,
                bool alwaysStream, bool targetSet);

}} // namespace mdxm::vban
```

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/test_vban_peers.cpp
#include "test_framework.h"
#include "net/vban_peers.h"
using namespace mdxm::vban;

MDXM_TEST_CASE(VbanPeers_PingCreatesRenewsExpires) {
    PeerTable t;
    PeerKey k{ 0x0A000001, 50000 };
    Peer* p = t.Ping(k, 1000);
    CHECK(p && t.All().size() == 1);
    t.Ping(k, 5000);                       // renew, not duplicate
    CHECK(t.All().size() == 1);
    t.Expire(14999);                       // 9999 ms since last ping: still alive
    CHECK(t.All().size() == 1);
    t.Expire(15001);                       // 10001 ms: gone, auth and all
    CHECK(t.All().empty());
}

MDXM_TEST_CASE(VbanPeers_TableFullEviction) {
    // Review Focus #3: a LAN scanner must not evict the phone.
    PeerTable t;
    for (uint16_t i = 0; i < 8; ++i)
        t.Ping(PeerKey{ 0x0A000001, (uint16_t)(50000 + i) }, 1000);
    CHECK(t.All().size() == 8);
    // Slot 3 is the authorized phone.
    t.All();   // (const access) -- mutate via Find:
    t.Find(PeerKey{ 0x0A000001, 50003 })->auth = AuthState::Authorized;
    // A 9th unauthenticated pinger with nothing expired: evicts the OLDEST
    // UNAUTHENTICATED (50000, pinged at 1000), never the authorized one.
    t.Ping(PeerKey{ 0x0A000001, 50001 }, 2000);   // make 50000 the oldest
    Peer* np = t.Ping(PeerKey{ 0x0B000001, 60000 }, 3000);
    CHECK(np != nullptr);
    CHECK(t.Find(PeerKey{ 0x0A000001, 50000 }) == nullptr);   // evicted
    CHECK(t.Find(PeerKey{ 0x0A000001, 50003 }) != nullptr);   // phone survives
    // All 8 authorized + a 9th unauthenticated: refused, reply still stateless.
    for (auto& pr : t.All()) {} // no-op; set all authorized via Find:
    for (uint16_t i = 0; i < 8; ++i)
        if (Peer* q = t.Find(PeerKey{ 0x0A000001, (uint16_t)(50000 + i) }))
            q->auth = AuthState::Authorized;
    if (Peer* q = t.Find(PeerKey{ 0x0B000001, 60000 })) q->auth = AuthState::Authorized;
    CHECK(t.Ping(PeerKey{ 0x0C000001, 60001 }, 4000) == nullptr);
}

MDXM_TEST_CASE(VbanPeers_EntitlementMatrixPhase1) {
    Peer anon;  anon.auth = AuthState::None;
    Peer authd; authd.auth = AuthState::Authorized;
    // pin set, openSubscribe off: only authorized peers hear audio.
    CHECK(!AudioEntitled(anon,  true, false));
    CHECK( AudioEntitled(authd, true, false));
    // openSubscribe on: any pinger hears audio (and ONLY audio).
    CHECK( AudioEntitled(anon,  true, true));
    CHECK( AudioEntitled(anon,  false, true));     // pin empty + open: audio ok
    // pin empty, open off: discovery-only posture -- nobody hears anything.
    CHECK(!AudioEntitled(anon,  false, false));
    CHECK(!AudioEntitled(authd, false, false));    // authorizedDevices are INERT (spec §4)
    // Per-peer audio=0 opts an authorized peer out (control-only session).
    authd.audioOn = false;
    CHECK(!AudioEntitled(authd, true, false));
    // Frames: authorized + framesOn, and pin must be set.
    Peer f; f.auth = AuthState::Authorized; f.framesOn = true;
    CHECK( FramesEntitled(f, true));
    CHECK(!FramesEntitled(f, false));
    f.framesOn = false;
    CHECK(!FramesEntitled(f, true));
    Peer anonF; anonF.framesOn = true;
    CHECK(!FramesEntitled(anonF, true));
}

MDXM_TEST_CASE(VbanPeers_VbanWanted) {
    CHECK(!VbanWanted(false, 5, true, true));      // listener off: never
    CHECK( VbanWanted(true, 1, false, false));
    CHECK(!VbanWanted(true, 0, false, false));
    CHECK( VbanWanted(true, 0, true, true));       // alwaysStream + target
    CHECK(!VbanWanted(true, 0, true, false));      // target empty = inert (spec §4)
}

MDXM_TEST_CASE(VbanPeers_CountAudioEntitled) {
    PeerTable t;
    t.Ping(PeerKey{1,1}, 0);
    t.Ping(PeerKey{1,2}, 0);
    t.Find(PeerKey{1,2})->auth = AuthState::Authorized;
    CHECK(t.CountAudioEntitled(true, false) == 1);
    CHECK(t.CountAudioEntitled(true, true)  == 2);
    CHECK(t.CountAudioEntitled(false, false) == 0);
}
```

- [ ] **Step 2: Add vcxproj entries; build; expect failure.**

- [ ] **Step 3: Implement `vban_peers.cpp`**

```cpp
#include "net/vban_peers.h"
#include <algorithm>

namespace mdxm { namespace vban {

Peer* PeerTable::Find(PeerKey k) {
    for (auto& p : m_peers)
        if (p.key == k) return &p;
    return nullptr;
}

Peer* PeerTable::Ping(PeerKey k, uint64_t nowMs) {
    if (Peer* p = Find(k)) { p->lastPingMs = nowMs; return p; }
    if (m_peers.size() >= kMaxPeers) {
        // Expired first, then the oldest unauthenticated. An authenticated
        // entry is never evicted for an unauthenticated newcomer (spec §3.3).
        int victim = -1;
        for (size_t i = 0; i < m_peers.size(); ++i)
            if (nowMs - m_peers[i].lastPingMs > kExpiryMs) { victim = (int)i; break; }
        if (victim < 0) {
            uint64_t oldest = UINT64_MAX;
            for (size_t i = 0; i < m_peers.size(); ++i)
                if (m_peers[i].auth != AuthState::Authorized &&
                    m_peers[i].lastPingMs < oldest) {
                    oldest = m_peers[i].lastPingMs; victim = (int)i;
                }
        }
        if (victim < 0) return nullptr;        // all 8 authorized: refuse
        m_peers.erase(m_peers.begin() + victim);
    }
    Peer p; p.key = k; p.lastPingMs = nowMs;
    m_peers.push_back(p);
    return &m_peers.back();
}

void PeerTable::Expire(uint64_t nowMs) {
    m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(),
        [nowMs](const Peer& p) { return nowMs - p.lastPingMs > kExpiryMs; }),
        m_peers.end());
}

size_t PeerTable::CountAudioEntitled(bool pinSet, bool openSubscribe) const {
    size_t n = 0;
    for (const auto& p : m_peers)
        if (AudioEntitled(p, pinSet, openSubscribe)) ++n;
    return n;
}

bool AudioEntitled(const Peer& p, bool pinSet, bool openSubscribe) {
    if (openSubscribe) return true;                       // any pinger, audio only
    if (!pinSet) return false;                            // authorizedDevices inert
    return p.auth == AuthState::Authorized && p.audioOn;
}

bool FramesEntitled(const Peer& p, bool pinSet) {
    return pinSet && p.auth == AuthState::Authorized && p.framesOn;
}

bool VbanWanted(bool enabled, size_t audioEntitledPeers,
                bool alwaysStream, bool targetSet) {
    if (!enabled) return false;
    return audioEntitledPeers > 0 || (alwaysStream && targetSet);
}

}} // namespace mdxm::vban
```

- [ ] **Step 4: Build + run; pass. One test touches `All()` mutation awkwardly — if `All()` const access reads wrong, fix the TEST to use `Find` (as written above), not the API.**
- [ ] **Step 5: Commit** — `feat: VBAN peer table with eviction and phase-1 entitlement`

---

### Task 7: `VbanServer` — socket, receive thread (PING0), paced sender

**Files:**
- Create: `src/mdxmixer/net/vban_server.h`, `src/mdxmixer/net/vban_server.cpp`
- Create: `tests/test_vban_server.cpp`
- Modify: `src/mdxmixer/mdxmixer.vcxproj` (two entries)

**Interfaces:**
- Produces:

`VbanStatus` lives in **`ipc/mixer_control.h`** from the start (it is the
IMixerControl currency Task 8 needs, and that header is Windows-free);
`net/vban_server.h` includes it.

```cpp
// ipc/mixer_control.h — added by THIS task, used by Task 8's virtuals.
// Everything MDXM_VBANSTATE reports (spec §6.3), snapshotted under one mutex.
struct VbanStatus {
    bool on = false, emitting = false;
    int port = 0; std::wstring name;
    int peers = 0;
    bool sourceStreaming = false, formatFloat32 = false;
    int gainPercent = 100; double fps = 2.0;
    bool open = false, always = false, alwaysFrames = false;
    std::wstring target;
    uint64_t sent = 0, starved = 0, dropped = 0, framesSent = 0;
    int depthMs = 0, behindMs = 0, srcLatencyMs = 0;
    int authPending = 0;
    std::wstring lastError;          // bind failure etc. -- never silent
};
```

```cpp
// net/vban_server.h
#pragma once
#include "config/config.h"           // VbanConfig
#include "dsp/ring_buffer.h"
#include "ipc/mixer_control.h"       // VbanStatus
#include "net/vban_peers.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mdxm {

class VbanServer {
public:
    struct Callbacks {
        // Marshalled MDXM dispatch (bound to DispatchToUi in phase 2; phase 1
        // passes an empty function and TXT packets are counted + dropped).
        std::function<std::vector<std::wstring>(const std::wstring&)> dispatch;
    };
    ~VbanServer();
    // ring = Engine::VbanRing(). mixRate for the SR index; UpdateMixRate on change.
    bool Start(const VbanConfig& cfg, RingBuffer* ring, uint32_t mixRate,
               Callbacks cb, std::wstring* err);
    void Stop();
    void UpdateConfig(const VbanConfig& cfg);    // control thread, any field
    void UpdateMixRate(uint32_t rate);
    // cushionMs + packet-ms + pacer-wake margin, computed by the app layer:
    // the PC term of the phone's lag estimate (spec §6.3 srclatencyms).
    void SetSrcLatencyBase(int ms);
    VbanStatus Status() const;
    bool Running() const;

private:
    void ReceiveLoop();
    void SendLoop();
    // ... socket handle, threads, PeerTable + m_mutex, pacer, atomics ...
};

} // namespace mdxm
```

Key implementation decisions (write them exactly like this):

- `#pragma comment(lib, "ws2_32.lib")` at the top of `vban_server.cpp`; `WSAStartup` once in `Start` (guarded by a static `std::once_flag`), `WSACleanup` never (process lifetime — matches the leak-on-wedge posture of `CaptureStream::Stop`).
- One `SOCKET` bound to `cfg.bindAddress:cfg.port` (`INADDR_ANY` when empty). Bind failure → `*err` + `m_status.lastError` + return false — **Review Focus #4**; the caller surfaces it, never retries silently.
- Receive thread: `recvfrom` with a 1000 ms `SO_RCVTIMEO` so `Stop()` can join without `closesocket` races; loop body wrapped try/catch per no-crash rule. On datagram: `ParsePacket`; `IsPing0Request` → `PeerTable::Ping` (under `m_mutex`) + `BuildPing0Reply` + `sendto` back to the source address (identification reply is stateless — sent even when the table refused an entry). `kProtoTxt` → phase 2 (count `m_txtDropped` for now). Everything else ignored.
- The PING0 identity block is built once per `UpdateConfig`:
  `bitType = kDeviceTransmitter|kDeviceVirtualMixer`, `bitFeature = kFeatureAudio|kFeatureTxt|kFeatureFrame`, `preferredRate = mixRate`, `minRate = 44100`, `maxRate = 192000`, `nVersion = {MDXM_VERSION_MAJOR, MDXM_VERSION_MINOR, MDXM_VERSION_PATCH, 0}` (include `version.h`), `deviceName = applicationName = "mdxmixer"`, `hostName` from `GetComputerNameA`.
- Sender thread, 1 ms wait loop (`WaitForSingleObject` on the stop event with 1 ms timeout; this thread may call clocks — it is not the audio thread): compute `nowUs` from `QueryPerformanceCounter`; `grant = m_pacer.Due(nowUs)`; for each granted packet: snapshot entitled recipients (under `m_mutex`: every peer where `AudioEntitled(...)` plus the parsed `alwaysStreamTarget` when `alwaysStream`); if none → skip (pacer stays anchored; `emitting=false`). Read `framesPerPacket` frames from the ring into a float scratch; if the ring returned fewer, zero-fill the remainder and `++m_starved` (silence keeps the cadence, spec §3.2 step 5). Apply gain (`gainPercent/100.0f`), run the member `SoftLimiter`, `BuildAudioPacket` with the shared `m_nuFrame++`, `sendto` each recipient, `++m_sent`.
- `Expire` peers once per second on the sender thread (same `m_mutex`).
- `UpdateConfig` re-derives everything except the socket; a port/bind change while running closes and rebinds (reply-before-rebind is a phase-2 concern at the verb layer; here just rebind).
- `Status()` fills `depthMs` from `ring->Depth()*1000/rate`, `behindMs` from `m_pacer.BehindUs`, `srcLatencyMs = cushionMs-equivalent` — the server cannot see the engine cushion, so `Start`/`UpdateConfig` take it: add `int srcCushionMs` to `Start`/`UpdateConfig` via a `SetSrcLatencyBase(int ms)` setter the app layer calls (cushion + packet-ms + 2).
- `alwaysStreamTarget` parse: `"ip:port"` via `inet_pton` + `wcstol`; invalid → treated as empty (inert), noted in `lastError`.

- [ ] **Step 1: Write the failing loopback tests** (no audio devices needed — this is a plain test, not `Audio_`):

```cpp
// tests/test_vban_server.cpp
#include "test_framework.h"
#include "net/vban_server.h"
#include "net/vban_protocol.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <chrono>
#pragma comment(lib, "ws2_32.lib")
using namespace mdxm;
using namespace mdxm::vban;

namespace {
constexpr int kTestPort = 46980;   // not 6980: a running mdxmixer must not interfere
SOCKET MakeClient(int* outPort) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a = {}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr*)&a, sizeof a);
    int len = sizeof a; getsockname(s, (sockaddr*)&a, &len);
    *outPort = ntohs(a.sin_port);
    DWORD tmo = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tmo, sizeof tmo);
    return s;
}
void SendPing0(SOCKET s, uint32_t txn) {
    Header h = {}; h.vban = kMagic; h.format_SR = kProtoService;
    h.format_nbs = kServiceFnPing0; h.format_nbc = kServiceIdentification;
    FillStreamName(h.streamname, "VBAN Service"); h.nuFrame = txn;
    sockaddr_in to = {}; to.sin_family = AF_INET; to.sin_port = htons(kTestPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s, (const char*)&h, sizeof h, 0, (sockaddr*)&to, sizeof to);
}
} // namespace

MDXM_TEST_CASE(Vban_Ping0GetsIdentificationReply) {
    WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    VbanServer srv;
    std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    int cport = 0; SOCKET c = MakeClient(&cport);
    SendPing0(c, 1234);
    uint8_t buf[2048]; sockaddr_in from; int flen = sizeof from;
    int n = recvfrom(c, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &flen);
    CHECK(n == (int)(kHeaderSize + sizeof(Ping0)));
    Parsed p = ParsePacket(buf, (size_t)n);
    CHECK(p.valid && p.proto == kProtoService);
    CHECK(p.hdr.format_nbs == (kServiceFnPing0 | kServiceReplyBit));
    CHECK(p.hdr.nuFrame == 1234);
    Ping0 id; memcpy(&id, p.data, sizeof id);
    CHECK((id.bitFeature & kFeatureAudio) != 0);
    CHECK(std::string(id.deviceName) == "mdxmixer");
    closesocket(c);
    srv.Stop();
}

MDXM_TEST_CASE(Vban_OpenSubscribePingerReceivesPacedAudio) {
    RingBuffer ring(48000 * 2);
    // Pre-fill one second of a known tone so starvation isn't the test.
    std::vector<float> tone(48000 * 2, 0.25f);
    ring.Write(tone.data(), 48000);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    cfg.openSubscribe = true;                       // phase-1 entitlement path
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    int cport = 0; SOCKET c = MakeClient(&cport);
    SendPing0(c, 1);
    // First packet is the PING0 reply; after it, AUDIO at ~5.33 ms cadence.
    uint8_t buf[2048]; sockaddr_in from; int flen = sizeof from;
    int audioPackets = 0; uint32_t firstSeq = 0, lastSeq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
        int n = recvfrom(c, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &flen);
        if (n <= 0) break;
        Parsed p = ParsePacket(buf, (size_t)n);
        if (!p.valid || p.proto != kProtoAudio) continue;
        CHECK(p.hdr.format_nbs == 255 && p.hdr.format_nbc == 1);
        CHECK((p.hdr.format_SR & 0x1F) == 3);
        if (audioPackets == 0) firstSeq = p.hdr.nuFrame;
        lastSeq = p.hdr.nuFrame;
        ++audioPackets;
    }
    // ~300 ms at 5.33 ms/packet ≈ 56; pacing means well under a flood and
    // comfortably above a trickle. Sequence must be contiguous.
    CHECK(audioPackets > 30 && audioPackets < 80);
    CHECK(lastSeq - firstSeq == (uint32_t)(audioPackets - 1));
    int16_t s; memcpy(&s, buf + kHeaderSize, 2);
    CHECK(s > 7000 && s < 9000);                    // 0.25f * 32767 ≈ 8192
    closesocket(c);
    VbanStatus st = srv.Status();
    CHECK(st.emitting);
    CHECK(st.sent >= (uint64_t)audioPackets);
    srv.Stop();
}

MDXM_TEST_CASE(Vban_BindFailureIsReported) {
    // Review Focus #4 and the MdnsDiscovery lesson: never a silent swallow.
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;
    VbanServer a, b; std::wstring err;
    CHECK(a.Start(cfg, &ring, 48000, {}, &err));
    CHECK(!b.Start(cfg, &ring, 48000, {}, &err));
    CHECK(!err.empty());
    CHECK(!b.Status().lastError.empty());
    a.Stop();
}

MDXM_TEST_CASE(Vban_NoSubscriberNoPackets) {
    RingBuffer ring(48000 * 2);
    std::vector<float> tone(9600 * 2, 0.5f);
    ring.Write(tone.data(), 9600);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort;   // open=false, no pings
    VbanServer srv; std::wstring err;
    CHECK(srv.Start(cfg, &ring, 48000, {}, &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    VbanStatus st = srv.Status();
    CHECK(st.sent == 0);              // nothing asked: nothing emitted (spec §1)
    CHECK(!st.emitting);
    srv.Stop();
}
```

- [ ] **Step 2: vcxproj entries; build; expect failure.**
- [ ] **Step 3: Implement `vban_server.{h,cpp}` to the decisions above.** The two thread bodies follow the house shape — `_beginthreadex`, whole body `try { … } catch (const std::exception&) { Log(1, …); } catch (...) { Log(1, …); }`, a manual-reset stop event checked each iteration. `Stop()` sets the event, `closesocket`, joins both threads, resets state.
- [ ] **Step 4: Build + run until all four pass.** Timing-sensitive bounds (30..80) are deliberately wide; if flaky on the build machine, widen further rather than sleeping longer.
- [ ] **Step 5: Commit** — `feat: VbanServer -- PING0 subscription and paced audio emission`

---

### Task 8: App wiring + `MDXM_VBAN` / `MDXM_VBANSTATE` + docs

**Files:**
- Modify: `src/mdxmixer/ipc/mixer_control.h`, `src/mdxmixer/ipc/protocol.cpp`, `src/mdxmixer/app/app_controller.h/.cpp`, `docs/ipc.md`, `tests/test_protocol.cpp`, `tests/test_docs.cpp`

**Interfaces:**
- Produces on `IMixerControl` (phase 2 adds peers/revoke):

```cpp
    // ── VBAN stream server (spec §6.3) ──────────────────────────────
    virtual VbanStatus GetVbanStatus() = 0;
    // One keyed field: "on","port","name","source","format","gain","fps",
    // "open","always","alwaysframes","target","pin". Validates + clamps,
    // applies live, persists via the config store. Unknown key -> false+err.
    virtual bool SetVbanOption(const std::wstring& key, const std::wstring& value,
                               std::wstring* err) = 0;
```

(`VbanStatus` moves to `mixer_control.h`? No — `mixer_control.h` must stay Windows-free and `vban_server.h` already is except `config.h`; **define `VbanStatus` in `ipc/mixer_control.h`** and have `net/vban_server.h` include it, keeping dependency direction protocol→control clean.)

- Produces in `protocol.cpp`: verbs `MDXM_VBAN` (keyed set/query) and reply records `MDXM_VBANSTATE`; exported `std::wstring VbanStateRecord(const VbanStatus&)` in `protocol.h` (one formatter, the `ChannelRecord` lesson).

- [ ] **Step 1: Write the failing protocol tests** (extend the existing fake in `tests/test_protocol.cpp` — add the two new virtuals to its `FakeMixerControl` with a captured-calls vector, like its existing fakes):

```cpp
MDXM_TEST_CASE(Protocol_VbanQueryReturnsState) {
    FakeMixerControl fake;                      // fake.vbanStatus pre-filled
    fake.vbanStatus.on = true; fake.vbanStatus.port = 6980;
    fake.vbanStatus.gainPercent = 1600; fake.vbanStatus.emitting = true;
    auto r = HandleProtocolMessage(L"MDXM_VBAN", fake, nullptr);
    CHECK(r.size() == 1);
    CHECK(r[0].find(L"MDXM_VBANSTATE|on=1|emitting=1|port=6980") == 0);
    CHECK(r[0].find(L"|gain=1600") != std::wstring::npos);
    CHECK(r[0].find(L"|srclatencyms=") != std::wstring::npos);
}

MDXM_TEST_CASE(Protocol_VbanKeyedSetIsSetNotQuery) {
    // These are the protocol's FIRST keyed-argument inbound verbs: the handler
    // must read r.Find(), never the positional vector 'a' (which is EMPTY for
    // a keyed record -- the MDXM_FEED-shaped bug the spec §6.3 warns about).
    FakeMixerControl fake;
    auto r = HandleProtocolMessage(L"MDXM_VBAN|gain=250", fake, nullptr);
    CHECK(fake.vbanSets.size() == 1);
    CHECK(fake.vbanSets[0] == std::make_pair(std::wstring(L"gain"), std::wstring(L"250")));
    CHECK(r.size() == 1 && r[0].rfind(L"MDXM_VBANSTATE|", 0) == 0);  // echo = state
    r = HandleProtocolMessage(L"MDXM_VBAN|on=1|port=7001", fake, nullptr);
    CHECK(fake.vbanSets.size() == 3);
}

MDXM_TEST_CASE(Protocol_VbanPerPeerKeysAreVbanOnlyOnPipe) {
    FakeMixerControl fake;
    auto r = HandleProtocolMessage(L"MDXM_VBAN|frames=1", fake, nullptr);
    CHECK(r.size() == 1 && r[0] == L"MDXM_ERR|msg=frames is VBAN-only");
    r = HandleProtocolMessage(L"MDXM_VBAN|audio=0", fake, nullptr);
    CHECK(r[0] == L"MDXM_ERR|msg=audio is VBAN-only");
    r = HandleProtocolMessage(L"MDXM_AUTH|pin=1|device=d|name=n", fake, nullptr);
    CHECK(r[0] == L"MDXM_ERR|msg=auth is VBAN-only");
    CHECK(fake.vbanSets.empty());
}

MDXM_TEST_CASE(Protocol_VbanBadValuesError) {
    FakeMixerControl fake;
    fake.vbanSetFails = true;                   // fake returns false + err text
    auto r = HandleProtocolMessage(L"MDXM_VBAN|gain=purple", fake, nullptr);
    CHECK(r.size() == 1 && r[0].rfind(L"MDXM_ERR|", 0) == 0);
}
```

- [ ] **Step 2: Run; expect compile failures (new virtuals) across every IMixerControl implementer — fake(s) and AppController.**

- [ ] **Step 3: Implement**

1. `mixer_control.h`: move/define `struct VbanStatus` here (exact fields from Task 7) + the two virtuals. `net/vban_server.h` includes `ipc/mixer_control.h` for it.
2. `protocol.h`: `std::wstring VbanStateRecord(const VbanStatus& s);`
3. `protocol.cpp` — formatter + handler (full field list, no ellipsis — spec §6.3):

```cpp
std::wstring VbanStateRecord(const VbanStatus& s) {
    wchar_t fps[32]; swprintf(fps, 32, L"%g", s.fps);
    return L"MDXM_VBANSTATE|on=" + std::wstring(s.on ? L"1" : L"0") +
           L"|emitting=" + (s.emitting ? L"1" : L"0") +
           L"|port=" + std::to_wstring(s.port) +
           L"|name=" + s.name +
           L"|peers=" + std::to_wstring(s.peers) +
           L"|source=" + (s.sourceStreaming ? L"streaming" : L"personal") +
           L"|format=" + (s.formatFloat32 ? L"f32" : L"i16") +
           L"|gain=" + std::to_wstring(s.gainPercent) +
           L"|fps=" + fps +
           L"|open=" + (s.open ? L"1" : L"0") +
           L"|always=" + (s.always ? L"1" : L"0") +
           L"|alwaysframes=" + (s.alwaysFrames ? L"1" : L"0") +
           L"|target=" + s.target +
           L"|sent=" + std::to_wstring(s.sent) +
           L"|starved=" + std::to_wstring(s.starved) +
           L"|dropped=" + std::to_wstring(s.dropped) +
           L"|depthms=" + std::to_wstring(s.depthMs) +
           L"|behindms=" + std::to_wstring(s.behindMs) +
           L"|srclatencyms=" + std::to_wstring(s.srcLatencyMs) +
           L"|framessent=" + std::to_wstring(s.framesSent) +
           L"|authpending=" + std::to_wstring(s.authPending) +
           L"|error=" + s.lastError;
}
```

Handler in `HandleInner` (place near `MDXM_FEED`):

```cpp
    // The VBAN stream server (spec §6.3). THE FIRST KEYED-ARGUMENT INBOUND
    // VERBS: read r.fields, never the positional vector -- a keyed record's
    // 'a' is EMPTY, and the MDXM_FEED-shaped "empty args means query" rule
    // would silently turn every keyed set into a query.
    if (r.verb == L"MDXM_VBAN") {
        bool anyKeyed = false;
        for (const auto& f : r.fields) {
            if (f.first.empty()) continue;
            anyKeyed = true;
            if (f.first == L"frames") return Err(L"frames is VBAN-only");
            if (f.first == L"audio")  return Err(L"audio is VBAN-only");
            std::wstring verr;
            if (!ctl.SetVbanOption(f.first, f.second, &verr))
                return Err(verr.empty() ? L"bad vban option: " + f.first : verr);
        }
        (void)anyKeyed;   // set or query, the reply is the state either way
        return { VbanStateRecord(ctl.GetVbanStatus()) };
    }

    // MDXM_AUTH is meaningful only over VBAN-TXT, where VbanServer answers it
    // before dispatch (spec §4). Reaching this handler means the pipe.
    if (r.verb == L"MDXM_AUTH")
        return Err(L"auth is VBAN-only");
```

4. `app_controller.h/.cpp`:
   - Members: `VbanServer m_vban;` + startup wiring in `Run` after the pipe starts:

```cpp
    // The VBAN listener persists with vban.enabled (unlike MDXM_FEED): the
    // phone must be able to subscribe at any moment; EMISSION stays
    // subscriber-gated inside the server (spec §3.3).
    ApplyVbanConfig();
```

   - `void AppController::ApplyVbanConfig()` — reads `m_store.Get().vban`; when enabled: `m_engine.SetVbanSink(true, cfg.vban.sourceStreaming)`, `m_vban.Start(cfg.vban, &m_engine.VbanRing(), m_engine.MixRate(), {}, &err)` (log the error at level 1 on failure, keep running); when disabled: `m_vban.Stop(); m_engine.SetVbanSink(false, …)`. Also `m_vban.SetSrcLatencyBase(GetCushionMs() + (cfg.vban.formatFloat32 ? 3 : 6) + 2);`
   - `GetVbanStatus()` → `m_vban.Status()` (fill `on` from config when the server is stopped so the record is honest).
   - `SetVbanOption(key, value, err)` — the clamp → engine/server call → config mutate → `BroadcastState()` shape:

```cpp
bool AppController::SetVbanOption(const std::wstring& key, const std::wstring& value,
                                  std::wstring* err) {
    MixerConfig& cur = m_store.Get();
    VbanConfig v = cur.vban;                     // edit a copy, apply, persist
    if (key == L"on") { bool b; if (!ParseBool01W(value, &b)) { *err = L"on wants 0|1"; return false; } v.enabled = b; }
    else if (key == L"port") { int p = _wtoi(value.c_str()); if (p < 1 || p > 65535) { *err = L"bad port"; return false; } v.port = p; }
    else if (key == L"name") { if (value.empty() || value.size() > 16) { *err = L"name wants 1..16 chars"; return false; } v.streamName = value; }
    else if (key == L"source") { if (value != L"personal" && value != L"streaming") { *err = L"source wants personal|streaming"; return false; } v.sourceStreaming = value == L"streaming"; }
    else if (key == L"format") { if (value != L"i16" && value != L"f32") { *err = L"format wants i16|f32"; return false; } v.formatFloat32 = value == L"f32"; }
    else if (key == L"gain") { int g = _wtoi(value.c_str()); if (g < 0 || g > 6400) { *err = L"gain wants 0..6400"; return false; } v.gainPercent = g; }
    else if (key == L"fps") { double f = _wtof(value.c_str()); if (f < 0.2 || f > 10.0) { *err = L"fps wants 0.2..10"; return false; } v.frames.fps = f; }
    else if (key == L"open") { bool b; if (!ParseBool01W(value, &b)) { *err = L"open wants 0|1"; return false; } v.openSubscribe = b; }
    else if (key == L"always") { bool b; if (!ParseBool01W(value, &b)) { *err = L"always wants 0|1"; return false; } v.alwaysStream = b; }
    else if (key == L"alwaysframes") { bool b; if (!ParseBool01W(value, &b)) { *err = L"alwaysframes wants 0|1"; return false; } v.alwaysFrames = b; }
    else if (key == L"target") { v.alwaysStreamTarget = value; }
    else if (key == L"pin") { v.pin = value; }     // pipe-only by caller contract (§6.3)
    else { *err = L"unknown vban option: " + key; return false; }
    m_store.Mutate([&](MixerConfig& c) { c.vban = v; });
    // Port/on changes rebind the socket. DEFERRED (PostMessage to the main
    // window, which calls ApplyVbanConfig on the next pump) rather than inline:
    // a TXT-carried `MDXM_VBAN|port=...` must get its reply out on the OLD
    // socket before the rebind tears it down (spec §6.3). Everything else
    // applies inline.
    if (key == L"port" || key == L"on") PostApplyVbanConfig();
    else ApplyVbanConfig();
    return true;
}
```

`OnResume` (the existing `UiContext::onResume` chain): call `ApplyVbanConfig()`
— a socket that died across Modern Standby re-arms the way `RenderRetry`
re-arms a render (spec §8).

   (`ParseBool01W` — small local helper, or reuse the protocol's by exporting it; a local static lambda is fine.)
   - `AppController::Stop()`: `m_vban.Stop();` before `m_engine.Stop();`.
5. `docs/ipc.md`: new section "§ VBAN" documenting `MDXM_VBAN` (every key, clamps, pipe-only `pin`, VBAN-only `frames`/`audio`), `MDXM_VBANSTATE` (every field), `MDXM_AUTH` (pipe behavior + forward-reference to the VBAN-TXT semantics, spec §4), and add `MDXM_VBAN`/`MDXM_AUTH` to the §3 verb table.
6. `tests/test_docs.cpp`: after the existing check, add the reply-record guard:

```cpp
MDXM_TEST_CASE(Docs_VbanReplyRecordsAreDocumented) {
    const std::string root = RepoRoot();
    CHECK(!root.empty());
    if (root.empty()) return;
    const std::string doc = ReadFile(root + "\\docs\\ipc.md");
    // Reply records are invisible to the verb extractor; pinned by name
    // (spec §6.3). MDXM_VBANPEERS joins this list in phase 2.
    for (const char* rec : { "MDXM_VBANSTATE" })
        CHECK(doc.find(rec) != std::string::npos);
}
```

- [ ] **Step 4: Build Test + run; every suite green (test_docs included — write the ipc.md section before running).**
- [ ] **Step 5: Commit** — `feat: MDXM_VBAN verb family and VBAN server app wiring`

---

### Task 9: CLI smoke tools + end-to-end audio test — phase 1 acceptance

**Files:**
- Modify: `src/mdxmixer/main.cpp` (two CLI commands, patterned on `RunFeed`/`RunMonitor` at `main.cpp:165-208`/`474-516`), `tests/test_audio_integration.cpp` (one `Audio_` test), `README.md` (short "VBAN" section), `docs/Changes.md`

**Interfaces:**
- Consumes: `VbanServer`, `vban_protocol` parse.
- Produces: `mdxmixer.exe --vban [seconds]` — connects to the running instance's pipe, polls `MDXM_VBAN` once a second, prints the state record. `mdxmixer.exe --vbanrx <host> [port] [seconds]` — standalone receiver: sends PING0 every 2 s, prints one line per second: packets received, sequence gaps, current depth estimate. (Uses raw winsock in `main.cpp`, same includes as `vban_server.cpp`.)

- [ ] **Step 1: Add the `Audio_` end-to-end test** (needs the real engine + devices, runs only under `--audio`):

```cpp
MDXM_TEST_CASE(Audio_VbanCarriesPersonalMix) {
    // Engine started on real devices (reuse the harness the other Audio_
    // tests in this file use), VBAN sink on, server up on the test port,
    // openSubscribe client: assert >0 audio packets with nonzero samples
    // arrive within 2 s while audio plays, and Status().starved stays low.
}
```

Follow the setup/teardown idiom of the existing `Audio_` cases in `tests/test_audio_integration.cpp` exactly — same device acquisition, same skip-when-no-cable messages.

- [ ] **Step 2: Implement the two CLI commands;** wire them into the arg dispatch at `main.cpp:529-640` beside `--feed`/`--monitor`, and into the usage text.
- [ ] **Step 3: Build Release; manual acceptance checklist (record results in the commit message body):**
  1. `mdxmixer.exe` running with `vban.enabled=true`, `openSubscribe=true` (set via `MDXM_VBAN|on=1|open=1` over the pipe or the JSON).
  2. `mdxmixer.exe --vbanrx 127.0.0.1` shows a packet stream with no gaps.
  3. **VBAN Receptor Lite on the phone** (free, Play Store): add source = PC IP, stream `mdxmixer` — audio plays. This validates against VB-Audio's own implementation (spec §9).
  4. Voicemeeter (optional): VBAN incoming stream from the PC's IP plays.
- [ ] **Step 4: Run `build.ps1 Test` + `mdxmixer_test.exe` (headless) + `mdxmixer_test.exe --audio` (rig).**
- [ ] **Step 5: Commit** — `feat: --vban/--vbanrx CLI and phase-1 end-to-end audio test`

---

# Phase 2 — TXT control + AUTH + UI

### Task 10: TXT codec — UTF-8, packet build/parse, BEGIN…END chunking

**Files:**
- Modify: `src/mdxmixer/net/vban_protocol.h/.cpp`, `tests/test_vban_protocol.cpp`

**Interfaces:**
- Produces:
  - `constexpr uint8_t kTxtUtf8 = 0x10;`
  - `std::string WideToUtf8(const std::wstring&)` / `std::wstring Utf8ToWide(const std::string&, bool* ok)` — pure (hand-rolled UTF-16⇄UTF-8, no WinAPI); `*ok=false` on malformed input
  - `size_t BuildTxtPacket(uint8_t* out, const char name[16], uint32_t nuFrame, const std::string& utf8)` — 0 if `utf8.size() > kMaxData` (a record NEVER spans packets, spec §2.4)
  - `bool ParseTxt(const Parsed& p, std::string* utf8)` — proto/charset check + copy
  - `std::vector<std::wstring> ChunkReplies(const std::vector<std::wstring>& records)` — groups whole records into ≤`kMaxData` UTF-8 payload units (BEGIN…END span packets **at record boundaries only**); a single record too large is replaced by `MDXM_ERR|msg=toolong`

- [ ] **Step 1: Failing tests**

```cpp
MDXM_TEST_CASE(VbanTxt_Utf8RoundTrip) {
    const std::wstring w = L"MDXM_VBAN|gain=250|name=caf\x00E9 \x2713";
    bool ok = false;
    CHECK(Utf8ToWide(WideToUtf8(w), &ok) == w);
    CHECK(ok);
}

MDXM_TEST_CASE(VbanTxt_RejectsOversizeAndBadUtf8) {
    // Review Focus #5.
    uint8_t out[kMaxPacket];
    std::string big(kMaxData + 1, 'x');
    CHECK(BuildTxtPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 0, big) == 0);
    bool ok = true;
    Utf8ToWide(std::string("\xFF\xFE\x80 garbage"), &ok);
    CHECK(!ok);
    std::string okStr = "MDXM_PING";
    size_t n = BuildTxtPacket(out, "mdxmixer\0\0\0\0\0\0\0\0", 9, okStr);
    Parsed p = ParsePacket(out, n);
    CHECK(p.valid && p.proto == kProtoTxt);
    CHECK((p.hdr.format_bit & 0xF0) == kTxtUtf8);
    CHECK(p.hdr.nuFrame == 9);
    std::string back;
    CHECK(ParseTxt(p, &back) && back == okStr);
}

MDXM_TEST_CASE(VbanTxt_ChunkRepliesAtRecordBoundaries) {
    std::vector<std::wstring> recs{ L"MDXM_BEGIN" };
    for (int i = 0; i < 100; ++i)
        recs.push_back(L"MDXM_CHAN|id=ch" + std::to_wstring(i) +
                       L"|name=" + std::wstring(40, L'x'));
    recs.push_back(L"MDXM_END");
    auto chunks = ChunkReplies(recs);
    CHECK(chunks.size() > 1);                         // spans packets
    for (const auto& c : chunks)
        CHECK(WideToUtf8(c).size() <= kMaxData);      // every chunk fits
    // Records are newline-joined inside a chunk and never split across two.
    std::wstring all;
    for (const auto& c : chunks) { if (!all.empty()) all += L"\n"; all += c; }
    size_t count = 1;
    for (wchar_t ch : all) if (ch == L'\n') ++count;
    CHECK(count == recs.size());
    // One record too big for any packet: replaced, not split.
    auto bad = ChunkReplies({ std::wstring(kMaxData + 10, L'z') });
    CHECK(bad.size() == 1 && bad[0] == L"MDXM_ERR|msg=toolong");
}
```

- [ ] **Step 2: Run; fail.**
- [ ] **Step 3: Implement.** The UTF-8 codec is the standard 1–4-byte encoder/decoder with surrogate-pair handling (~60 lines); malformed sequences set `*ok=false` and decode as U+FFFD. `ChunkReplies` greedily packs records joined by `\n` while the UTF-8 length stays ≤ `kMaxData`.
- [ ] **Step 4: Build + run; pass.**
- [ ] **Step 5: Commit** — `feat: VBAN TXT codec with record-boundary chunking`

---

### Task 11: Auth machine + full §4 entitlement matrix (pure)

**Files:**
- Modify: `src/mdxmixer/net/vban_peers.h/.cpp`, `tests/test_vban_peers.cpp`

**Interfaces:**
- Produces:

```cpp
enum class AuthOutcome { Ok, BadPin, Locked, Denied, Pending };
// One decision, pure (spec §4 table). deviceDenied = denied-this-session list.
AuthOutcome DecideAuth(const std::wstring& configuredPin, const std::wstring& requestPin,
                       bool deviceAuthorized, bool deviceDenied, bool ipLocked);

class AuthStrikes {           // per-IP, survives peer expiry and port rotation
public:
    static constexpr int kMaxStrikes = 3;
    static constexpr uint64_t kLockoutMs = 60000;
    static constexpr size_t kMaxIps = 8;
    bool Locked(uint32_t ip, uint64_t nowMs) const;
    void Strike(uint32_t ip, uint64_t nowMs);     // 3rd strike sets lockedUntil
    void Clear(uint32_t ip);                      // a successful auth clears
private:
    struct Row { uint32_t ip; int strikes; uint64_t lockedUntilMs; uint64_t lastMs; };
    std::vector<Row> m_rows;                      // <= kMaxIps, oldest replaced
};
```

- [ ] **Step 1: Failing tests** — the complete matrix, one CHECK per cell:

```cpp
MDXM_TEST_CASE(VbanAuth_Decide) {
    // pin, reqPin, authorized, denied, locked -> outcome
    CHECK(DecideAuth(L"42", L"42", true,  false, false) == AuthOutcome::Ok);
    CHECK(DecideAuth(L"42", L"42", false, false, false) == AuthOutcome::Pending);
    CHECK(DecideAuth(L"42", L"41", false, false, false) == AuthOutcome::BadPin);
    CHECK(DecideAuth(L"42", L"42", false, true,  false) == AuthOutcome::Denied);
    CHECK(DecideAuth(L"42", L"42", true,  false, true)  == AuthOutcome::Locked);
    CHECK(DecideAuth(L"42", L"41", false, false, true)  == AuthOutcome::Locked);
    // Empty configured pin: the caller never reaches DecideAuth (TXT is dead,
    // spec §4) -- but defense in depth: it must NOT read as a match.
    CHECK(DecideAuth(L"",  L"",  false, false, false) == AuthOutcome::BadPin);
}

MDXM_TEST_CASE(VbanAuth_StrikesLockoutAndRotation) {
    AuthStrikes s;
    CHECK(!s.Locked(7, 1000));
    s.Strike(7, 1000); s.Strike(7, 2000);
    CHECK(!s.Locked(7, 3000));
    s.Strike(7, 3000);                       // third strike
    CHECK(s.Locked(7, 3001));
    CHECK(s.Locked(7, 62999));
    CHECK(!s.Locked(7, 63001));              // 60 s after the third strike
    // Port rotation cannot reset: strikes key on IP alone (spec §3.3), which
    // is exactly why this is not a Peer field.
    AuthStrikes s2;
    s2.Strike(9, 0); s2.Strike(9, 1); s2.Strike(9, 2);
    CHECK(s2.Locked(9, 10));
    s2.Clear(9);
    CHECK(!s2.Locked(9, 10));
}

MDXM_TEST_CASE(VbanPeers_AuthDiesWithPeerExpiry) {
    PeerTable t;
    Peer* p = t.Ping(PeerKey{1,1}, 0);
    p->auth = AuthState::Authorized; p->deviceId = L"dev";
    t.Expire(10001);
    CHECK(t.All().empty());                  // auth binds ip:port and dies with it (§4)
    Peer* q = t.Ping(PeerKey{1,1}, 10002);   // re-ping: a NEW unauthenticated peer
    CHECK(q->auth == AuthState::None);
}
```

- [ ] **Step 2: Run; fail. Step 3: Implement** (`DecideAuth` is an ordered if-chain: locked → denied → pin mismatch (empty-vs-empty included) → authorized → pending; `AuthStrikes` a small vector with replace-oldest). **Step 4: pass. Step 5: Commit** — `feat: VBAN auth decision, strike lockout, and matrix tests`

---

### Task 12: `DispatchToUi` extraction + TXT dispatch + AUTH flow on the wire

**Files:**
- Modify: `src/mdxmixer/app/app_controller.h/.cpp` (extract + reuse), `src/mdxmixer/net/vban_server.h/.cpp` (TXT path), `src/mdxmixer/ipc/mixer_control.h` + `protocol.cpp` (`MDXM_VBANPEERS`), `docs/ipc.md`, `tests/test_vban_server.cpp`, `tests/test_docs.cpp` (add `MDXM_VBANPEERS`, `MDXM_AUTHSTATE` to the pinned list)

**Interfaces:**
- Produces:

```cpp
// app_controller.h -- the one marshalling discipline, shared by pipe and VBAN
// (spec §3.3). Extracted verbatim from the lambda at app_controller.cpp:168-196;
// both crashes it encodes (main_window.h:72-93) stay fixed in ONE place.
std::vector<std::wstring> DispatchToUi(HWND hwnd, const std::wstring& msg,
                                       bool* wantSubscribe, int* wantIntervalMs);
```

```cpp
// mixer_control.h additions:
struct VbanPeerRow { std::wstring addr; std::wstring deviceName;
                     bool authed; std::wstring since; bool audioOn, framesOn;
                     bool isAlwaysTarget; };
virtual std::vector<VbanPeerRow> GetVbanPeers() = 0;
```

- VbanServer TXT handling order, per received TXT packet (receive thread):
  1. Stream-name check (`StreamNameIs` vs config) — mismatch: drop + count (spec §2.4).
  2. Pin empty → drop + count, **including `MDXM_AUTH`** (spec §4).
  3. Decode UTF-8 (bad → drop + count). Parse verb prefix (first `|`-token).
  4. `MDXM_AUTH` → fields pin/device/name; `DecideAuth` with the strike table and the session denied-list; outcomes → replies `MDXM_AUTHSTATE|ok=1`, `|ok=0|err=badpin` (+Strike), `|ok=0|err=locked`, `|ok=0|err=denied`, `|pending=1` (+ post the approval request via callback, deduped by deviceId while pending). `Ok` additionally marks the peer `Authorized`, stores deviceId/name, `Clear`s strikes.
  5. `MDXM_VBAN` whose keyed fields are ONLY `audio`/`frames` → apply to the peer, reply with the state record (ask via one marshalled `MDXM_VBAN` query). Mixed records: apply per-peer keys locally, strip them, forward the remainder.
  6. `MDXM_SUBSCRIBE` → reply `MDXM_ERR|msg=subscribe is pipe-only` (spec §2.4).
  7. Anything else from an **Authorized** peer → `cb.dispatch(record)` (the marshalled path) → `ChunkReplies` → one TXT packet per chunk back to the peer with its `txTxtNuFrame++`. From a non-authorized peer → drop + count.
- Callbacks grow: `onAuthPending(deviceId, name)`, `onAuthorized(deviceId, name)`; plus `void VbanServer::AuthorizationResult(const std::wstring& deviceId, bool allow)` (control thread: flips the pending peer to Authorized and replies on its next AUTH resend, or adds to the denied list).
- AppController: pipe handler lambda body replaced with a call to `DispatchToUi`; `m_vban` callbacks wired — `dispatch` binds `DispatchToUi(hwnd, msg, nullptr, nullptr)`, `onAuthPending` posts `MainWindow::kVbanAuthMsg` (Task 13), `onAuthorized` mutates config (`authorizedDevices` upsert with `lastSeen` = today) on the UI thread via `PostMessage` + handler.
- `GetVbanPeers()` → snapshot from `m_vban` (+ synthetic `always` row when a target is configured, spec §4). `MDXM_VBANPEERS` handler in protocol.cpp: `MDXM_BEGIN`, one `MDXM_VBANPEER|addr=…|device=…|authed=…|since=…|audio=…|frames=…|always=…` per row, `MDXM_END`.

- [ ] **Step 1: Failing loopback tests** (extend `test_vban_server.cpp`; helper `SendTxt(sock, utf8)` mirrors `SendPing0`):

```cpp
MDXM_TEST_CASE(VbanTxt_AuthFlowOverTheWire) {
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    std::vector<std::wstring> pendings;
    VbanServer srv; std::wstring err;
    VbanServer::Callbacks cb;
    cb.dispatch = [](const std::wstring& m) {
        return std::vector<std::wstring>{ L"MDXM_PONG|version=1" };  // fake UI
    };
    cb.onAuthPending = [&](const std::wstring& id, const std::wstring&) {
        pendings.push_back(id);
    };
    CHECK(srv.Start(cfg, &ring, 48000, cb, &err));
    int cport; SOCKET c = MakeClient(&cport);
    SendPing0(c, 1);
    RecvSkipUntil(c, kProtoService);            // swallow the ping reply
    // Bad pin -> badpin; three of them -> locked.
    SendTxt(c, "MDXM_AUTH|pin=9|device=d1|name=P");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=0|err=badpin");
    SendTxt(c, "MDXM_AUTH|pin=9|device=d1|name=P");
    SendTxt(c, "MDXM_AUTH|pin=9|device=d1|name=P");
    RecvTxt(c); RecvTxt(c);
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=P");   // right pin, but locked now
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=0|err=locked");
    srv.Stop();
}

MDXM_TEST_CASE(VbanTxt_PendingThenApprovedThenDispatch) {
    RingBuffer ring(48000 * 2);
    VbanConfig cfg; cfg.enabled = true; cfg.port = kTestPort; cfg.pin = L"42";
    std::vector<std::wstring> pendings;
    VbanServer srv; std::wstring err;
    VbanServer::Callbacks cb;
    cb.dispatch = [](const std::wstring& m) {
        return std::vector<std::wstring>{ L"MDXM_PONG|version=1" };
    };
    cb.onAuthPending = [&](const std::wstring& id, const std::wstring&) {
        pendings.push_back(id);
    };
    CHECK(srv.Start(cfg, &ring, 48000, cb, &err));
    int cport; SOCKET c = MakeClient(&cport);
    SendPing0(c, 1);
    RecvSkipUntil(c, kProtoService);
    // An un-authed peer's ordinary verb is dropped, never dispatched.
    SendTxt(c, "MDXM_PING");
    CHECK(RecvTxtTimeout(c).empty());                       // nothing comes back
    // Good pin + unknown device -> pending; the resend does NOT re-prompt.
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=Pixel");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|pending=1");
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=Pixel");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|pending=1");
    CHECK(pendings.size() == 1);
    srv.AuthorizationResult(L"d1", true);
    SendTxt(c, "MDXM_AUTH|pin=42|device=d1|name=Pixel");
    CHECK(RecvTxt(c) == "MDXM_AUTHSTATE|ok=1");
    // Authorized: dispatch works, SUBSCRIBE is refused, audio=0 opts out.
    SendTxt(c, "MDXM_PING");
    CHECK(RecvTxt(c) == "MDXM_PONG|version=1");
    SendTxt(c, "MDXM_SUBSCRIBE|1");
    CHECK(RecvTxt(c) == "MDXM_ERR|msg=subscribe is pipe-only");
    CHECK(srv.Status().emitting);                           // authorized + audioOn
    SendTxt(c, "MDXM_VBAN|audio=0");
    RecvTxt(c);                                             // state echo
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!srv.Status().emitting);                          // control-only session
    closesocket(c);
    srv.Stop();
}
```

(`RecvTxtTimeout` = `RecvTxt` with a short 300 ms `SO_RCVTIMEO` returning empty
on timeout — add it beside the other helpers.)

```cpp

MDXM_TEST_CASE(VbanTxt_EmptyPinDropsEverything) {
    // cfg.pin empty, openSubscribe=true: PING0 reply + audio still flow, but
    // every TXT -- MDXM_AUTH included -- is dropped silently (spec §4).
}

MDXM_TEST_CASE(VbanTxt_WrongStreamNameDropped) {
    // TXT with streamname "other" never reaches dispatch; counted.
}
```

(Write `RecvTxt`/`RecvSkipUntil`/`SendTxt` helpers in the test file with the obvious recvfrom/ParsePacket/ParseTxt bodies — ~20 lines.)

- [ ] **Step 2: Run; fail. Step 3: Implement** per the numbered order above; `DispatchToUi` extraction is a pure cut-paste of the lambda body into a free function (keep the comment block with it). **Step 4: full suite green. Step 5: Commit** — `feat: MDXM IPC over VBAN-TXT with mirrored MDropDX12 auth`

---

### Task 13: Approval prompt, device persistence, revoke

**Files:**
- Modify: `src/mdxmixer/ui/main_window.h/.cpp`, `src/mdxmixer/app/app_controller.h/.cpp`, `src/mdxmixer/ipc/mixer_control.h` (`virtual bool RevokeVbanDevice(const std::wstring& deviceId) = 0;`)

- [ ] **Step 1:** `main_window.h`: `static constexpr UINT kVbanAuthMsg = WM_APP + 14;   // lParam = heap wchar "deviceId\nname", owned by the handler`.
- [ ] **Step 2:** `main_window.cpp` handler: split id/name; `TaskDialog` (topmost, `TDCBF_YES_BUTTON | TDCBF_NO_BUTTON`, title "VBAN access request", main text `"<name>" (MilkRemote) requests VBAN access.`) — a TaskDialog pumps messages, so timers keep running; when the main window is hidden, first `Shell_NotifyIcon` balloon via `TrayIcon` (add a `Balloon(title, text)` method — `NIF_INFO`), and show the dialog regardless. Yes → `ctx->vbanAuthResult(deviceId, true)`; No → `(…, false)`. Add `std::function<void(const std::wstring&, bool)> vbanAuthResult;` to `UiContext`.
- [ ] **Step 3:** AppController wires `vbanAuthResult` → `m_vban.AuthorizationResult(id, allow)`; on allow also `m_store.Mutate` upserting `authorizedDevices` (and pushes the refreshed authorized-id list into the server via `UpdateConfig`). `RevokeVbanDevice` removes from config + `UpdateConfig` (the live peer, if any, drops to `AuthState::None` on the server's next config application).
- [ ] **Step 4:** Manual test: phone-side not built yet — use `--vbanrx` extended? No: use a 20-line throwaway PowerShell/`--vbanrx --auth pin` flag on the CLI receiver to send AUTH (add `--auth <pin>` to `--vbanrx` while in here; it is a dev tool). Verify: prompt appears, Allow persists in `mdxmixer.json`, second AUTH instant, Revoke in the tab (Task 14) returns it to pending.
- [ ] **Step 5: Commit** — `feat: VBAN device approval prompt with persisted authorizations`

---

### Task 14: The VBAN tab — phase 2 acceptance

**Files:**
- Create: `src/mdxmixer/ui/tab_vban.cpp` (model: `tab_eq.cpp` — private class `L"mdxmixerTabVban"`, heap state struct in `GWLP_USERDATA`, whole proc try/catch, `ThemeCtlColor`/`ThemeEraseBkgnd`)
- Modify: `src/mdxmixer/ui/main_window.h` (`kPageCount` 5→6 **and the enumerating comment** at :155), `src/mdxmixer/ui/main_window.cpp` (names array :137 → `{ L"Mixer", L"Routing", L"EQ", L"Devices", L"VBAN", L"Options" }` — VBAN before Options so Options stays last; forward decl at :28-31; `m_pages[4] = CreateVbanTab(...)` + shift Options to `[5]`), `src/mdxmixer/app/app_controller.cpp` (`kTabs[]` at :1295 gains `L"vban"` in the same position, **and the comment** at :1288-1289), `src/mdxmixer/mdxmixer.vcxproj`, `docs/ipc.md` (**both** tab lists: the table row ~:304 and the `MDXM_TAB` section ~:397-403; also `MDXM_TAB`'s error string in `protocol.cpp` gains `vban`)

Contents (spec §6.2) — all controls drive `ctl->SetVbanOption` + read `ctl->GetVbanStatus()` on `kRefreshMsg` (250 ms):
- Row 1: `Enable` checkbox (`on`), `Port` edit, `Stream name` edit, **`PIN` edit with `ES_PASSWORD`**; a static badge `PIN unset — control disabled` shown when pin empty && authorizedDevices non-empty (spec §4).
- Row 2: `Source` combo (personal/streaming), `Format` combo (i16/f32), `Gain` slider 0–6400 (`TBM_*` trackbar, label shows `%d%%`), `Frames fps` edit.
- Row 3: `openSubscribe` / `alwaysStream` / `alwaysFrames` checkboxes + `Target` edit.
- Status strip (static, refreshed): `peers=N emitting=0|1 depth=NNms behind=NNms sent=N starved=N dropped=N frames=N [error]`.
- Authorized devices listbox + `Revoke` button → `ctl->RevokeVbanDevice`.
- Note: `ui.activeTab` clamp already handles old configs (`main_window.cpp:163`); nothing else persists tab indices. Inserting VBAN at index 4 means a config saved on the Options tab (4) reopens on VBAN once — cosmetic, self-healing on the next user tab switch; accepted for keeping Options last.

- [ ] Steps: build the tab; manual pass over every control (set → `MDXM_VBAN` query over pipe shows the new value; restart → persisted); `MDXM_TAB|vban` works; `MDXM_CAPTURE` of main window on the VBAN tab renders. Run full headless suite (test_docs now sees the new tab lists). Commit — `feat: VBAN tab`.

---

# Phase 3 — FRAME screen captures

### Task 15: FRAME chunker (pure)

**Files:** `src/mdxmixer/net/vban_protocol.h/.cpp`, `tests/test_vban_protocol.cpp`

**Interfaces:**
- `constexpr uint8_t kFrameStart = 0x01, kFrameContinue = 0x02, kFrameEnd = 0x04;`
- `void ChunkFrame(std::vector<std::vector<uint8_t>>& out, const char name[16], uint32_t nuFrame, const uint8_t* bytes, size_t len)` — ≤1436-byte chunks; 16-bit packet index LSB in `format_nbs` / MSB in `format_nbc` **wrapping at 65536** (the spec's own LSB/MSB split code; its prose "64535/64536" is a digit-transposition typo — keep 65536); `format_bit` = start/continue/end in bits 0–2, frame type 0 in bits 4–7; a single-packet frame sets `start|end = 0x05` (spec is silent; inference from the bit-flag layout, pinned here and in the VBAN-Screen interop check); `nuFrame` identical on every packet of one frame.

- [ ] **Step 1: Failing tests** — three cases: (a) an 8000-byte buffer → 6 packets, first `0x01`, middle `0x02`, last `0x04`, indices 0..5, payload reassembles byte-identical; (b) a 100-byte buffer → 1 packet with `format_bit == 0x05`; (c) a synthetic `len = 1436 * 65540` is NOT built (cap: return early, out empty — frames that big are a bug upstream) **and** index arithmetic: chunk a buffer yielding exactly 65537 packets? — too big to allocate; instead expose the index math as `inline uint16_t FramePacketIndex(size_t i) { return (uint16_t)(i % 65536); }` and CHECK `FramePacketIndex(65536) == 0`, `FramePacketIndex(65537) == 1`.
- [ ] Steps 2–5: run-fail, implement, run-pass, commit — `feat: VBAN FRAME chunker`.

---

### Task 16: Display capture + frame streaming — phase 3 acceptance

**Files:**
- Create: `src/mdxmixer/net/display_capture.h/.cpp`
- Modify: `src/mdxmixer/net/vban_server.h/.cpp` (capture thread), `src/mdxmixer/app/app_controller.cpp` (plumb `frames` config), `docs/Changes.md`, `README.md`

**Interfaces:**

```cpp
// net/display_capture.h  (Windows-specific; D3D11 + DXGI + WIC)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mdxm {

struct CapturedFrame {
    int deviceNumber = 0;             // the n in \\.\DISPLAYn -- NOT an index
    std::vector<uint8_t> jpeg;
};

class DisplayCapture {
public:
    ~DisplayCapture();
    // Enumerates DXGI outputs, creates one duplication per output whose
    // GDI device name parses as \\.\DISPLAYn with 1 <= n <= 8 (spec §2.5:
    // out-of-range device numbers are skipped and counted).
    bool Init(std::wstring* err);
    void Shutdown();
    // Non-blocking-ish: AcquireNextFrame(0) per output; a changed output is
    // downscaled on the GPU (GenerateMips, largest mip with long edge <=
    // maxEdge), read back, WIC-JPEG-encoded to memory at `quality`.
    // Returns only outputs that changed since the last call.
    std::vector<CapturedFrame> Poll(int maxEdge, int quality);
    // DXGI_ERROR_ACCESS_LOST anywhere -> false from Poll's internals; the
    // caller Shutdown()s and re-Init()s with RenderRetry-style backoff.
    bool Lost() const;
    uint64_t SkippedOutputs() const;  // deviceNumber 0 or > 8
};

} // namespace mdxm
```

Implementation notes (follow exactly):
- `#pragma comment(lib, "d3d11.lib")`, `"dxgi.lib"`, `"windowscodecs.lib"` in the .cpp. `D3D11CreateDevice` on the default adapter; per output: `IDXGIOutput1::DuplicateOutput`.
- Device number from `DXGI_OUTPUT_DESC::DeviceName` (`\\.\DISPLAYn`) — `_wtoi` past the prefix; 0/`>8` → skip + count (spec §2.5 / the `deviceNumberOf` lesson from MDR_Android: device numbers are not enumeration indices).
- Downscale: copy the acquired frame into a `D3D11_BIND_RENDER_TARGET|SHADER_RESOURCE` texture created with full mip chain + `D3D11_RESOURCE_MISC_GENERATE_MIPS`, `GenerateMips`, `CopySubresourceRegion` of the chosen mip into a tiny staging texture, `Map`, feed WIC (`IWICImagingFactory` → `CreateBitmapFromMemory` BGRA → `IWICBitmapEncoder` JPEG on a `CreateStreamOnHGlobal`/`SHCreateMemStream` memory stream with `quality` via the encoder's property bag).
- The mip trick is the point: a 4K frame reads back hundreds of KB, not the 33 MB full-res copy `canvas_metric.h` (MDropDX12) documents as the mistake.
- VbanServer capture thread: created only while `AnyFramesEntitled()` (a peer with `framesOn`, or `alwaysFrames && target`); wakes at `1/fps`; `Poll` → for each frame `ChunkFrame` on stream name `"VIDEO" + std::to_string(deviceNumber)` with that display's own `nuFrame++` → `sendto` every frames-entitled recipient; cache the last JPEG per display and send it immediately when a peer becomes frames-entitled (spec §5); `framesSent` counter. `Lost()` → Shutdown + re-Init with the `RenderRetry` backoff shape; audio is unaffected.

- [ ] **Step 1:** No headless GPU test — gate one integration test under the `Audio_` prefix (`Audio_VbanFrameCaptureProducesJpeg`: Init, Poll until a frame arrives (move the mouse), CHECK jpeg starts `FF D8` and ends `FF D9`, deviceNumber ≥ 1). Headless tests cover the chunker (Task 15) and entitlement (Task 6/11).
- [ ] **Step 2:** Implement; build; run `--audio` suite on the rig.
- [ ] **Step 3: Manual acceptance (record in commit body):** `alwaysFrames=1`, `target=<laptop-ip>:6980`, **VBAN-Screen** on that machine shows the displays updating at ~2 fps, including after a deliberately near-black screen (the single-packet 0x05 frame). `MDXM_VBANSTATE` shows `framessent` climbing and `fps` honored at 0.2 and 10.
- [ ] **Step 4:** Changelog + README (one paragraph: VBAN audio + screen streams, Receptor Lite / VBAN-Screen interop). Firewall (spec §6.4): add a WiX `<fire:FirewallException>` element to `dist/mdxmixer.wxs` — **program-scoped** (`Program="[#mdxmixer.exe]"`, `Protocol="udp"`, no port attribute) so a user-edited port never strands a stale port rule; README documents the portable equivalent: `netsh advfirewall firewall add rule name="mdxmixer VBAN" dir=in action=allow program="<path>\mdxmixer.exe" protocol=udp`.
- [ ] **Step 5: Commit** — `feat: display capture streamed as VBAN FRAME (VIDEOn)`

---

## Post-plan

- Phases 4–5 (MDR_Android receiver + previews) are a separate plan in that repository, written against the same spec §7.
- Deferred by the spec, do not implement here: timer-paced fallback clock (§3.2), `MDXM_SUBSCRIBE` push over TXT and the broadcast fan-out seam (§2.4), hotkey action for the listener, Cast output (§1 note), phone-mic return (§3.3).
