# Contributing to mdxmixer

Contributions are welcome. This file exists because the repository's shape is
unusual enough that you deserve to know what you are looking at before you
spend an evening on it.

## What this repository is

mdxmixer is developed in a private repository and **published here as a
snapshot, one commit per release.** So:

- The history here is one commit per version, not per change. The development
  repository's commit-by-commit history is not published.
- That history is **append-only**. Each release is parented on the one before
  it, so a commit you fork from stays reachable, `git diff` between releases
  works, and a pull request has a stable base.

A pull request opened here is read and, if it is taken, imported into the
development repository and published in the next snapshot — crediting you.
The next snapshot will not overwrite a merged contribution: the publishing
workflow refuses to force-push for exactly that reason, and a rejected push
is treated as a thing to deal with rather than a thing to overwrite.

## What mdxmixer is

A native Windows audio mixer: WASAPI, Win32, C++17, x64 only, no external
dependencies. It exists to replace SteelSeries Sonar and MDropDX12's built-in
mixer on one specific desk, which shapes nearly every decision in it.

Two ideas run through the whole program and are worth knowing before reading
any of it:

- **Every channel has two levels**, Personal and Streaming. What the user
  hears and what an audience or a recording receives are different numbers,
  and on the author's machine the personal side runs at 1–10% of the
  streaming side. Any feature that collapses "the volume" into one number is
  wrong here.
- **Audio must never stop.** The render callback is wrapped so a fault
  produces silence rather than a crash, device enumeration is guarded against
  a half-built audio graph, and failover moves the output when a device
  disappears. Several of the comments in the source are the measurements that
  established why a particular ordering is necessary; they are not decoration
  and are worth reading before changing the code they sit above.

## Building

```
.\build.ps1 Release      # bin\Release\mdxmixer.exe
.\build.ps1 Test         # bin\Test\mdxmixer_test.exe
.\bin\Test\mdxmixer_test.exe
```

MSBuild and the MSVC toolchain, x64. `build.ps1` finds Visual Studio itself.
There is nothing to install and nothing to fetch.

## What a good change looks like

**Tests for anything pure.** The suite is plain C++ with no framework to
learn — see `tests/`. Device ordering, name matching, the failover state
machine, the protocol parser, the ring buffer and the date formatting are all
tested without touching an audio device, and that is deliberate: the parts
that can be tested on a build machine are the parts that get tested.

**Comments that say why, not what.** The house style is to record the reason
a thing is the way it is, especially when it is not the obvious way. If you
fix something subtle, the measurement that proves it belongs in the source
next to the fix.

**No new dependencies.** This is a deliberate constraint, not an oversight.

**Leave the audio thread alone** unless the change is about audio. Nothing in
the render path may allocate, lock, or call anything that can block.

## Reporting a problem

Issues are welcome here. The most useful report says what you expected, what
happened, and what hardware was involved — Bluetooth headsets in particular,
because most of the hard problems in this program have been Bluetooth
problems wearing a different hat.

`mdxmixer --levels`, `--btinfo` and `--diag` print the state the program is
working from and are usually worth pasting in.

## Licence

CC-BY-NC 4.0 — see [LICENSE](LICENSE). By contributing you agree your
contribution is licensed under the same terms.
