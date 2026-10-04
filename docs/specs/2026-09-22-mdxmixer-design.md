# mdxmixer — design

2026-09-22. Status: approved design, implemented (v1).

## What this is

A native Windows replacement for the SteelSeries Sonar mixer concept: apps
grouped into channels, each channel with its own EQ and independent
**personal** (what you hear) and **streaming** (what goes out) volumes, a
streaming submix any recorder can capture, and a processed virtual
microphone.

## Why

Sonar fails in layers, and the layers were observed separately on the machine
this replaces it on:

- The **driver and audio engine** are solid — muting a Sonar channel reaches
  real audio even while everything above it is broken.
- The **Electron GUI** crashes at startup (certificate-verification failures
  against its own localhost TLS, `ERR_FILE_NOT_FOUND` for its sub-app), and
  its restart mechanism no longer brings it back.
- The **local HTTP API** stops answering, at which point MDropDX12's Sonar
  provider sees no channels and remote volume control is gone entirely.

mdxmixer replaces the flaky layers (GUI, API, a six-process service mesh) with
one native tray app and a named pipe, and reuses the sturdy concept — signed
virtual audio devices — from a vendor whose driver is its whole product.

## Decisions already made

| decision | choice |
|---|---|
| Virtual endpoints | Third-party signed cables (VB-CABLE family first), never our own kernel driver in v1 — no EV cert, no test-signing mode. Cables are config-bound, so any vendor's cable works, including the open-source signed Virtual-Audio-Driver. |
| Channel layout | Configurable. Channels are config entries with names; Sonar's fixed five is just one possible config. Start with three app channels. |
| Program shape | Self-sufficient tray app + native Win32 UI, plus a named-pipe IPC server. Works with nothing else running. The tray icon is always present (Exit lives there); a **taskbar option** (`ui.taskbarButton`, off by default, toggleable from the tray menu) makes the main window a normal taskbar citizen — taskbar button shown, close minimizes instead of hiding to tray. Autostart at login stays a first-class setting either way. |
| Engine topology | Single mix graph, one clock master (the personal output device). Rejected: dual independent graphs (double capture, drift), APO-based EQ (registry surgery, per-mix processing foreclosed). |
| EQ model | One parametric EQ per channel, shared by both mixes; volumes are per mix per channel. Matches the dual-fader model MDropDX12's mixer window already renders. |
| Language / conventions | C++17, Win32, x64 only, wide strings throughout, static CRT, no-crash rule. MDropDX12 naming: `PascalCase` functions, `m_camelCase` members, namespace `mdxm`. |

## Architecture

One user-mode process, `mdxmixer.exe`.

```
apps ──per-app routing──▶ channel cable (render side)
                              │  capture side recorded
                              ▼
                     per-channel ring buffer
                              │
                      per-channel EQ (shared)
                         ┌────┴────┐
                 personal gain   streaming gain
                         │           │
                   Σ personal    Σ streaming
                    + limiter     + limiter
                         │           │
                 physical output  stream cable ──▶ OBS records
                 (clock master)      its capture side

physical mic ──capture──▶ mic gain/EQ ──▶ mic cable ──▶ voice apps
                                            record its capture side
```

- A **channel** owns one cable: apps are routed into the cable's render
  endpoint; mdxmixer records the cable's capture endpoint.
- The **personal mix** renders directly to the chosen physical device. That
  stream's event callback paces the whole graph.
- The **streaming mix** renders into a dedicated cable; recorders capture its
  other side like a microphone.
- The **mic chain** is the same pattern reversed: capture the physical mic,
  process, render into the mic cable; voice apps select the cable's capture
  side as their microphone. NVIDIA Broadcast can sit upstream unchanged — its
  output endpoint is just a capture device to bind as the mic input.

### Audio engine specifics

- WASAPI shared mode, event-driven, ~10 ms quantum everywhere. Internal
  format float32. All cables get configured to 48 kHz so the steady state
  does no resampling; a linear-interpolation fallback resampler covers
  mismatched endpoints rather than refusing them.
- One SPSC lock-free ring per channel between its capture thread and the mix
  thread. Clock drift between cable streams and the physical device is
  absorbed by the rings: underflow renders silence, overflow drops oldest,
  and both keep counters that `DIAG` reports — measured, not assumed silent.
- Gain changes ramp over ~10 ms (mute is a ramp to zero). No steps, no
  clicks.
- EQ: cascade of biquads (Direct Form II transposed), parametric bands with
  freq/gain/Q, 10 bands by default, per channel, bypassable per channel.
- A soft limiter on each mix sum, because summing N channels at unity must
  not clip the output.
- Expected end-to-end added latency ~20–30 ms (capture quantum + ring +
  render quantum) — the same order as Sonar's own user-mode engine.

### Per-app routing

The undocumented `IAudioPolicyConfigFactory` — the API Windows Settings'
"App volume and device preferences" page itself uses, the one EarTrumpet and
SoundSwitch have shipped on for years. It assigns an app's default render
endpoint persistently, which is how an app lands on its channel's cable.

