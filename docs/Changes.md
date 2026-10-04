# Changes

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
