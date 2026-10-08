# VBAN Stream Server — Design

Date: 2026-10-07
Status: approved design, adversarially verified against the codebases and the
VBAN rev 13 spec; pre-implementation
Scope: mdxmixer (server) + MDR_Android (receiver). One spec covers the protocol
contract and both ends; each repo gets its own implementation plan.

## 1. Context and goal

mdxmixer renders the **personal mix** (what Shane hears) to a physical output
device and the **streaming mix** to OBS/MDropDX12. Headphones today are wired or
Bluetooth devices attached to the PC. The goal is to make the phone (MDR_Android /
MilkRemote) a first-class destination for the personal monitor mix, so moving the
headset from the PC to the phone (a ~3-second replug for a wired headset, or just
walking away with BT buds paired to the phone) continues the same listening
session — plus low-res screen captures of the displays so the phone can show what
the visualizer is putting on each monitor.

Decisions made during brainstorming, recorded as requirements:

- **Protocol: VBAN, used compliantly** (official spec rev 13, SEP 2025). No
  custom framing where the spec provides a mechanism. Any standard VBAN tool on
  the LAN should be able to discover and (when permitted) play the stream.
- **On-demand only.** mdxmixer emits nothing until a subscriber asks, and stops
  when the last subscriber goes away. Subscription rides VBAN SERVICE/PING0.
- **The stream has its own volume**, applied PC-side in mdxmixer, settable
  remotely over the wire (`MDXM_VBAN|gain=`).
- **The VBAN association also carries MDXM IPC**: the MDXM verb surface is
  reachable over VBAN-TXT after authentication (one stated exception:
  `MDXM_SUBSCRIBE`, §2.4), so the phone can drive mdxmixer without MDropDX12
  running.
- **Auth mirrors MDropDX12**: PIN + per-device authorization with an approval
  prompt on the PC; authorized devices are remembered.