Known risk, accepted: undocumented means a Windows update can move it. It is
isolated behind one small interface in one file (`routing/audio_policy_config`),
which tries the known Win10 and Win11 interface IDs in turn; a break is one
file to fix, and the failure mode is "assignment stops working", surfaced in
the UI — never a crash, never broken audio for already-routed apps.

Same limitation Sonar has: an app that selects a specific output device by
name ignores per-app default routing. Listed as a fact in the UI's routing
tab, not fought.

### What happens when mdxmixer is not running

An app routed to a cable plays into a pipe nobody drains: **silence**. Sonar
has the identical property. Consequences, stated rather than hidden:

- Autostart at login is a first-class setting (HKCU Run key), presented at
  first run — but written only when the user ticks it, never silently.
- Exit from the tray menu warns that routed apps go silent until relaunch or
  reassignment.
- v1 ships no watchdog process. One process, one job.

## Components

| module | job |
|---|---|
| `device/` | endpoint enumeration; `IMMNotificationClient` watcher; bindings stored as endpoint id + name, matched id-first with exact-name fallback (a re-paired Bluetooth device returns under a new id with the same name — learned the hard way in MDropDX12) |
| `routing/` | the policy-config wrapper; audio-session enumeration (`IAudioSessionManager2` across render endpoints) so the routing tab lists what is actually playing, where |
| `engine/` | capture streams → rings → mix loop → two render sinks; the mic chain; drift/underrun counters |
| `dsp/` | biquads, gain ramps, limiter — pure functions over float buffers, no Windows types, unit-testable headless |
| `config/` | one JSON file, exe-relative (portable); buffered writes flushed ~1 s, atomic replace (temp + rename); no registry |
| `ipc/` | named-pipe server, protocol below |
| `ui/` | tray icon + one native window: Mixer / Routing / EQ / Devices tabs |
| `app/` | WinMain, single-instance mutex, autostart toggle, level-gated logging to `log/` |

## IPC protocol

`\\.\pipe\mdxmixer` — fixed name (single instance), duplex message mode,
UTF-16LE null-terminated messages. Same record grammar as MDropDX12's pipe:
`VERB|field=value|…`, ids contain no `|`.

    MDXM_PING                                → MDXM_PONG|version=…
    MDXM_STATE                               → chunked, MDXM_BEGIN … MDXM_END
      MDXM_CHAN|id=game|name=Game|health=ok|pvol=0.85|pmute=0|svol=1.0|smute=0|eq=1
      MDXM_ROUTE|id=personal|device=<endpointId>|name=<display>
      MDXM_DEV|id=<endpointId>|name=…|flow=render|active=1
    MDXM_SET=<ch>|<personal|streaming>|<0..1>
    MDXM_MUTE=<ch>|<personal|streaming>|<0|1>
    MDXM_EQ_SET=<ch>|<band>|<freq>|<gain>|<q>
    MDXM_EQ_ENABLE=<ch>|<0|1>
    MDXM_ASSIGN=<exePath>|<ch>            channel "-" clears the assignment
    MDXM_ROUTE_SET=personal|<endpointId>  move the personal output
    MDXM_SUBSCRIBE=<0|1>                  push MDXM_CHAN / MDXM_ROUTE on change
    MDXM_DIAG                             ring depths, drift counters, stream states

Writes are optimistic (reply carries the requested value); the subscription
push carries the truth. Chunked replies end with a terminator record —
clients read until `MDXM_END`, exactly the MDropDX12 `MIXER_STATE` contract.

## MDropDX12 integration (separate deliverable, other repo)

A `mixer_provider_mdxmixer` implementing `IMixerProvider`
(see `src/mDropDX12/mixer_provider.h`) over this pipe:

- one `Channel` per mdxmixer channel, with `personal` and `streaming` faders —
  the same dual-fader shape the Sonar provider publishes, so the mixer window,
  groups, and hotkeys need nothing new;
- one `RouteTarget`, `mdxm:personal`, for manual route moves from the
  MDropDX12 UI. Failover itself is native to mdxmixer (see Error handling);
  the MDropDX12 failover watcher stays disarmed for mdxm routes.

Named pipe instead of HTTP, no read-back inside writes, no service to wedge:
the request-count discipline docs/mixer.md records for Sonar becomes moot,
but the provider keeps the batch-then-refresh shape because it is correct.

Nothing in the mdxmixer repo depends on MDropDX12. The provider lands as a
branch in the MDropDX12 repo after mdxmixer's pipe is real.

## Configuration

`mdxmixer.json`, beside the exe:

