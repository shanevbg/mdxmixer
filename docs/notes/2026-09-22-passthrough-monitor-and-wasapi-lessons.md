# Passthrough monitor + WASAPI engine — lessons from the MDropDX12 prototype

2026-09-22. A working prototype of an audio monitor-out was built inside
MDropDX12, then pruned: the audio engine belongs here in mdxmixer, not bolted
onto the visualizer. This note carries what that prototype settled so mdxmixer's
engine can be implemented without re-learning it. The prototype's pure core (a
ring, packet extraction, format conversion, feedback predicates) was fully
unit-tested; the shapes below are known-good.

## What a "passthrough monitor" actually is

Capture an input at full level, and play it back out to a chosen output device.
Nothing else. Two independent consumers of the same captured audio:

- a **meter/visualizer** tap that reads it at full level (an FFT, in MDropDX12's
  case), and
- a **monitor render** that plays it to the user's headphones.

The volume the user hears is set by **the output device's own volume control**,
NOT by a gain multiply in code. This was the decisive correction: a passthrough
renders at **unity**, and attenuation is the OS/device volume like anything else.
Because the meter taps the *input* and the monitor renders to a *different*
output device, turning the output device down lowers what you hear without
touching the meter — the whole point ("engine sees 100%, I listen at 2%") falls
out for free, with no code-side gain.

Do NOT bake a fixed low gain into the render. The prototype defaulted to 2% in
code and it was wrong twice over: it's redundant with the device volume, and at
unity it exposed a latent bug (below).

## The engine shape (capture → ring → render)

```
input device ─capture─▶ ring (interleaved float) ─▶ resample/convert ─▶ output device
                  │                                                      (unity gain)
                  └─▶ meter/FFT (full level)
```

- **Capture**: WASAPI shared-mode capture (loopback of a render endpoint, or a
  capture endpoint). Each packet is decoded to interleaved **stereo float** at
  the source rate, full level. Mono duplicates to both channels; >2 channels
  keep the first two.
- **Ring** between capture and render threads: single-producer / single-consumer.
  On overflow, **drop oldest** (audio must never stall the capture). On underflow,
  **return zeros** (silence, not a stall). Count drops and underruns — they are
  the drift diagnostic.
- **Render**: WASAPI shared-mode, **event-driven** (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`
  + `SetEventHandle`). Each event: `GetCurrentPadding`, write `bufFrames - padding`
  frames, pull from the ring, convert to the device mix format, `ReleaseBuffer`.

### Format handling (measured, not assumed)

- Parse the output device's `GetMixFormat`: it is almost always **32-bit IEEE
  float**. Handle `WAVE_FORMAT_IEEE_FLOAT`, and `WAVE_FORMAT_EXTENSIBLE` whose
  `SubFormat` is `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`; also 16-bit PCM. Anything
  else: refuse to start rather than write garbage bytes.
- **Resample only when the rates differ.** If capture rate == output rate (the
  common case — both 48 kHz), do a straight copy; a linear interpolator with a
  fractional-position `carry` across blocks is fine for the mismatch case and
  sounds clean. Same-rate through the interpolator is exactly a copy (fraction 0),
  so one path covers both, but confirm same-rate is bit-identical.

## Why the prototype "sounded horrible" — the two bugs that matter most

These are the traps to get right from the start in mdxmixer. Both were found by
ear at unity gain; neither showed at 2%.

### 1. No pre-fill cushion → constant underrun crackle

If the render pulls from the ring as fast as the capture fills it, the ring
hovers near empty and any jitter underruns → a continuous crackle. **Pre-fill a
latency cushion before rendering audio**: on start, output silence until the ring
holds a target depth (~50–100 ms), then render normally. Monitoring music
tolerates 100 ms of latency easily, and the cushion makes the whole thing robust
against jitter and small clock drift without any adaptive resampling. This is the
single most important thing the prototype was missing.

### 2. A level-based feedback guard false-mutes at unity gain

The prototype had a "runaway detector" that muted after ~0.5 s of near-full-scale
output, intended to stop a feedback howl. At 2% gain it never fired; at unity,
**loud music legitimately sits near full scale**, so it muted loud passages —
cutting in and out. **Do not detect feedback by output level.** Prevent feedback
structurally instead:

- The monitor output device must never be the device the capture loops back from
  (rendering into the captured device is the feedback path). Enforce it two ways:
  the UI **excludes the input device from the output picker**, and the engine
  **refuses to start** when `outputDeviceId == capturedDeviceId`
  (case-insensitive; an empty/unknown id never matches, so it can't false-block).

That device-exclusion is the correct and complete feedback guard. The level
detector was a redundant backstop that became actively harmful.

## Other things worth carrying over

- **Clock drift** between two "48 kHz" devices is real (each has its own crystal).
  A generous ring (~200 ms) plus the pre-fill cushion absorbs minutes of it; the
  drop/underrun counters make it observable.
- **Feed the meter the raw packet before any monitor gain**, so the visualizer is
  independent of the listening level by construction.
- **Threading**: capture and render each on their own thread; the ring is the
  only shared state. Never block the capture thread — a slow/hung render must not
  stall capture (the ring's drop-oldest guarantees this).
- **Latency is cheap here.** For passive monitoring (not live perform/tracking),
  favor a bigger cushion and rock-solid playback over minimum latency.

## Scope discipline (the meta-lesson)

The request was small — "a passthrough monitor" — and the prototype over-grew
into a plan document, IPC verbs, hotkeys, and a multi-file subsystem before the
basic thing had been heard working. In mdxmixer, build the minimal passthrough
first (capture → cushioned ring → unity render, output device volume for level),
get it sounding right, and only then add control surfaces. The mdxmixer engine
already needs this exact capture→mix→render backbone (see the design spec), so
the passthrough is its first, smallest instance — not a separate feature.