- **Latency: expect ~200 ms end-to-end on this Wi-Fi**, even with wired
  headphones on the phone (owner's measured expectation of the network).
  Sub-100 ms is a best case on a quiet network with the low-latency profile,
  not a target the design engineers against. The phone's Bluetooth output adds
  its own 100–200 ms on top. Priorities, in order: **stability, quality,
  latency**. Known consequence: ears trail the beat-reactive visuals by
  roughly a fifth of a second; nothing upstream can fix that.
- **Lag transparency:** subscribers get a live end-to-end latency estimate
  (§7.5) so the lag can be dialed into other software — e.g. a movie player's
  audio-delay, so lips sync on screen while the audio rides the phone.
- **Licensing:** clean-room implementation from the published spec (explicitly
  invited by VB-Audio). No code copied from GPL implementations
  (quiniouben/vban, obs-vban, pyVBAN). MIT references (aiovban, thib3113/vban)
  may be consulted for semantics. No external libraries, per house rule.

**Supersedes a v1 non-goal.** The 2026-09-22 design listed "no network audio"
among v1 non-goals. That was an effort-budget call at the time — network audio
looked too hard to take on — not a judgment that it didn't belong. This spec
reverses it deliberately; the other v1 non-goals (no ASIO, no exclusive mode)
stand.

Noted for future work, out of scope here: **Cast output** to Google Home
speakers (speaker pulls a chunked LPCM/WAV HTTP stream directly from mdxmixer;
phone is only the Cast remote; seconds of latency by design — ambient listening,
not monitoring). It would tee off the same post-gain block this spec creates.

## 2. Protocol usage (VBAN rev 13)

Everything below is from the official spec PDF
(`https://vb-audio.com/Voicemeeter/VBANProtocol_Specifications.pdf`, rev 13,
SEP 2025). One UDP port carries all sub-protocols; packets are distinguished by
header bits and stream name.

### 2.1 Header (28 bytes, little-endian, packed)

```c
#pragma pack(push, 1)
struct VbanHeader {
    uint32_t vban;          // 'V','B','A','N'
    uint8_t  format_SR;     // bits 5-7: sub-protocol. bits 0-4:
                            //   audio: SR index; txt: informational bps index;
                            //   frame: informational Mbps index;
                            //   service: MUST be 0 (format_SR == 0x60 exactly,
                            //   spec p.27)
    uint8_t  format_nbs;    // audio: samples-per-packet - 1 (1..256)
                            // service: function (bit 7 = reply)
                            // frame: packet index LSB
    uint8_t  format_nbc;    // audio: channels - 1
                            // service: service type
                            // frame: packet index MSB
    uint8_t  format_bit;    // audio: bits 0-2 data type, bit 3 reserved 0,
                            //   bits 4-7 codec (PCM = 0x00)
                            // txt: bits 4-7 charset (UTF-8 = 0x10)
                            // frame: bits 0-2 packet type (0x01 start /
                            //   0x02 continue / 0x04 end), bits 4-7 frame
                            //   stream type = 0 (spec p.24)
    char     streamname[16];// ASCII, NUL-padded; receiver matches name + source IP
    uint32_t nuFrame;       // growing counter (see per-sub-protocol semantics)
};
#pragma pack(pop)           // sizeof == 28
```

Constants: sub-protocols AUDIO `0x00`, SERIAL `0x20`, TXT `0x40`, SERVICE
`0x60`, FRAME `0x80`, USER `0xE0`. Max data per packet **1436 bytes** (total
1464). Max 256 samples and 256 channels per audio packet. Default UDP port
**6980**. Sample-rate table has 21 entries in three geometric families —
**48000 Hz = index 3**, 44100 = 16. Data types: 0=u8, 1=int16, 2=int24,
3=int32, 4=float32, 5=float64. Codec: PCM = `0x00` (the only one used; VB's
VBCA/VBCV codecs are not freely licensed).

### 2.2 AUDIO (out)

Interleaved PCM, WAV-order, stereo, at the live mix rate (48 kHz steady state).

- **int16 (default):** 256 samples/packet = 1024 B payload, 5.33 ms of audio,
  ~188 packets/s, ~1.6 Mbps. Maximum interop (Receptor Lite, Voicemeeter).
- **float32 (config):** 1436 B caps packets at 179 samples; use 128
  samples/packet (1024 B, 2.67 ms), ~3.1 Mbps.

`nuFrame` increments per packet. The sequence is **shared by all recipients
and monotonic for the process lifetime** — it never resets when emission stops
and restarts. Receivers use it to detect loss/reorder (rules in §8); there is
no retransmission — a lost packet is a lost 5 ms. Stream name default
`mdxmixer` (config `vban.streamName`).

### 2.3 SERVICE (in/out) — discovery, subscription, keepalive

PING0 = service type 0 (IDENTIFICATION), function 0 in `format_nbs`, reply sets
bit 7 (`0x80`); `nuFrame` is a transaction id echoed in the reply. PING0
requests are accepted regardless of stream name; the identification reply
echoes the request's stream name. The reply payload is the spec's 676-byte
identification struct; mdxmixer fills:

- `bitType` = TRANSMITTER (0x2) | VIRTUALMIXER (0x20)
- `bitfeature` = AUDIO (0x1) | TXT (0x10000) | FRAME (0x1000)
- `PreferedRate` = current mix rate; `MinRate`/`MaxRate` = 44100/192000
- `nVersion` = `(MDXM_VERSION_MAJOR << 16) | (MDXM_VERSION_MINOR << 8) |
  MDXM_VERSION_PATCH` — computed from `version.h`'s three numeric macros, the
  only version source (no literals, per version.h's own warning)
- `DeviceName` = "mdxmixer"; `ApplicationName` = "mdxmixer"; `HostName` =
  computer name; color and user fields best-effort.

**Subscription semantics (our layer, wire-compliant):** any PING0 request
creates or renews that `ip:port` peer. The phone pings every ~2 s; a peer
expires after **10 s** without a ping. Audio flows only to peers entitled to it
(§4 matrix). When the last audio-entitled peer expires, emission stops —
unless `alwaysStream` keeps it running (§4). The PING0 *reply* is also the
capability negotiation — the phone reads `bitfeature`, needing no MDropDX12
`GET_VERSION` coupling. PING0 doubles as the **latency probe**: the phone
measures request→reply RTT via the echoed transaction id and uses RTT/2 as
the network term of the lag estimate (§7.5).

### 2.4 TXT (in/out) — the MDXM transport

VBAN-TXT is what Voicemeeter itself uses for remote command strings; here it
carries MDXM protocol records verbatim (the same `VERB|field=value|…` grammar
as the named pipe). `format_bit` charset = UTF-8 (`0x10`); records convert
UTF-16 ↔ UTF-8 at this boundary exactly as MDropDX12's TCP server does.

**Stream name rule (wire contract):** TXT packets in both directions carry
stream name `vban.streamName` (default `mdxmixer`); inbound TXT with any other
name is dropped and counted.

**Packetization rules:** a single record never spans packets, in either
direction. Multi-record replies (`MDXM_BEGIN`…`MDXM_END`) span consecutive
packets at record boundaries only. Requests are single-packet: the phone's
command builder must produce record sets ≤1436 UTF-8 bytes (truncating
free-text fields to fit). The server treats each inbound TXT packet as a
complete record set — no cross-packet request reassembly.

**Loss semantics:** each direction keeps its own incrementing `nuFrame`. The
MDXM surface is idempotent request→reply (queries return snapshots, sets carry
absolute values), so recovery is client-side and simple: a `nuFrame` gap
observed while assembling a BEGIN…END reply discards the partial and re-issues
the query; a set with no state echo is re-sent.

**Push is the one v1 exception to "full surface":** `MDXM_SUBSCRIBE` over TXT
is rejected with `MDXM_ERR|msg=subscribe is pipe-only`. Reason: every
broadcast call site is hard-wired to the pipe object (`m_pipe.Broadcast(...)`
in `AppController::BroadcastState`; the per-subscriber rate bookkeeping lives
inside `PipeServer`), and `IpcRequest` carries no transport identity — the
push direction has no seam today. The future shape, when wanted, is a small
broadcast interface that both `PipeServer` and `VbanServer` implement and
that `BroadcastState`/the peak tick iterate. The phone does not need peak push
in v1: the stream card polls `MDXM_VBANSTATE`, and the Mixer tab keeps its
TCP path.

### 2.5 FRAME (out) — screen captures

The rev-13 image sub-protocol: one complete JPEG per VBAN frame, chunked into
≤1436-byte packets. 16-bit packet index split LSB/MSB across
`format_nbs`/`format_nbc`, wrapping at 65536 (spec p.24 prose says
64535/64536 — a digit-transposition typo; the spec's own LSB/MSB split code
wraps at 65536). `format_bit` low bits: `0x01` start, `0x02` continue, `0x04`
end. The spec is silent on the one-packet case; **we set start|end = 0x05 as
an inference from the bit-flag layout**, and the interop checklist and unit
vectors cover it explicitly. `nuFrame` increments once per completed frame. A
missing packet discards that frame only — the next capture replaces it; the
JPEG decode is the integrity check.

**Stream naming:** `VIDEO<n>` where **n is the Windows display device number**
(the `n` in `\\.\DISPLAYn`) — *not* the enumeration index; device numbers are
neither contiguous nor bounded by the monitor count on a machine whose panels
have been replugged. Streams are capped at device numbers 1–8; a display whose
device number is 0 (unparsable) or >8 is skipped and counted. The `VIDEOn`
shape deliberately matches the VB-Audio convention so VBAN-Screen can consume
the streams.

## 3. mdxmixer architecture

New module `src/mdxmixer/net/`:

| file | contents | Windows-free? |
| --- | --- | --- |
| `net/vban_protocol.{h,cpp}` | header build/parse, SR table, audio packetizer, FRAME chunker, TXT chunker, PING0 structs | **yes — pure, headless-tested** |
| `net/vban_peers.{h,cpp}` | peer/auth/entitlement state machine over opaque addr keys + tick ms | **yes — pure, headless-tested** |
| `net/vban_server.{h,cpp}` | winsock socket, receive + sender threads, marshalling glue | no (`#pragma comment(lib, "ws2_32.lib")`) |
| `net/display_capture.{h,cpp}` | DXGI duplication, mip downscale, WIC JPEG | no |

Dependency direction holds: `net/vban_protocol` and `net/vban_peers` know
nothing of Windows; `vban_server` depends on them plus the engine seam. Every
new `.cpp` (sources *and* tests) gets its own `ClCompile` entry in
`src/mdxmixer/mdxmixer.vcxproj` — there is no glob; a test file missing from
the project silently never runs.

### 3.1 Engine tap

`Engine::MixPull` (`engine.cpp:384-465`) ends each block holding both
post-limiter sums. Beside the existing `m_feed.Write(m_sSum, …)` at line 461,
when the VBAN sink is live the selected source is written to a dedicated SPSC
ring:

- `m_vbanRing` — 2 s capacity at mix rate, stereo interleaved float32,
  drop-oldest on overflow (counted). The 2 s exists to absorb the personal
  render's burst writes (`kMaxPullFrames` reality), not as a latency budget —
  see the pacing rules in §3.2 for why steady-state backlog cannot
  accumulate. Audio-thread work is the ring write only: no gain, no
  conversion, no syscalls, no locks, no allocation.
- **Source** = config enum: `personal` (`m_pSum`, default — the monitor clone,
  per-channel personal gains and mutes included) or `streaming` (`m_sSum`).
- **Level note (measured):** the personal sum runs 20–40 dB below full scale
  because the personal faders are the listening level
  (`stream_feed.h:4-8`, `docs/ipc.md:245-252`). The PC-side stream gain (§3.2)
  provides makeup *before* int16 quantization, so the wire signal is healthy.
- **Demand model — deliberately reporting-only.** `MixDemand` gains a `vban`
  field that `AnyoneListening()` does **NOT** consider. Rationale: when a
  personal render exists, `personalRender` already keeps captures alive and
  the bit is redundant; when no personal render exists, `MixPull` never runs,
  so a demand bit that started captures would fill rings nobody drains — the
  exact fj#10 failure (41.4 M dropped frames) recorded in `mix_demand.h`'s
  header comment. The field exists for diagnostics only. Its value — call it
  `vbanWanted` — is defined as:
  `vban.enabled AND (≥1 audio-entitled unexpired peer OR (alwaysStream AND
  alwaysStreamTarget non-empty))`, independent of ring starvation (demand is
  the want, not the supply) and independent of `vban.source` (MixPull
  produces both sums regardless). `vbanWanted` is what gates the sender
  thread's emission. If the future timer-paced clock lands, `vban` becomes a
  real `AnyoneListening` term at that point and not before.
- On `ReconfigureForMixRate` the ring is flushed (`RingBuffer::Clear()`) and
  the sender derives a new SR index; VBAN receivers read the format from every
  packet header, so a rate change is self-announcing. The lifecycle close
  protocol copies `StreamFeed`'s single-writer discipline (atomic pointer +
  in-flight counter, `stream_feed.h:101-115`) so teardown can never unmap
  under an in-flight write.

### 3.2 Sender thread

Own `_beginthreadex` thread, normal priority (not Pro Audio — it talks to the
network, not a DAC). Loop:

1. Drain `m_vbanRing` into packet-sized blocks (256 frames int16 / 128 float32).
2. Apply gain (atomic float, percent 0–6400, default 100) folded into the
   float→wire conversion, then a `SoftLimiter` instance to catch post-gain
   clipping.
3. **Pace emission against a QPC-anchored send schedule** with a bounded
   catch-up burst (a few packets per wake). Nuance the implementation must
   respect: ring depth measures *producer lead* (a Bluetooth personal render
   writes seconds ahead of what its own buffers play), not phone latency — so
   the pacer must never "skip ahead" to shed depth. What it must prevent is
   falling behind its own schedule: when behind (a `sendto` stall, a scheduler
   hiccup), it catches up by sending faster, bounded per wake; sustained
   inability to catch up rides the ring's drop-oldest, which caps the backlog
   at capacity and counts the loss. `depthMs` and `behindMs` are reported in
   `MDXM_VBANSTATE` so none of this is invisible.
4. `sendto` each packet to every audio-entitled recipient; one shared,
   process-lifetime `nuFrame` sequence (§2.2).
5. **On ring starvation** (personal render stalled or absent) keep the cadence
   with silence packets and count starvation — "zeros on underflow, count
   both". The association stays warm and the phone distinguishes "no signal"
   (pings still answered) from "network dead" (nothing answered).

**Known v1 limit — the clock.** `MixPull` is driven by the personal render
callback (`engine.cpp:470-472`); with *no* personal render device at all there
is no mix and the stream carries silence. The failover allowlist covers the
headset-unplug case on this rig. A timer-paced fallback clock that drives
`MixPull` when no personal device exists is future work, not v1.

### 3.3 VbanServer and the receive thread

The UDP socket binds `vban.bindAddress:vban.port` (default any:6980) whenever
`vban.enabled` is true — the **listener** persists (unlike `MDXM_FEED`),
because the phone must be able to subscribe at any moment; **emission** remains
gated by `vbanWanted` (§3.1), honoring the no-subscribers-no-stream rule.
`WSAStartup` on first enable, `WSACleanup` at exit. Socket errors retry with
the `RenderRetry` backoff shape; both threads follow the no-crash rule (SEH +
catch-all at entry).

Receive thread: blocking `recvfrom`; parse header; switch on sub-protocol:

- SERVICE/PING0 request → peer create/renew + identification reply.
- TXT → stream-name check (§2.4), entitlement gate (§4), then the record is
  marshalled to the UI thread. **Prerequisite refactor:** the ref-counted
  `IpcRequest` + `SendMessageTimeoutW(kIpcMsg)` discipline currently lives in
  an anonymous lambda inside `AppController::Run` (`app_controller.cpp:168-196`)
  — it is extracted into a shared named function (e.g.
  `DispatchToUi(HWND, const std::wstring&, …)`) that both `PipeServer::Start`
  and `VbanServer` call, so the two crashes that forced that discipline
  (`main_window.h:72-93`) stay fixed in exactly one place. Reply records come
  back to the receive thread, are chunked into TXT packets, and `sendto` to
  the asking peer. Busy/timeout behaves exactly like the pipe
  (`MDXM_ERR|msg=busy`).
- Inbound AUDIO/SERIAL: ignored in v1 (phone-mic return is future work).

**Peer table:** fixed 8 entries, no hot-path allocation:
`{addr, port, lastPingTick, authState, deviceId, audioOn, framesOn, rxNuFrame, txNuFrame}`.
Table-full policy: a PING0 from an unknown address still gets its
identification reply (the reply is stateless) but creates no entry unless one
can be evicted — eviction order: expired first, then oldest unauthenticated;
an authenticated entry is never evicted for an unauthenticated pinger.

**AUTH-strike state lives in a separate fixed 8-entry per-IP table**
`{ip, strikes, lockedUntilTick}`, independent of peer-entry lifetime — a
peer entry expires in 10 s, long before a 60 s lockout would, and strikes must
survive source-port rotation.

## 4. Subscription, auth, and entitlement

Mirrors MDropDX12's two-layer model so the phone reuses one mental model and
one identity (device id + device name, §7.2).

`MDXM_AUTH|pin=…|device=…|name=…` over TXT (keyed fields, §6.3):

| case | reply | effect |
| --- | --- | --- |
| bad PIN | `MDXM_AUTHSTATE\|ok=0\|err=badpin` | strike recorded; 3 strikes → 60 s lockout for that IP |
| locked out | `MDXM_AUTHSTATE\|ok=0\|err=locked` | until `lockedUntilTick` |
| previously denied | `MDXM_AUTHSTATE\|ok=0\|err=denied` | until mdxmixer restarts |
| good PIN, unknown device | `MDXM_AUTHSTATE\|pending=1` | approval prompt on the PC (UI thread, topmost TaskDialog + tray balloon when hidden): "*name* requests VBAN access — Allow / Deny". Phone re-sends AUTH while pending; deduped by device id, no dialog spam. The prompt persists until answered even if the peer expires mid-pending; Allow persists the device (so the next AUTH succeeds), Deny → `err=denied` until restart. |
| good PIN, authorized device | `MDXM_AUTHSTATE\|ok=1` | immediate — first approval is the only slow one |

Auth binds to that `ip:port` and dies with the ping timeout. Authorized
devices are visible and revocable in the VBAN tab; PIN and device list live in
`mdxmixer.json`. PIN travels plaintext on the LAN — identical threat model to
the existing MDropDX12 TCP AUTH, inherited knowingly. The named pipe keeps its
ACL and remains the local control plane; nothing moves off it. (Unrelated
name collision, noted to prevent confusion: the existing `MDXM_PIN` verb is
the always-on-top sticky tack, not authentication.)

**Entitlement matrix** (the normative statement; prose elsewhere defers to it):

*PIN set:*

| peer class | audio | TXT control | frames |
| --- | --- | --- | --- |
| unauthenticated pinger | only if `openSubscribe` | no (PING0 + AUTH only) | no |
| authorized device (pinging) | yes, unless it sent `audio=0` | yes — full surface (exceptions: §2.4 `MDXM_SUBSCRIBE`; §6.3 pipe-only fields) | only after it sent `frames=1` |
| `alwaysStreamTarget` (synthetic, no ping) | if `alwaysStream` | no | if `alwaysFrames` |

*PIN empty:* all inbound TXT — `MDXM_AUTH` included — is dropped silently and
counted. Entries in `authorizedDevices` are **inert**: previously authorized
phones get neither audio nor control unless `openSubscribe` is on. The VBAN
tab badges "PIN unset — control disabled" whenever the pin is empty and
`authorizedDevices` is non-empty. Pin empty + `openSubscribe` off is a legal
discovery-only posture (answers PING0, serves no one) and the tab says so.

Notes:

- `openSubscribe` pingers and `audio=1` authorized peers both count as
  "audio-entitled" for §2.3's stop rule and §3.1's `vbanWanted`.
- `audio=0|1` (default 1) exists so a **control-only session** (phone sends
  verbs, does not listen) doesn't force ~1.6 Mbps of unwanted audio and flip
  `vbanWanted` for nobody — `VbanSession` sends `audio=0` when the stream card
  is stopped while a control session stays open.
- `alwaysStreamTarget` is not a peer: no table entry, excluded from
  `MDXM_VBANSTATE`'s `peers` count, shown in `MDXM_VBANPEERS` as a synthetic
  `always` row; it receives the same shared-`nuFrame` packet stream.
- All stream settings (gain, source, format, fps…) are global; concurrent
  authorized peers are last-writer-wins.
- Foot-gun accepted and documented: an authorized phone can
  `MDXM_VBAN|on=0` and cut its own line; re-enable is pipe/UI-only.

## 5. Screen capture (FRAME source)

- **DXGI Desktop Duplication, one `IDXGIOutputDuplication` per monitor** —
  duplication is the only API that captures a whole monitor *including another
  process's swapchain presentation*; `PrintWindow` is per-window and
  in-process by lesson (`window_capture.h:1-16` — owner-drawn content and the
  in-process constraint), and can't be aimed at a monitor at all. Lives in
  `net/display_capture.{h,cpp}` on its **own capture thread** — never the UI
  thread (the `CaptureUi`-under-IPC-timeout trap).
- Active only while ≥1 frames-entitled recipient exists (§4 matrix: a peer
  that sent `frames=1`, or `alwaysFrames` with a target). `AcquireNextFrame`
  returns only on change, so static screens cost nothing.
  `DXGI_ERROR_ACCESS_LOST` (UAC, mode change) → re-init with backoff.
- **Downscale on the GPU via the mip chain:** copy the duplicated frame into a
  `D3D11_RESOURCE_MISC_GENERATE_MIPS` texture, `GenerateMips()`, read back the
  largest mip whose long edge ≤ `frames.maxEdge` (default 480). A 4K frame
  becomes a ~hundreds-of-KB readback instead of the 33 MB full-res copy that
  MDropDX12's `canvas_metric.h` documents as the mistake.
- **Encode: WIC JPEG to memory** (`IWICBitmapEncoder` on a memory `IStream`,
  quality `frames.quality` default 60) → ~8–15 KB per cap.
- Send as one VBAN FRAME on `VIDEO<deviceNumber>` (§2.5), rate-limited to
  `frames.fps` (default 2.0, clamp 0.2–10, global, last-writer-wins, applied
  live and persisted like any config set). `frames=1` entitles the requesting
  peer to **all** active `VIDEOn` streams; per-display selection is
  receiver-side (ignore unwanted stream names). The **last JPEG per display is
  cached** and sent immediately to a newly frames-entitled recipient so the
  phone never opens blank.

## 6. mdxmixer config, UI, verbs

### 6.1 `VbanConfig` (in `MixerConfig`, lenient JSON like everything else)

```text
vban: {
  enabled: false, port: 6980, bindAddress: "",
  streamName: "mdxmixer", source: "personal"|"streaming" = "personal",
  format: "i16"|"f32" = "i16", gainPercent: 100 (0..6400),
  pin: "", authorizedDevices: [{id, name, lastSeen}],
  openSubscribe: false,
  alwaysStream: false, alwaysFrames: false, alwaysStreamTarget: "",
  frames: { fps: 2.0, quality: 60, maxEdge: 480 }
}
```

Ping timeout (10 s), ping expectation (~2 s), peer cap (8), strike policy
(3 / 60 s), and the catch-up burst bound are constants.

### 6.2 UI — a sixth tab, "VBAN"

Per the established checklist, extended with the sites that go silently
stale: bump `kPageCount` (`main_window.h:156`) **and the enumerating comment
above it** (`main_window.h:155`); add the name (`main_window.cpp:137`), the
`CreateVbanTab` forward declaration (`main_window.cpp:28-31`), and the
`m_pages[5] = CreateVbanTab(...)` call (beside `:152-154`); add `"vban"` to
`AppController::ShowTab`'s `kTabs[]` (`app_controller.cpp:1295`) **and its
enumerating comment** (`:1288-1289`); new `ui/tab_vban.cpp` modeled on
`tab_eq.cpp` (state struct in `GWLP_USERDATA`, theme hooks, whole proc in
try/catch); a vcxproj `ClCompile` entry; and **both** tab lists in
`docs/ipc.md` (the table row at :304 and the `MDXM_TAB` section at :397-403).

Contents: enable, port, stream name; **PIN (masked edit; shows the
"PIN unset — control disabled" badge state from §4)**; source and format
combos; gain slider; frames fps; the escape-hatch checkboxes
(`openSubscribe`, `alwaysStream`, `alwaysFrames`) + target field; a live
status strip (peers, emitting, depth/behind, packets sent, starved, dropped,
frames sent) on the 250 ms `kRefreshMsg` tick; the authorized-devices list
with **Revoke**. Optional config-driven hotkey action: toggle VBAN listener.

### 6.3 Verbs

`MDXM_VBAN` and `MDXM_AUTH` are the protocol's **first keyed-argument inbound
verbs**: every existing inbound verb is positional, and `HandleInner`'s
positional vector is built from empty-key fields only — so these handlers must
read `r.Find(key)`/`r.fields`, never the positional vector, and the rule is:
**any non-empty-key field present ⇒ set; none ⇒ query** (otherwise the
`MDXM_FEED`-shaped precedent silently turns keyed sets into queries).

- `MDXM_VBAN` — set/query.
  - Global fields (pipe always; TXT when authorized): `on`, `port`, `name`,
    `source`, `format`, `gain`, `fps`, `open`, `always`, `alwaysframes`,
    `target`. **`on` reads/writes `vban.enabled` — the listener socket.
    Emission is never directly settable; it is reported as `emitting`.**
    `port=` set while enabled rebinds immediately; the reply is sent on the
    old socket before the rebind (the `on=0` foot-gun caveat applies).
  - `pin=` — **pipe-only** (setting the secret over the channel it protects is
    circular); over TXT: `MDXM_ERR|msg=pin is pipe-only`.
  - Per-peer fields (TXT-meaningful only): `audio=0|1`, `frames=0|1`. On the
    pipe: `MDXM_ERR|msg=frames is VBAN-only` / `MDXM_ERR|msg=audio is
    VBAN-only` (`MDXM_ERR|msg=<text>` is the grammar's only negative reply —
    there is no "warning" kind).
- `MDXM_VBANSTATE` — reply record, fields enumerated in full:
  `on|emitting|port|name|peers|source|format|gain|fps|open|always|alwaysframes|target|sent|starved|dropped|depthms|behindms|srclatencyms|framessent|authpending`.
  `srclatencyms` is the server's own pipeline estimate — capture cushion +
  packet duration + mean pacer wake — recomputed from the live cushion
  values; it is the PC term of the phone's lag estimate (§7.5).
- `MDXM_VBANPEERS` — chunked list: addr, device name, authed, since, audioOn,
  framesOn; plus the synthetic `always` row when a target is configured.
- `MDXM_AUTH` / `MDXM_AUTHSTATE` — TXT-only semantics (§4); on the pipe,
  `MDXM_AUTH` → `MDXM_ERR|msg=auth is VBAN-only`.

Device revocation is UI-only in v1 — revoking auth over the channel being
revoked is a knot not worth tying.

**Documentation guard, stated precisely:** `test_docs.cpp` extracts inbound
verbs from `protocol.cpp` (`r.verb ==` scan), so it mechanically covers
`MDXM_VBAN` and `MDXM_AUTH` only. The reply records `MDXM_VBANSTATE`,
`MDXM_VBANPEERS`, and `MDXM_AUTHSTATE` get an explicit required-strings list
added to `test_docs.cpp` so they cannot drift either.

### 6.4 Firewall

First inbound listener in the app: the WiX installer adds a **program-scoped**
inbound rule (any UDP for `mdxmixer.exe`) rather than a port-scoped one — the
port is user-editable and a stale port rule would present as an undiagnosable
dead listener. The portable README documents the `netsh advfirewall`
equivalent.

## 7. MDR_Android receiver

### 7.1 Transport

One **ephemeral-port `DatagramSocket`**. The phone sends PING0 *from* it, so
audio, frames, and TXT replies all arrive on that source port — no fixed-port
bind (avoids the `MdnsDiscovery`-binds-9271-and-swallows-failure trap, and
can't clash with another VBAN app on the phone). Target = the host the app is
already connected to (or a `SavedServer` host), VBAN port from settings.

### 7.2 Components (house split: commands/parsing pure, application-scoped session)

- `network/vban/VbanCodec.kt` — pure header pack/parse + packetizers/parsers
  for AUDIO, SERVICE, TXT, FRAME. Pure JVM, unit-tested like `MessageParser`.
- `network/vban/VbanSession.kt` — socket + coroutines on `Dispatchers.IO`;
  2 s ping loop; the AUTH state machine; receive loop dispatching by
  sub-protocol into flows; session states:
  `Idle → Pinging → AuthPending → Streaming → Lost(reason)` with
  `Lost(denied)` and `Lost(locked)` terminal (no auto-retry; `err=denied` /
  `err=locked` from §4 map straight onto them). In `AuthPending` with pings
  still answered it waits indefinitely ("waiting for approval on the PC"),
  user-cancelable. **After any re-auth following peer expiry (AP roam, Wi-Fi
  drop), the session replays its state — the `audio`/`frames` flags and
  `fps`** — so previews don't silently die on every roam. Survives navigation
  and screen-off; stops when the user stops the stream.
- **Device identity:** minting moves out of `SettingsViewModel` into an
  application-scope helper (`DeviceIdentity.get(context)` — `ANDROID_ID` with
  UUID fallback, persisted on first use) used by `MdrApp`,
  `SettingsViewModel`, and `VbanSession` alike. Today the id is minted lazily
  inside the Settings ViewModel and only when its flow is collected —
  `RemoteViewModel.tryAutoConnect` already refuses to run without it — so an
  application-scoped session on a fresh install would otherwise find nothing
  and mint a divergent id, breaking the "first approval is the only slow one"
  promise.
- `audio/VbanAudioPlayer.kt` — **`AudioTrack`, no NDK/Oboe** (the repo's first
  native code is not justified against a ~200 ms network reality). Accepts
  int16 (`ENCODING_PCM_16BIT`) and float32 (`ENCODING_PCM_FLOAT`). The
  playout decisions (buffer target, skip/insert on drift, gap-vs-restart)
  live in a **pure class `JitterPolicy`** (shaped like `MixerQueryThrottle`)
  that the player only drives — the JVM test suite has no Robolectric, and
  `android.media` classes throw `Stub!` there, so the policy must be
  Android-free to be testable. Profiles: **resilient (~150 ms buffer) is the
  default for all routes** — it is what makes ~200 ms *stable* instead of
  ~200 ms with dropouts; **low-latency (~40 ms) is an explicit opt-in** for
  quiet-network experiments. The active audio route is shown as information,
  not used for auto-selection. `nuFrame` rules: serial-number arithmetic mod
  2³²; a gap ≤ the jitter-buffer depth inserts silence for the span; any
  larger gap, or a backwards jump beyond the reorder window, is a stream
  restart — flush and re-lock. The session and player live in application
  scope (the `LinkedFaderController` precedent) so streaming survives
  navigation. Underruns, inserts, skips, gaps all counted
  and surfaced. The player publishes its actual buffered depth and the
  `AudioTimestamp`-derived output latency as the phone-side terms of the lag
  estimate (§7.5).
- `network/vban/FrameAssembler.kt` — per-`VIDEOn` reassembly by packet index
  between start/end bits; output is a **completed `ByteArray` + display
  number** (pure, testable); the ViewModel/UI layer does
  `BitmapFactory.decodeByteArray`. Incomplete frames dropped at the next
  start bit.
- `network/vban/MdxmCommands.kt` — builders/parsers for the MDXM dialect
  (`MDXM_AUTH`, `MDXM_VBAN`, `MDXM_VBANSTATE`, BEGIN/END assembly with
  `nuFrame` gap → discard + re-query; enforces the ≤1436-byte single-packet
  request rule from §2.4). Kept separate from `CommandBuilder`/`MessageParser`
  so the two dialects never blur.

Capability gating is the PING0 reply itself — `bitfeature` advertises
AUDIO/FRAME/TXT — so no `PcFeature`/`GET_VERSION` coupling to MDropDX12. The
whole feature works with the visualizer closed.

### 7.3 Foreground service and background delivery

**Starting the VBAN stream starts `RemoteForegroundService`** regardless of
the `background_service` setting (and stops it with the stream unless that
setting holds it) — on a default install the service doesn't exist, so there
is nothing to keep the process alive otherwise. The real mechanisms, named:
the foreground service is what exempts the socket from Doze/standby network
restrictions, and active `AudioTrack` playback keeps the CPU up; an explicit
wake/Wi-Fi lock is added only if soak testing shows it is needed. (The
`MulticastLock` is unrelated — it lifts the Wi-Fi driver's multicast filter
for *beacon discovery*; all VBAN traffic here is unicast.)

The written policy ("`connectedDevice`, never `mediaPlayback`") inverts
because its premise does: once real PCM renders, the honest declaration is
`android:foregroundServiceType="connectedDevice|mediaPlayback"` +
`FOREGROUND_SERVICE_MEDIA_PLAYBACK`, passing `mediaPlayback` in
`startForeground` **only while the stream is live**. The policy comments in
`AndroidManifest.xml` and `RemoteForegroundService.kt` are updated to say why.
The player takes audio focus (`AUDIOFOCUS_GAIN`) while streaming — phone media
pauses, as users expect — and abandons it on stop.

### 7.4 Volume model and rocker precedence

Two shipped modes already bind the rocker to the PC (`volume_rocker_enabled`
key interception in `MainActivity.onKeyDown`; `media_device`'s remote
`VolumeProvider` session), so "native local volume" must be *made* true, not
assumed: **while the VBAN stream is live, local playback wins** — the rocker
interception stands down and the remote `VolumeProvider` session is released
(restored when the stream stops), so the keys reach `STREAM_MUSIC`. The
**PC-side stream gain** is a slider on the stream card sending
`MDXM_VBAN|gain=…` (absolute, idempotent); the phone reads gain/source from
`MDXM_VBANSTATE` on connect **and refreshes them on its periodic state poll**
(another authorized peer may have moved them). Binding the rocker to PC gain
instead is a possible later setting, not v1.

### 7.5 UI and persistence

- **Stream card** on the Remote screen: start/stop, session state (including
  "waiting for approval on the PC"), PC gain slider, latency profile
  indicator, drop/underrun counters, and a live **end-to-end lag estimate**:
  RTT/2 (PING0, §2.3) + `srclatencyms` (§6.3) + actual jitter-buffer depth +
  `AudioTimestamp` output latency + a manual **output offset** slider for
  sinks that don't report theirs (Bluetooth; hint ~150 ms). Shown big, in ms,
  with the breakdown a tap away — the number exists to be typed into a video
  player's audio-delay for movie lip-sync.
- **Display previews** drawn with `drawImage` into the existing `MonitorMap`
  `DrawScope`, behind the number/caption — the map is one `Canvas`, and its
  single tap gesture already means *select monitor to configure*, which is
  preserved; the larger view gets its own gesture (**long-press**). Previews
  are keyed by **device number** (`DisplayInfo.deviceNumber`, the `n` in
  `\\.\DISPLAYn` ↔ `VIDEOn`), never by list position. Frames toggle sends
  `MDXM_VBAN|frames=1|fps=…`.
- **Settings → "Headphone stream" section:** VBAN port, PIN (its own stored
  value — mdxmixer's PIN is independent of MDropDX12's; the hint text may
  suggest using the same value, but nothing links them), latency profile
  (resilient default / low-latency opt-in), frames on/off + fps.
- DataStore only (`vban_port`, `vban_pin`, `vban_profile`, `vban_frames`,
  `vban_fps`, `vban_output_offset`); **no Room change** (no migration risk). PC-side state (gain,
  source) is never persisted on the phone — it is queried.

## 8. Edge cases and error handling (consolidated)

- **Packet loss (audio):** `nuFrame` serial-arithmetic rules in §7.2 — small
  gap = silence fill, large/backwards = flush and re-lock. No retransmission
  by design.
- **Packet loss (TXT):** §2.4 — discard partial reply, re-query; re-send sets.
- **Packet loss (FRAME):** drop that frame; next capture replaces it.
- **Ring starvation (PC):** silence packets at cadence + starved counter (§3.2).
- **Sender behind schedule:** bounded catch-up bursts; worst case rides
  drop-oldest, counted; `depthMs`/`behindMs` observable (§3.2).
- **Mix-rate change:** ring flush; new SR index self-announces; receiver
  treats the header change as a restart and reconfigures `AudioTrack`.
- **Phone roams APs / IP changes:** peer expires in 10 s; phone's next PING0 +
  AUTH (instant for an authorized device) re-establishes, and the session
  **replays `audio`/`frames`/`fps`** (§7.2).
- **PC sleep/resume:** engine already handles suspend; VbanServer re-arms the
  socket on resume like `RenderRetry`.
- **Two mdxmixer instances:** second bind of 6980 fails → logged, VBANSTATE
  shows error; no silent swallow (the lesson from `MdnsDiscovery`).
- **Peer table full / port-rotating attacker:** eviction order and the
  separate per-IP strike table (§3.3).
- **Pin cleared while devices authorized:** everything TXT dies silently by
  design; the VBAN tab badge is the diagnosis path (§4).
- **Duplication denied (secure desktop, DRM):** capture thread backs off and
  retries; audio is unaffected.
- **Approval dialog while main window hidden:** tray balloon; clicking opens
  the prompt. The prompt outlives the requesting peer's expiry (§4).
- **Receiver behind on bursts:** pacing (§3.2) bounds burst size at the source;
  the resilient profile absorbs the rest.

## 9. Testing

**mdxmixer (headless `Test` config, homegrown framework; every new test file
gets its vcxproj `ClCompile` entry):**

- `test_vban_protocol.cpp` — header round-trips (48 kHz = index 3,
  value-minus-1 fields, sub-protocol bits, **SERVICE `format_SR == 0x60`
  exactly**), audio packetizer sizes, FRAME chunker (start/continue/end,
  **single-packet frame = 0x05**, 16-bit index wrap at 65536), TXT chunking
  of BEGIN…END at record boundaries, `nuFrame` sequencing, gain→int16
  conversion + `SoftLimiter` clipping.
- `test_vban_peers.cpp` — pure state machine: ping renew, 10 s expiry, auth
  binding and expiry, strike/lockout table (incl. source-port rotation),
  denied-until-restart, pending-auth dedupe, table-full eviction order, the
  full §4 entitlement matrix (pin set/empty × openSubscribe × per-peer
  audio/frames × alwaysStream/alwaysFrames), and the `vbanWanted` predicate.
- `test_mix_demand.cpp` — extended to assert `AnyoneListening()` **ignores**
  the reporting-only `vban` field (the fj#10 guard).
- Socket loopback over 127.0.0.1 with an injected ring — no audio devices;
  full engine-to-wire integration gated behind the `Audio_` prefix.
- `test_docs.cpp` — covers the inbound verbs `MDXM_VBAN`/`MDXM_AUTH`
  mechanically; extended with an explicit required-strings list for
  `MDXM_VBANSTATE`/`MDXM_VBANPEERS`/`MDXM_AUTHSTATE` (§6.3).
- Dev CLI (house precedent `--feed`/`--monitor`): `--vban` counters dump,
  `--vbanrx <host>` minimal receiver for phoneless dev loops.

**Interop (the payoff of compliance):** before any Android code exists —
**VBAN Receptor Lite** (phone) plays the audio with `openSubscribe=true`;
**Voicemeeter** receives it via `alwaysStream` + target; **VBAN-Screen**
displays `VIDEOn` via `alwaysFrames` + target, including a deliberately
near-black (single-packet) frame. Any failure against these is a spec bug on
our side.

**MDR_Android (pure-JVM junit, `MessageParser`-style — no Robolectric, which
is why `JitterPolicy` and `FrameAssembler` are Android-free):** `VbanCodec`
golden byte vectors hand-derived from the spec; `FrameAssembler` under loss,
reorder, interleaved streams, and the single-packet frame; `JitterPolicy`
skip/insert/restart decisions including `nuFrame` serial arithmetic across
the 2³² wrap; `MdxmCommands` parse/build + the 1436-byte request bound; the
lag-estimate assembly (a pure function over the RTT / `srclatencyms` /
buffer-depth / output-latency / manual-offset terms). On device: latency per
route (wired vs BT) checked against the on-card estimate, screen-off soak,
AP-roam reconnect with state replay.

## 10. Rollout

Each phase lands independently useful:

1. **mdxmixer:** `vban_protocol` + `vban_peers` pure + tests → `vban_server`
   AUDIO + SERVICE → *listening already works via Receptor Lite.* AUTH does
   not exist yet, so `openSubscribe`/`alwaysStream` are the only entitlement
   paths in this phase and the acceptance check runs with
   `openSubscribe=true`; the §4 default-off posture is meaningful from
   phase 2 on.
2. **mdxmixer:** TXT control + AUTH + approval UI + VBAN tab + verbs/docs
   (includes the `DispatchToUi` extraction, §3.3).
3. **mdxmixer:** FRAME + `display_capture` → VBAN-Screen check via
   `alwaysFrames`.
4. **MDR_Android:** `DeviceIdentity` helper, `VbanCodec`/`VbanSession`/
   `VbanAudioPlayer`+`JitterPolicy`, stream card, foreground-type change,
   rocker precedence. (v1.5.0 target.)
5. **MDR_Android:** `FrameAssembler` + Displays previews + settings.

mdxmixer target version: **v1.2.0** (`version.h`'s three numeric macros are
the only place it changes). Changelog, README (third destination class:
network), and `docs/ipc.md` sections land with each phase. Phases 1–3 are one
implementation plan in this repo; phases 4–5 are a second plan in MDR_Android
referencing this spec.

## 11. References

- VBAN spec rev 13: `https://vb-audio.com/Voicemeeter/VBANProtocol_Specifications.pdf`
  (text extraction archived in session scratchpad during research).
- VB-Audio VBAN hub + apps: `https://vb-audio.com/Voicemeeter/vban.htm`;
  open-source list: `https://vb-audio.com/Services/support.htm#VBAN`.
- MIT semantic references: `github.com/wmbest2/aiovban`, `github.com/thib3113/vban`.
  GPL (reference-only, never copy): `github.com/quiniouben/vban`,
  `github.com/norihiro/obs-vban`.
- House precedents cited throughout: `ipc/stream_feed.{h,cpp}`,
  `engine/mix_demand.h` (fj#10), `dsp/ring_buffer.h` (fj#13),
  `app/app_controller.cpp` (IpcRequest marshalling),
  `docs/notes/2026-09-22-passthrough-monitor-and-wasapi-lessons.md`,
  MDropDX12 `tcp_server.{h,cpp}` (auth model), `canvas_metric.{h,cpp}`
  (readback economics).
