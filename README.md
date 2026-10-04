# mdxmixer

**Hear your games at a sane volume while everyone else hears them properly —
and keep your audio alive when your Bluetooth headset drops.**

A small Windows audio mixer. No account, no launcher, no background service:
one executable that keeps its settings in the folder you put it in.

---

## Where this came from

This started as a Bluetooth problem, not a mixer problem.

Picture the setup: a drawer of Bluetooth earbuds — five identical pairs of
WF-1000XM5s, an XM6, a Razer headset — swapped around through the day as
batteries run down. Bluetooth being Bluetooth, they drop. A lot.

On its own that would be a minor annoyance. The trouble is what the rest of
the machine does about it. When a Bluetooth headset disappears *suddenly*
rather than politely, Windows hands your audio to whatever it feels like,
usually the monitor speakers, at whatever volume they were last on. And if
you are running [SteelSeries Sonar](https://steelseries.com/gg/sonar) (free,
part of the SteelSeries GG suite), it gets worse: Sonar's engine would hang,
taking the routing for every app with it. The recovery was a three-step
ritual — disable Sonar, restart the Windows audio services, re-enable Sonar —
performed by hand, several times a day, often mid-session.

So the first thing this program did was not mix anything. It watched for the
headset going away and moved the audio somewhere sensible before Windows
could make a mess of it.

Everything else grew out of living with that:

- once something else owned the routing, **virtual cables** kept apps from
  ever pointing at a headset that might vanish;
- once audio was being mixed anyway, the **two-fader** idea became possible —
  a quiet level for your ears and a full level for everyone else;
- once there were seven identically-named headsets in the list, they needed
  **short names** and a **last seen** column;
- and once the batteries were the thing that decided which pair you reached
  for, a small **floating battery readout** earned its place on screen.

Sonar is still supported — its channels appear here as ordinary faders — but
it is no longer load-bearing. [docs/rollout.md](docs/rollout.md) walks through
moving off it one app at a time, in an order you can back out of at any step.

---

## The two faders

Every channel carries two levels, and they move independently.

![One channel, two faders: personal at 8 percent, streaming at 100 percent](docs/images/two-faders.svg)

If you stream, record, or share a call, you are running two mixes at once
whether or not your software admits it. Your ears want the game at a few
percent at one in the morning. Your audience wants it at a normal level. Turn
it down for yourself and the stream goes quiet; turn it up for the stream and
you flinch.

Here they are simply two numbers.

---

## How the audio actually gets there

![Apps render into virtual cables, mdxmixer captures the cables, and splits each channel into a quiet personal mix and a full-level streaming mix](docs/images/signal-flow.svg)

**Apps never point at your headset.** They point at a virtual audio cable —
a driver that pretends to be a sound card, so whatever an app plays into one
end, mdxmixer can record out of the other. You install a handful of these once
and then forget they exist.

