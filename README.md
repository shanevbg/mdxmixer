# mdxmixer

**Hear your games at a sane volume while your stream hears them properly.**

mdxmixer is a small Windows audio mixer with one idea at its centre: *what you
hear* and *what everyone else hears* are two different numbers, and software
that pretends they are one number is the reason you keep reaching for the
volume knob.

It also knows your Bluetooth headset is going to drop, and moves your audio
somewhere else when it does.

---

## The problem it solves

If you stream, record, or just share a call, you are running two mixes at
once without a way to say so:

- **Your ears.** Sensitive hearing, late at night, headphones on — you want
  the game at a few percent.
- **Everyone else.** Your audience, your recording, your visualiser — they
  want it at a normal level.

Turn it down for yourself and the stream goes quiet. Turn it up for the
stream and you flinch. Every channel in mdxmixer therefore has **two faders**:

```text
Game   [P]   ████░░░░░░░░░░░░░░░░   8%     ← what you hear
Game   [S]   ████████████████████  100%    ← what goes out
```

Then there is the headset. Bluetooth drops, and when it does Windows hands
your audio to whatever it feels like — usually your monitor speakers, at
whatever volume they were last on. mdxmixer watches for it and moves you to
the next device on a list you chose, after it has been steadily present long
enough to be trusted.

---

## Install