```json
{
  "channels": [
    {
      "id": "game",
      "name": "Game",
      "cable": { "renderId": "…", "captureId": "…", "renderName": "CABLE-A Input (VB-Audio…)", "captureName": "CABLE-A Output (VB-Audio…)" },
      "personal":  { "vol": 1.0, "mute": false },
      "streaming": { "vol": 1.0, "mute": false },
      "eq": { "enabled": false, "bands": [ { "freq": 62.5, "gain": 0.0, "q": 1.0 } ] },
      "apps": [ "C:/Games/game.exe" ]
    }
  ],
  "personalOutput": { "id": "…", "name": "Headphones (2- WF-1000XM6)" },
  "personalFailover": {
    "armed": false,
    "allow": [ { "id": "…", "name": "Speakers (Realtek)" } ],
    "stabilitySec": 5,
    "dwellSec": 30
  },
  "streamingCable": { "renderId": "…", "renderName": "…" },
  "mic": {
    "input": { "id": "…", "name": "…" },
    "cable": { "renderId": "…", "renderName": "…" },
    "gain": 1.0,
    "eq": { "enabled": false, "bands": [] }
  },
  "ui": { "taskbarButton": false },
  "autostart": false,
  "logLevel": 2
}
```

Every device binding carries id and name, for the re-pair reason above.

## Error handling

- The mixer never crashes: top-level SEH plus `std::exception` handling on
  every thread, matching the MDropDX12 rule.
- A missing cable at startup = that channel drawn unhealthy with a fix-it
  hint (which endpoint name it wanted). The app always starts.
- Personal output disappearing = immediately fall back to the Windows
  default render device (a temporary hop, not a route change), say so in the
  UI. Never silent, never stuck.
- **Native failover** (2026-09-23, replaces "re-homes over IPC"): mdxmixer
  itself owns personal-output failover, with the semantics MDropDX12's
  `mixer_failover` proved out: opt-in (armed flag; with no allowlisted
  device present it does nothing and says why), an ordered allow list where
  the first present entry wins, a stability window so a brief Bluetooth
  dropout cancels on return and nothing moves, a minimum dwell after each
  commit so one bad unplug doesn't cascade down the list, and **fail over,
  never back** — a commit makes the new device the configured personal
  output; reconnecting the old one does nothing. Allow entries store id AND
  name and match on either (the re-pair lesson). The temporary default-
  device hop above is not a commit: if the original device returns within
  the stability window, the route returns to it. Clock and device presence
  are injected so every sequence is testable without hardware.
- Routing API failure = assignment features degrade and say so; audio paths
  already established keep flowing.
- Device arrival/loss comes from `IMMNotificationClient` events, never a
  poll.

## Testing

- **Unit, headless** (no cables, no devices): biquad magnitude response
  pinned at reference frequencies; gain-ramp continuity (bounded first
  difference — the no-click property); limiter ceiling; config round-trip;
  protocol tokenizer including the `MDXM_END` chunking contract.
- **Integration, on a machine with cables**: render a known tone into a
  channel cable, run the engine, capture the streaming cable's output,
  assert RMS and frequency. Audio-only — no windows, no default-device
  changes, nothing routed away from a device a person is listening to.
- Engine drift counters are asserted zero-ish over a minutes-long soak.

## Rollout on the target machine

Ordered so audio never goes silent without a way back:

1. Install VB-CABLE and the A+B and C+D packs — five cables: one per app
   channel (three to start) plus streaming out plus mic. Fewer cables just
   means fewer app channels at first; the two fixed roles always cost two.
2. Set every cable to 48 kHz shared-mode default format.
3. Configure mdxmixer channels bound to the cables; run the tone test.
4. Assign one low-stakes app to a channel; verify it is heard through the
   personal mix.
5. Migrate remaining apps, then voice apps to the virtual mic, then point
   the recorder at the streaming cable.
6. Idle Sonar (stop GG autostart). Rollback at any step = reassign apps back
   to their previous devices; Sonar's engine is still installed and working.

## Risks

| risk | posture |
|---|---|
| `IAudioPolicyConfigFactory` shifts in a Windows update | isolated in one file; failure degrades assignment, never audio |
| Cable/device clock drift | rings absorb; counters measure; soak test pins |
| Shared-mode latency stacks | ~20–30 ms budget accepted; same class as Sonar |
| VB cable count caps channels (5 cables ≈ 3 app channels) | acceptable for v1; more channels later via multi-instance cable drivers |
| Apps that pick their own output device bypass routing | inherent to per-app routing; Sonar shares it; documented in UI |

## Non-goals (v1)

No first-party kernel driver. No noise suppression (NVIDIA Broadcast chains
upstream). No ASIO, no exclusive mode, no network audio. No changes to
MDropDX12 in this repo. No watchdog process. No Sonar config import.

## License

CC-BY-NC 4.0, the same as MDropDX12 and the rest of these projects, held by
the same copyright holder — which also settles the terms on the parts adapted
from MDropDX12 (`config/json_utils`, the pipe server's shape, the mixer
window's conventions). See `LICENSE`.

The repo is private on Forgejo with no push mirror, so nothing is published by
committing.