[**VB-CABLE**](https://vb-audio.com/Cable/) is what this was built against and
it is free (donationware). The base download gives you one cable; the **A+B**
and **C+D** packs on the same page give you four more, which is enough for
three app channels plus a streaming output and a virtual microphone. Set every
cable to **24-bit, 48000 Hz** on both its playback and recording sides in
`mmsys.cpl` — mismatched rates are the one setup mistake that produces
confusing results.

That indirection is the whole trick. A cable never disconnects, never
re-enumerates and never changes its id. So when the headset drops, exactly one
thing in the chain has to move — the last hop, from mdxmixer to your ears —
and not one application notices. That is the difference between a headset
dropping and your whole machine's audio rearranging itself around it.

Out the other side there are two mixes:

- the **personal** mix goes to your headphones, at your level;
- the **streaming** mix goes to another cable, which your recorder or
  streaming software records, at full level.

Your microphone works the same way in reverse: mdxmixer takes the real mic,
applies gain and EQ, and publishes it on a cable your voice apps use as their
input.

---

## Failover: what happens when it drops

![The Devices tab: an armed failover list of short-named headsets with their batteries and when each was last seen](docs/images/tab-devices.png)

An ordered list of outputs you would accept. When the one you are on
disappears, the first *present* device down the list takes over.

Deliberately unhurried. A device has to be steadily present for a few seconds
before it is trusted, and once a move is committed, plugging the old headset
back in does **not** drag your audio back. A Bluetooth link that is flapping
should not flap your audio with it.

Two things on that screen worth pointing out:

- **Short names.** Seven headsets called some variation of `Headphones (11-
  WF-1000XM5)` is a list nobody can read. Name one `Bk1` and the name follows
  that physical headset — across re-pairings, and even across changing your
  Bluetooth adapter, because the name is keyed to the headset's own address
  rather than to the pairing.
- **Last seen**, sortable, so "which of these did I actually use yesterday"
  has an answer.

The line at the top — **Now on: Rg1** — is always the device your audio is
coming out of this second, not the one you configured.

---

## The battery overlay

![A small floating readout showing Rg1 colon 100 percent](docs/images/battery-overlay.png)

That is actual size. A frameless, always-on-top readout of every connected
headset's battery, colour-coded green through red, which answers "do I need
to swap pairs before this call" without opening anything.

It is **click-through by default**, so it cannot get in the way of what is
underneath it — but you can turn a frame on, **drag it anywhere**, resize it,
and the text scales to fit the box. Snap buttons put it in a display corner if
you would rather not aim. Set the opacity to taste, and write the text
yourself: `$sn:$b%` gives `Rg1:100%`, where `$sn` is the short name and `$b`
the battery. One line per headset, because two headsets showing at once
usually means one of them is not charging, and that is worth knowing.

---

## The mixer

![The Mixer tab: an in-use device at the top, then each channel with a personal and a streaming fader](docs/images/tab-mixer.png)

The device you are listening through sits at the top under **In use**, so you
never have to hunt for it. Below that, every channel with its `[P]` and `[S]`
pair.

Notice `Game [P]` at 23 and `Game [S]` at 100 — quiet in the room, normal on
the stream. And the little green bars: those are live meters, so you can see
which row is actually making a noise.

Right-click any row to move it, pin it to the top, hide it, or rename it.
Those two columns of `-10` / `+10` buttons are one of the two fader styles —
there is a checkbox on the Options tab to switch between step buttons and
sliders, depending on whether you would rather be precise or quick.

**If you have [SteelSeries Sonar](https://steelseries.com/gg/sonar)**, its own
channels — Aux, Media, Game, Chat, Mic and Master — show up here as ordinary
faders with the same two levels.
They have to be driven differently behind the scenes: Sonar's virtual devices
will accept a Windows volume change, report success, and quietly ignore it, so
mdxmixer asks Sonar directly instead.

---

## Install

Grab the latest from
[Releases](https://github.com/shanevbg/mdxmixer/releases):

**`mdxmixer-vX.Y.Z-portable.zip`** — unzip it anywhere and run
`mdxmixer.exe`. Settings live next to the exe, so the folder *is* the
installation: copy it to a stick, copy it to another machine, delete it to
uninstall.

**`mdxmixer-vX.Y.Z-x64.msi`** — a normal installer with a Start Menu entry and
an Apps-and-features listing. **No administrator needed** — it installs under
your own account, because mdxmixer keeps its settings beside itself and
Program Files is not somewhere you can write. Choose **Express** to get on
with it, or **Custom** to pick the folder and whether it starts when you sign
in.

### Free things you may want alongside it

| | | |
| --- | --- | --- |
| [**VB-CABLE**](https://vb-audio.com/Cable/) | donationware | The virtual cables. Needed for per-app channels; not needed if you only want control over the devices you already have. Take the A+B and C+D packs from the same page for five cables in total. |
| [**SteelSeries Sonar**](https://steelseries.com/gg/sonar) | free | Optional. If you already run it, its channels appear here as ordinary faders. You do not need it — this exists partly because of it — but nothing makes you remove it. |
| [**OBS Studio**](https://obsproject.com/) | free | Optional. The usual thing on the receiving end of the streaming mix. Anything that can record an audio input works. |

Neither of the first two is required to use mdxmixer as a plain volume mixer
for your real devices with a failover list and a battery overlay.

---

## First run

1. **Devices tab** → set **Personal output** to the headphones you listen on.
   Two of the choices at the top of that list are automatic, and the
   difference matters:
   - *Follow the Windows default output* — whatever Windows is using.
   - *Use failover list* — ignore Windows, take the first device from your own
     list that is actually there.
2. **Mixer tab** → set your personal level. If your hearing is anything like
   the author's this will look absurdly low, and that is correct.
3. **Options tab** → turn on the battery overlay, and set a hotkey or two.

Hotkeys can drive as many channels at once as you like, and each key gets its
own step size — a coarse key for finding the range and a fine one for settling
are different keys.

---

## Driving it from another program

MDropDX12 keeps its own mixer window but, when mdxmixer is running, reads and
writes **through** it rather than calling the Windows audio APIs itself. Two
programs both moving endpoints is how a route gets flapped between them, and
the failover watcher has to be the only one deciding.

Connect to `\\.\pipe\mdxmixer` and talk a line-oriented text protocol:
`MDXM_STATE` for everything, `MDXM_SUBSCRIBE|1` to be told when it changes,
`MDXM_FAILOVER` for the rule and what it currently sees. Every verb is in
**[docs/ipc.md](docs/ipc.md)**.

---

## Building it yourself

Visual Studio 2022 (v143) with the C++ workload. Nothing to fetch, no package
manager.

```text
.\build.ps1              # Debug
.\build.ps1 Release
.\build.ps1 Test
```

x64 only, so there is nothing to select. Binaries land in `bin\<Config>\`.

```text
.\release.ps1            # builds, runs the tests, writes the zip and the MSI
.\release.ps1 -ZipOnly   # skip the MSI, so WiX is not needed
```

The MSI needs WiX 5 — `dotnet tool install --global wix --version 5.*` and
`wix extension add -g WixToolset.UI.wixext/5.0.2`. Version 5 on purpose: 6 and
later require accepting a maintenance-fee licence, while 5 is free.

### Tests

```text
bin\Test\mdxmixer_test.exe            # headless, no audio hardware touched
bin\Test\mdxmixer_test.exe --audio    # cable-dependent integration tests
```

The headless suite runs anywhere: device ordering, name matching across
re-pairings, the failover state machine, the protocol parser, the ring buffer,
the peak meters. The `--audio` tests skip cleanly unless environment variables
name real endpoints (ids come from `mdxmixer --devices`):

| variable | meaning |
| --- | --- |
| `MDXM_TEST_RENDER` / `MDXM_TEST_CAPTURE` | channel cable pair (tone in / engine taps) |
| `MDXM_TEST_RENDER2` / `MDXM_TEST_CAPTURE2` | streaming cable pair (engine out / verification) |
| `MDXM_TEST_LOOPBACK=1` | capture ids equal render ids, tapped via WASAPI loopback — runs with no cable installed |
| `MDXM_TEST_PERSONAL` | personal-output endpoint for the graph tests |
| `MDXM_TEST_SOAK=1` | run the drift soak for the full 3 minutes |

Measurements are frequency-selective (a 440 Hz Goertzel bin), so music playing
on those endpoints does not break the assertions.

---

## Command line

Useful when something is wrong and you want to see what mdxmixer sees:

```text
mdxmixer                      the tray app (a second launch raises the first)
mdxmixer --levels             every endpoint: name, battery, last seen, volume
mdxmixer --devices            endpoint ids and names
mdxmixer --btinfo             Bluetooth containers, batteries, addresses
mdxmixer --sessions           what is playing, and whether per-app routing works
mdxmixer --sonarch            Sonar's channels and their two levels
mdxmixer --meter <id> 5       is Windows actually putting audio on this device?
mdxmixer --monitor <in> <out> passthrough: capture, cushion, render at unity
```

`--levels` and `--btinfo` are the two worth pasting into a bug report.

---

## Where things live

- **Settings:** `mdxmixer.json` beside the exe. Written atomically, with a
  `"complete": true` marker last, so a half-written file falls back to
  defaults rather than loading half a configuration.
- **Logs:** `log\mdxmixer.log` beside the exe.
- **Design and rationale:**
  [docs/specs/2026-09-22-mdxmixer-design.md](docs/specs/2026-09-22-mdxmixer-design.md).

The screenshots above are real, and regenerable: `MDXM_TAB` switches tabs and
`MDXM_CAPTURE` writes a PNG of a window, including the overlay, which is
otherwise unscreenshottable because it is frameless and click-through.

---

## Licence

CC-BY-NC 4.0 — see [LICENSE](LICENSE). Same terms and holder as MDropDX12,
which is what keeps the parts adapted from it clean.

Contributions are welcome; [CONTRIBUTING.md](CONTRIBUTING.md) explains the
snapshot-publishing shape of this repository.
