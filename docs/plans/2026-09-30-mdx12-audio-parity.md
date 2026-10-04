# Porting MDropDX12's audio settings into mdxmixer

Read of MDropDX12 at `29c4806d` (v3.3, 29 Sep 2026) against mdxmixer at
`06c81fd`. The question behind it: mdxmixer is not yet workable as the daily
mixer, and mdx12's mixer is — what has to move across for that to change.

MDropDX12's audio surface is about 12,000 lines across `audio_*`,
`mixer_*` and `engine_mixer*`. Most of it is not settings: it is the provider
model, the Sonar transport and the device watcher. The settings themselves are
one file, `mixer_settings.h`, and they are the cheap half.

## The finding that matters most

**Sonar's channel levels are not reachable through Windows.** mdx12 measured
it and wrote it down in `mixer_provider_sonar.h`: `SteelSeries Sonar - Aux`
accepts `SetMasterVolumeLevelScalar`, returns success, and holds its level at
1.0 — "only Sonar can move a Sonar channel". Its own `mixer_settings.h` calls
those endpoints decoys: "named like the channels they shadow, stuck at 1.000,
and inert", and hides them by default behind `showVirtualEndpoints`.

Re-measured here on 2026-09-30 through mdxmixer, and the Windows side is
**writable and readable**: `MDXM_DEVVOL` on Sonar Aux moved it 100 → 50 → 42 →
100 and `--levels` read each value back. What that proves is only that Windows
stores the value; it does not show Sonar honouring it in its own mix, which is
the claim mdx12 makes and which matches the endpoints sitting at 1.000 with
nothing ever moving them.

So the eight Sonar rows in mdxmixer's device list are, at best, unproven
controls, and the Personal/Streaming pair per channel — the thing the mixer is
actually for — is not among them at all. mdx12 gets that pair from Sonar's own
local HTTP API.

## What mdx12 has that mdxmixer does not

Ordered by what it would change about using mdxmixer daily.

### 1. The Sonar provider — `mixer_sonar_http.*`, `mixer_provider_sonar.*`

~500 lines and the only route to Game/Chat/Media/Aux Personal and Streaming
levels. Discovery is `coreProps.json` → `ggEncryptedAddress` → one HTTPS GET
`/subApps` → the sonar sub-app's plain-HTTP address; the port moves when GG
restarts, so a connection-level failure re-discovers exactly once. Timeouts are
deliberately short (1 s connect, 2 s receive) because GG hangs regularly and a
wedged GG must park one worker thread, not the app.

Two hard-won details are in the code and would be lost by a rewrite: the write
path puts the **slider before the channel** (confirmed against a live GG on
2026-08-30; channel-first returns HTTP 400, and so does a nonsense channel, so
the error distinguishes nothing), and the provider only talks to the network
while something is subscribed.

This is the piece that makes mdxmixer a usable Sonar front-end **today**,
independently of whether the loopback mixing path (`Add Sonar channels`, added
yesterday) is trusted yet. The two are complements: the provider *controls*
Sonar's mixer, the loopback path *replaces* it.

### 2. The provider vocabulary — `mixer_provider.h`

A channel owns one or more faders; an endpoint has `main`, a Sonar channel has
`monitoring` and `streaming`. Health is `Ok | Degraded | Unavailable`, with
Degraded showing stale values rather than hiding them. mdxmixer currently has
two hard-coded axes (channel faders, endpoint volumes) and no room for a third
source. Adopting the vocabulary is what lets Sonar channels, endpoint volumes
and mdxmixer's own loopback channels sit in one list without special cases.

`EndpointInfo` there also carries two flags mdxmixer lacks: `isDisplayAudio`
(from `PKEY_AudioEndpoint_FormFactor`, not the name — 76 of 84 endpoints on
that machine were HDMI/DisplayPort ports with nothing plugged in) and
`isVirtual`.

### 3. Volume hotkeys and fader groups

mdxmixer has **no hotkeys at all**. mdx12 ships three actions — Group 1 Up,
Down, Mute — moving every fader ticked into the group, and the semantics are
the tested part:

* up/down **clamp per fader, not per group**, so a group whose members sit at
  different levels keeps those differences after being run to the rail;
* mute is **one decision for the whole group** — if any member is unmuted the
  key mutes them all — and a fader that refuses mute (both Sonar masters do) is
  skipped rather than allowed to decide the group's direction;
