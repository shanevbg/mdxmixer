# Changes

## v1.2.0 (unreleased)

### mdxmixer speaks VBAN

The monitor mix can leave this machine over the network, to be listened to on a
phone — and the phone can drive the mixer back over the same association. The
protocol is **VBAN** (VB-Audio's, rev 13), implemented compliantly rather than
approximated, so any standard VBAN tool on the network can discover this
machine and play its stream: no second protocol to invent, and the first-party
apps are the interoperability test.

Design: `docs/specs/2026-10-07-vban-stream-server-design.md`. This reverses a
v1 non-goal — "no network audio" was an estimate of the work, not a judgement
that it did not belong.

- **The wire layer.** The 28-byte header, the sample-rate table (three
  geometric families, so 48 kHz is index 3 and computing the index instead of
  looking it up puts the stream on the wrong rate), and packet validation.
  Pure and headless-tested down to the byte offsets of every field: this is the
  one part of the feature whose mistakes are invisible from this end of the
  network and silent at the other.

- **Audio out, on demand.** The listener is up whenever you have switched it on
  — and that setting persists, because the point is that a phone can subscribe
  without anyone touching the PC. Nothing is actually sent until a client asks,
  by pinging, and it stops when the last one stops asking. A client that goes
  quiet for ten seconds has stopped asking.

- **The personal mix, amplified before the wire.** What goes out is the monitor
  mix by default — your per-channel balance and personal mutes, not the
  programme mix. It is amplified PC-side (up to 6400 %) because the personal sum
  on this machine runs at a few percent of full scale: the faders *are* the
  listening level, so sending it unamplified would put a 20–40 dB-down signal
  into a phone.

- **It is the real protocol, so VB-Audio's own apps play it.** VBAN Receptor
  Lite on a phone, or Voicemeeter, will find and play the stream with nothing of
  ours installed. That is the point of implementing the specification rather than
  approximating it: the interoperability test is somebody else's software.

- **Two new diagnostics.** `mdxmixer --vban` asks the running instance what the
  stream is doing; `mdxmixer --vbanrx <ip>` subscribes to one and reports what
  arrives, so "the phone hears nothing" can be split into "nothing is being
  sent" and "something is wrong at the phone" without a phone in the room.

- **The mixer, over the same association.** An authorised phone can drive
  mdxmixer through VBAN's own text sub-protocol — the same record grammar the
  named pipe speaks, so a client needs no second dialect — which means the
  stream and the controls need the visualiser running for neither. Getting in
  works the way MDropDX12's remote does: a PIN, then a one-time approval prompt
  on the PC, after which that device reconnects without asking. Three wrong PINs
  lock the address out for a minute, and a refusal is remembered.

- **Pictures of the displays.** Each screen goes out as `VIDEO<n>` using VBAN's
  `FRAME` sub-protocol, which is what VB-Audio's VBAN-Screen reads — so the
  phone is not the only thing that can watch them. The downscale happens on the
  GPU before the read-back: a 4K screen costs a dozen kilobytes rather than the
  thirty megabytes a full-resolution copy would, and nothing is captured at all
  while nobody is watching. Desktop Duplication, not PrintWindow, because the
  thing most worth seeing on these screens is another process's DX12 output.

- **A VBAN tab**, with the settings, the live counters and the list of devices
  that have been let in. The three latency numbers are labelled rather than
  printed, because the one everybody reads first — how far ahead the producer is
  — is the one that usually means nothing is wrong.

- **One bug caught before it shipped.** A datagram larger than any legal VBAN
  packet made the receive loop treat a per-datagram error as fatal, so one
  oversized packet from anywhere on the network would have stopped mdxmixer
  answering pings for good. The same path had a worse everyday case: Windows
  reports a bounced send as an error on the next *receive*, so the listener
  would have died the first time a phone walked out of range.

- **The installer authorises the exe for inbound UDP**, program-scoped rather
  than port-scoped so that changing the port cannot strand the rule. A portable
  copy needs one `netsh` line, which the README gives: without it the symptom is
  a phone that never connects and nothing anywhere saying why.

## v1.1.0 (unreleased)

Everything here is about the seam between mdxmixer and the programs that drive
it — MDropDX12 above all, which now takes its mixer channels from here rather
than driving Sonar itself.

### A front-end can delegate its device list

- **Device rows reach subscribers.** `MDXM_DEVSET` says which rows exist, in
  mdxmixer's own sort order, and a changed row arrives as the same
  `MDXM_DEVLVL` a poll would have given. Before this a subscriber learned
  about channels and never about devices, so a delegated device list had to
  keep polling the whole state block. Nothing is sent when nothing changed,
  and nothing at all while nobody is listening.
- **`hidden` and `pinned` can be set**, by `MDXM_HIDE` and `MDXM_PIN`, and the
  state is shared both ways: filing a device away over the pipe files it away
  in mdxmixer's own window, and doing it in that window reaches every
  subscriber. The two flags are now mutually exclusive, both ways round.
- **The failover watcher says what it is doing.** `MDXM_FOSTATE` carries its
  state, the device it believes the route is on, the device it last committed
  to, how many attempts have not stuck, the dwell actually in force and the
  reason — so a remote Failover tab can show what happened instead of
  inferring it.

### Routing and failover

- **A stored app assignment is applied when the app next plays.** mdxmixer now
  listens for new audio sessions instead of waiting for a device change, so an
  app launched while it sits in the tray lands on the channel it was assigned
  to.
- **The Routing tab shows drift**: what each app is assigned to, against what
  Windows is actually doing with it.
- **The failover hold is driven by AUDIODG.EXE itself.** When the Windows
  audio engine restarts, every endpoint reads absent at once; the watcher now
  recognises the cause rather than the symptom, stands down for the rebuild,
  and resumes as soon as the engine has been back and steady for a moment.

### The window

- **Channel rows have peak meters**, one per fader, each showing what *that*
  fader is passing rather than the channel's source — so a Personal fader at
  zero shows an empty bar while the Streaming fader beside it shows the signal
  going out.

### Fixed

- `build.ps1 -Install` installed nothing: the whole block was unreachable, so
  the flag was accepted, copied nothing and still reported success.

### For anyone writing against the pipe

`docs/ipc.md` gains `MDXM_TAB` and `MDXM_CAPTURE`, which were accepted and
undocumented, and a test now fails the build if an accepted verb is missing
from that document.

## v1.0.0 (2026-10-04)

First release.

mdxmixer began as a Bluetooth problem rather than a mixer problem: a drawer of
interchangeable earbuds that drop several times a day, Windows handing the
audio to the monitor speakers at whatever level they were last on, and
SteelSeries Sonar hanging when a headset disappeared suddenly — taking every
application's routing down with it, recovered by hand with a three-step
ritual. The first thing this program did was watch for the headset going away
and move the audio somewhere sensible before Windows could make a mess of it.
Everything below grew out of living with that.

It is one executable. No account, no launcher, no service, no installer
required; settings live in `mdxmixer.json` beside the exe, so the folder is
the installation.

### Two levels per channel

Every channel carries a **personal** level and a **streaming** level that move
independently — what you hear, and what everyone else hears. Mute one side and
the other is untouched.

The streaming half needs somewhere to go, and there are three places it can:
Sonar keeps both mixes itself; a **streaming cable** can be captured by a
recorder; or the **shared-memory ring** hands it to a program that wants the
audio rather than a device. With none of those configured the personal fader
still works, because that is just your output level.

### The mixer

- Channels from any endpoint. A render device is tapped by WASAPI loopback, so
  an output can be a channel source with no cable between them.
- 10-band parametric EQ per channel (RBJ peaking biquads), a soft limiter on
  each mix sum, and click-free gain ramps.
- Per-application routing: send one app to one channel, or straight to one
  endpoint, through the same undocumented policy API the Windows Settings
  "App volume and device preferences" page uses.
- Endpoint volumes alongside the channel faders, so the device's own level is
  reachable without going to the Windows slider.
- Live peak meters, and the device you are listening through pinned to the top
  under **In use**.
- Fader control as sliders or as step buttons, whichever suits.

### SteelSeries Sonar

Sonar's own channels — Aux, Media, Game, Chat, Mic and Master — appear as
ordinary rows with the same two levels. They are driven over Sonar's local
HTTP API because they cannot be driven any other way: a Sonar virtual endpoint
accepts `SetMasterVolumeLevelScalar`, returns success, and holds at 1.0.

Sonar can also be **disabled and re-enabled** from here, which is the first
two thirds of the recovery ritual that used to be done by mouse.

Its rows come and go with it. When Sonar is disabled or crashes its faders
disappear and return without a restart, and the mixer carries on either way.

### Telling identical headsets apart

Seven devices called some variation of `Headphones (11- WF-1000XM5)` is a list
nobody can read.

- **Short names** that follow the physical headset — across re-pairings, and
  across changing the Bluetooth adapter, because the name is keyed to the
  headset's own Bluetooth address rather than to the pairing.
- **Battery** and **last seen**, sortable, with the sort order remembered.
- Pin, hide, and reorder rows; disconnected devices are listed so they can be
  named and dated, which is the only way a device you are not currently using
  can be made identifiable.

### Failover

An ordered list of outputs you would accept. When the one you are on
disappears, the first present device down the list takes over.

Deliberately unhurried: a device must be steadily present for a few seconds
before it is trusted, there is a minimum dwell and a minimum gap, and a
commit is **forward only** — reconnecting the old headset does not drag the
audio back. A link that is flapping should not flap the audio with it.

The watcher is singular on purpose. MDropDX12 can set the rule but does not
enforce it; two processes moving endpoints is how a route gets flapped between
them.

### Battery overlay

A frameless, always-on-top readout of every connected headset's charge,
colour-coded green through red, click-through by default so it cannot get in
the way. Movable and resizable with the frame turned on, with the text scaled
to fit, snap-to-corner buttons, adjustable opacity, and a format string of
your own — `$sn:$b%` gives `Rg1:100%`.

One line per headset, because two showing at once usually means one of them is
not charging.

### Hotkeys

Volume up, volume down, mute, and show the window, bound to as many channels
at once as you like. Each binding carries **its own step size** with a global
default, because a coarse key for finding the range and a fine one for
settling are different keys. The window reports what Windows actually
answered — held, taken by another application, unbound — rather than what was
requested.

### A control surface other programs can drive

A line-oriented protocol on `\\.\pipe\mdxmixer`: read all state, subscribe to
changes, move faders, set the failover rule, drive Sonar, move the Windows
default output, route an application. Documented in full in
[docs/ipc.md](ipc.md).

`MDXM_PEAK` pushes every channel's and endpoint's level four times a second to
subscribers, so a remote list can be ordered by what is actually making a
noise — the one question nothing else in the state answers, because a channel
that starts blasting does not move its own fader. A peak of `-1` means *cannot
know* and is deliberately distinct from `0`, which means silence.

`MDXM_FEED` publishes the streaming mix into a shared-memory ring for a
visualiser, with no cable and no audio device involved. It is off until asked,
because when Sonar is working it is already mixing this audio and two live
sources of it work against each other. MDropDX12 3.3 and later reads it.

### Surviving a broken audio stack

This runs on a machine where the audio graph is routinely in pieces, and the
rule throughout is that a fault costs a reading, not the process.

- Structured-exception guards around the endpoint sweep, engine start, and
  every stream thread, after `AudioSes.dll` was measured faulting inside calls
  on endpoints that were dying mid-read.
- Recovery from **suspend and resume**. A render stream can stop being clocked
  without any call returning an error — a Bluetooth endpoint stays listed,
  stays active, keeps answering its volume, and simply stops. That is now
  detected by the stream itself, by a watchdog that checks whether frames are
  actually moving, and by handling the power-broadcast messages; the failover
  watcher is held while the device set settles, because its timers are
  wall-clock and a suspend expires all of them at once.
- The audio service and Sonar can both be restarted from here when they wedge.

### Packaging

`release.ps1` builds, runs the test suite, and produces both artifacts from
one staged payload, so they cannot drift:

- **Portable zip** — unzip and run.
- **Per-user MSI** — Start Menu entry, Apps & features listing, no
  administrator required (settings live beside the exe, and Program Files is
  not user-writable). Express, or Custom for the folder and whether it starts
  with Windows.

### Tests

A headless suite that touches no audio hardware covers device ordering, name
matching across re-pairings, the failover state machine, the protocol parser
and formatter, the ring buffer, the peak hold, the Sonar path builder and date
formatting. A separate `--audio` suite exercises the real graph through
cables, measuring frequency-selectively so music playing on the endpoints does
not break the assertions.