Grab the latest from [Releases](https://github.com/shanevbg/mdxmixer/releases):

**`mdxmixer-vX.Y.Z-portable.zip`** — unzip it anywhere and run `mdxmixer.exe`.
Settings live in `mdxmixer.json` beside the exe, so the whole folder is the
installation: copy it to a stick, copy it to another machine, delete it to
uninstall.

**`mdxmixer-vX.Y.Z-x64.msi`** — a normal installer with a Start Menu entry and
an Apps-and-features listing. It offers **Express** or **Custom**; Custom lets
you choose the folder and whether mdxmixer starts when you sign in. **No
administrator rights needed** — it installs under your own account, because
mdxmixer keeps its settings next to itself and Program Files is not
user-writable.

You will also want **virtual audio cables** (the VB-CABLE family, or any
signed cable driver) if you want per-app channels rather than just controlling
the devices you already have. [docs/rollout.md](docs/rollout.md) walks through
doing that on a machine currently running SteelSeries Sonar, in an order you
can back out of at each step.

---

## First run

1. **Devices tab** → set **Personal output** to the headphones you listen on.
   There are two automatic choices at the top of that list, and the difference
   matters:
   - *Follow the Windows default output* — whatever Windows is using.
   - *Use failover list* — ignore Windows, take the first device from your own
     list that is actually present.
2. **Mixer tab** → the device you are listening through sits at the top under
   **In use**, so you never have to hunt for it. Set your personal level. If
   your hearing is like the author's, this will look absurdly low, and that is
   correct.
3. **Options tab** → turn on the **battery overlay** if you want your headset's
   charge floating on screen, and set a hotkey or two.

Right-click any row to move it, pin it, hide it, or rename it. With five pairs
of earbuds all called `WF-1000XM5`, renaming is not a luxury — and the name
you give follows the headset across re-pairings, even if you change Bluetooth
adapters.

---

## What it does

**Channels.** Group apps into a channel, give the channel a personal and a
streaming level, and put a 10-band EQ on it if you like. Any endpoint can be a
channel source — a render device is tapped by loopback, so "SteelSeries Sonar
– Gaming" becomes a channel with no cable to install.

**Sonar's own channels, if you have it.** Aux, Media, Game, Chat, Mic and
Master appear as ordinary channels with the same two faders. Sonar's virtual
endpoints cannot be driven through Windows at all — they accept a volume
change, report success, and stay at 1.0 — so mdxmixer asks Sonar directly.

**Failover.** An ordered list of devices you would accept. When the one you
are on disappears, the first present device on that list takes over, once it
has been steady for a few seconds. Deliberately slow: a Bluetooth link that is
flapping should not drag your audio around with it.

**A battery overlay.** A frameless always-on-top readout of every connected
headset, colour-coded green through red. Two headsets showing at once usually
means one of them is not charging, which is why it shows both rather than
picking one.

**Hotkeys.** Volume up, volume down, mute, show the window — bound to as many
channels at once as you like, each key with its own step size, because a
coarse key for finding the range and a fine one for settling are different
keys.

**It gets out of the way.** Tray icon, optional taskbar button, remembers
where you put the window and how big you made it.

---

## Driving it from another program

MDropDX12 keeps its own mixer window but, when mdxmixer is running, reads and
writes **through** it rather than calling the Windows audio APIs itself. That
indirection is the point: two programs both moving endpoints is how a route
gets flapped between them, and the failover watcher has to be singular.

Connect to `\\.\pipe\mdxmixer` and talk a line-oriented text protocol.
`MDXM_STATE` for everything, `MDXM_SUBSCRIBE|1` to be told when it changes,
`MDXM_FAILOVER` for the rule and what it currently sees. If you need the audio
itself, `MDXM_FEED|1` publishes the streaming mix into shared memory — off
until asked, because when Sonar is working it already mixes this audio and two
live sources of it work against each other.

Every verb is in **[docs/ipc.md](docs/ipc.md)**.

---

## Building it yourself

Visual Studio 2022 (v143) with the C++ workload. Nothing to fetch, nothing to
install, no package manager.

```text
.\build.ps1              # Debug
.\build.ps1 Release
.\build.ps1 Test
.\build.ps1 Debug Clean
```

x64 only, so there is nothing to select and no platform suffix anywhere.
Binaries land in `bin\<Config>\`, intermediates in `obj\<Config>\`, and both
are git-ignored. Compilation is parallel and the build prints the core count.

To produce the release artifacts:

```text
.\release.ps1                    # builds, runs the tests, writes dist\
.\release.ps1 -ZipOnly           # skip the MSI (no WiX needed)
.\release.ps1 -DryRun            # show the payload and stop
```

The MSI needs WiX 5 — `dotnet tool install --global wix --version 5.*` and
`wix extension add -g WixToolset.UI.wixext/5.0.2`. Version 5 on purpose: WiX 6
and later require accepting a maintenance-fee licence, while 5 is MS-RL and
free.

### Tests

```text
bin\Test\mdxmixer_test.exe            # headless, no audio hardware touched
bin\Test\mdxmixer_test.exe --audio    # cable-dependent integration tests
```

The headless suite is the one that runs everywhere: device ordering, name
matching across re-pairings, the failover state machine, the protocol parser,
the ring buffer, date formatting. The `--audio` tests skip cleanly unless
environment variables name real endpoints (ids come from `mdxmixer --devices`):

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
mdxmixer --feed 2             read the shared-memory audio feed for 2 seconds
mdxmixer --meter <id> 5       is Windows actually putting audio on this device?
mdxmixer --monitor <in> <out> passthrough: capture, cushion, render at unity
```

`--levels` and `--btinfo` are the two worth pasting into a bug report.

---

## Where things live

- **Settings:** `mdxmixer.json` beside the exe. Written atomically, with a
  `"complete": true` marker last, so a truncated file falls back to defaults
  rather than loading half a configuration.
- **Logs:** `log\mdxmixer.log` beside the exe; verbosity from `logLevel`.
- **Design and rationale:**
  [docs/specs/2026-09-22-mdxmixer-design.md](docs/specs/2026-09-22-mdxmixer-design.md).

---

## Licence

CC-BY-NC 4.0 — see [LICENSE](LICENSE). Same terms and holder as MDropDX12,
which is what keeps the parts adapted from it clean.

Contributions are welcome; [CONTRIBUTING.md](CONTRIBUTING.md) explains the
snapshot-publishing shape of this repository and what a good change looks
like here.