* the notification **counts, it does not name**: five fader names do not fit.

It also records what this replaced — two named "personal"/"streaming" slots
that "never did anything for me" — so the group model is the second attempt,
not the first.

### 4. Failover hardening (#410, 29 Sep)

Two changes from a real incident: thirteen "Failover moved a route" in a row,
exactly 24 s apart, each a teardown and rebuild of the audio graph, ending with
the audio engine restarting.

* **Escalating dwell on repeated commits to the same target** — 10 s, 20, 40,
  80, capped. Reset only by a move that sticks, keyed on the target rather than
  on who re-baselined the watcher. The anti-flap timers were not failing; they
  were setting the loop's period instead of stopping it.
* **`minGapSeconds` (default 5)** — a floor between *any* two endpoint
  reassignments across all routes. `minDwellSeconds` governs one route and
  cannot see the others, so two routes losing their device together still moved
  in the same tick.

mdxmixer's `FailoverDecider` has the stability window and the per-route dwell
and neither of these. Its exact loop is harder to reach — mdxmixer rewrites
`personalOutput` to the committed target, so the bound device is the target by
construction — but a flapping Bluetooth device still walks the allowlist at
dwell speed, and every commit here is also a graph teardown.

### 5. Audio-engine watch and "Restart audio" (#410, #422)

mdx12 watches AUDIODG.EXE by pid from the mixer worker and notices the audio
engine dying and coming back. The warning box is **armed on detection and shown
only if the engine is still down ten seconds later** — killed cleanly AUDIODG is
back in under a second, so an immediate box reports an outage nobody noticed.

The button does, in this order:

    Stop-Process -Name audiodg -Force -ErrorAction SilentlyContinue
    Restart-Service -Name Audiosrv -Force

Kill first because the slow half of a service restart is the stop, and AUDIODG
is usually what the SCM is waiting on. **Killing AUDIODG alone is not enough** —
that was measured the hard way and left the machine with no sound at all, since
Windows only starts it on demand and nothing can open a stream when the path is
broken enough to need restarting. Both steps need elevation, so it goes through
`ShellExecuteEx` with `runas`, `-NoProfile -NonInteractive`.

### 6. Settings mdxmixer simply does not have

| mdx12 setting | mdxmixer | Worth porting |
|---|---|---|
| `showVirtualEndpoints` (default off) | shows all 8 Sonar decoys | **Yes** — cuts the list in half today |
| `volumeStepPercent` (5) | spinner steps 1, no setting | Yes, with hotkeys |
| `sortUnmutedFirst` | no | Yes — sorts by channel, not fader |
| `pinFailoverDevices` | manual pin only | Yes — auto-pin allowlist members |
| `allowSort` (preferred/name/battery/last seen) | failover list unsorted | Yes |
| `faderPrefs.shortName` | no | Yes — pairs with the alias work |
| `confirmHotkeySeconds` / `confirmToolWindow` / `confirmRemote` | no | With hotkeys / remote |
| `spinBoxes` | both always shown | No — mdxmixer's is better |
| `order` (manual fader order) | sort rules + pin | Maybe — the sort may be enough |
| `enabled` / `sonarEnabled` | n/a | Only `sonarEnabled` applies |

Already at or past parity: device aliases (mdxmixer anchors on ContainerId,
which mdx12 does not), hidden faders + "show hidden", battery, last-seen with
the local-time trap handled, stability/dwell, the allowlist itself.

### Not worth porting

`audio_profile_store.*` is the visualiser's FFT/band analysis — `bandMode`,
`fftScale`, `avgAttack`. Nothing to do with a mixer.

`mixer_sonar_restart.*` (restarting the whole GG tree) was superseded in mdx12
itself by the audio-routing restart above.

#418 (per-buffer logging drowning the verbose log at ~50 lines/second) does not
apply: mdxmixer's capture and render paths log nothing per buffer.

#420 (losing the audio device tearing the app down mid-render) is a DX12
render-thread lifetime bug, not a mixer one.

## Suggested order

1. Hide the virtual/decoy endpoints by default, with the setting to show them.
   One evening, and it halves the device list.
2. The Sonar provider, behind the provider vocabulary. This is the block that
   makes mdxmixer worth opening instead of Sonar.
3. Volume hotkeys with groups.
4. Failover: `minGapSeconds`, escalating dwell, the monitor filter on the
   device picker.
5. The remaining view settings, which are small once there is a settings tab to
   put them on.
