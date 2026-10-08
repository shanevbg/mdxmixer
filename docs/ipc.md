# The mdxmixer control surface

How another program drives mdxmixer. Written for MDropDX12, which keeps its
own mixer window but, when mdxmixer is running, reads and writes **through**
it rather than calling the Windows audio APIs itself.

That indirection is the point rather than a convenience. Two programs both
moving endpoints is how a route gets flapped between them, and the failover
watcher in particular has to be singular: one process deciding, one timer,
one dwell. The rule can be *set* from anywhere; it is *enforced* here.

---

## 1. Transport

| | |
|---|---|
| Pipe | `\\.\pipe\mdxmixer` |
| Mode | byte stream, duplex |
| Encoding | UTF-16LE |
| Framing | each record ends with a NUL (`\0`) |
| Protocol version | `1` — ask with `MDXM_PING` |

One connection may carry many requests. The server replies to each request
with one or more records; a multi-record reply is wrapped in `MDXM_BEGIN` …
`MDXM_END` so a client knows when it has the lot. Single-record replies
(`MDXM_OK`, `MDXM_ERR`, a channel echo) are **not** wrapped.

Every request is handled on mdxmixer's UI thread. Pipe threads marshal onto
it with a 5-second `SendMessageTimeout`, so a wedged UI returns
`MDXM_ERR|msg=busy` rather than parking the caller forever.

### Record grammar

```
VERB|positional|positional|key=value|key=value
```

- The first token is the verb. `VERB=value` is also accepted, and that value
  becomes the first positional field — `MDXM_SET=game|personal|0.5` and
  `MDXM_SET|game|personal|0.5` are the same request.
- Fields without `=` are **positional**, in order.
- Fields with `=` are **keyed**. Keys may repeat, and repeats keep their
  order — that is what lets `MDXM_FAILOVER_LIST` carry an ordered list.
- There is no escaping. A value cannot contain `|` or NUL. Device names and
  endpoint ids never do.

### Errors

`MDXM_ERR|msg=<human-readable reason>`. A malformed request is always an
error reply, never a silent no-op.

---

## 2. Reading state

### `MDXM_PING`
→ `MDXM_PONG|version=1`

The handshake. A client should check the version before assuming any field
below exists.

### `MDXM_STATE`
→ `MDXM_BEGIN`, then every record below, then `MDXM_END`.

**`MDXM_CHAN`** — one per mixer channel.

| key | meaning |
|---|---|
| `id` | channel id. `sonar:<key>` is one of Sonar's own (see §5) |
| `name` | display name |
| `health` | `ok` / `bad` |
| `pvol` `pmute` | the Personal fader — what the user hears |
| `svol` `smute` | the Streaming fader — what goes out |
| `eq` | `1` when the channel's EQ is enabled |
| `peak` | what the channel's **source** is carrying, `0..1`, or `-1` — see §2.1 |
| `idle` | `1` when the channel is not capturing because **nothing is pulling the mix** |

> **`idle=1` is not a fault.** mdxmixer captures on demand: with no personal
> render, no streaming cable and no feed subscriber, nothing is consuming the
> mix, so no cable is opened and no ring is run. Such a channel reports
> `health=ok` — its cable has not been touched, so nothing is known to be wrong
> with it — and `peak=-1`, because nothing is being measured. It starts
> capturing again, from an empty ring, the moment anything listens. A client
> showing channel state should treat `idle=1` as "resting", never as "broken".

**`MDXM_ROUTE|id=personal|device=<endpointId>`** — where a route is actually
playing right now. Read this rather than the configured output: failover may
have moved it.

**`MDXM_DEV`** — the raw endpoint list: `id`, `name`, `flow`, `active`.

**`MDXM_DEVLVL`** — the endpoint list a UI can be drawn from without
recomputing anything:

| key | meaning |
|---|---|
| `id` `name` | endpoint id, and what Windows calls it |
| `alias` | **our** name for it, or the Windows name when none is set |
| `flow` | `render` / `capture` |
| `default` | `1` if it is the system default for its flow |
| `vol` `mute` | the endpoint's own Windows volume |
| `active` | `1` when the endpoint is usable **now** |
| `battery` | percent, or `-1` when the device reports none |
| `seen` | `YYYY-MM-DD HH:MM` local, or empty — see the note below |
| `handsfree` | `1` for the mono hands-free twin of a headset |
| `hidden` `pinned` | how the user has filed it |
| `container` | ContainerId: the same physical device, across its endpoints |
| `bt` | the headset's Bluetooth address — see §6 |
| `peak` | what is **flowing** on the endpoint, `0..1`, or `-1` — see §2.1 |

> **`active`, not `present`.** mdxmixer's internal `present` flag means
> *paired*, which is true of a headset switched off in a drawer. Anything
> asking "can I use this device" wants `active`.

> **`seen` is the sortable absolute form on purpose.** Rendering it as "Today
> 16:07" is the client's job. A pre-formatted label sorts wrongly — "Today"
> falls below "Ystrdy" and both above every real date — and a client in
> another timezone could not undo the substitution.

> **A battery figure beside an inactive device is stale.** Windows keeps the
> last percentage it was told after a disconnect; one set here has read 1%
> for months sitting in its case. `MDXM_DEVLVL` reports it anyway because it
> is a reasonable thing to *sort* by; do not display it as current fact.
> `MDXM_FAILOVER` already applies that rule for you (§4).

### `MDXM_DIAG`
→ framed. `MDXM_RING` per ring buffer, `MDXM_DIAGDEV` with the live personal
device and whether it is on a fallback, and `MDXM_DIAGMIX` with the latency.
For troubleshooting, not for driving a UI.

```text
MDXM_RING|id=sonar|depth=840|cap=24000|drops=0|underruns=0|speed=1.0000
```

| key | meaning |
|---|---|
| `depth` | frames currently in the ring — **read it against `cap`, never alone** |
| `cap` | the ring's capacity, 500 ms at the mix rate; it bounds the worst a stall can leave behind |
| `drops` | frames discarded because the ring was full — the producer outran the consumer |
| `underruns` | times the mix found the ring empty |
| `speed` | the varispeed trim: `> 1` draining, `< 1` refilling, exactly `1.0000` left alone |

> **A depth means nothing on its own.** A 1.99-second backlog was once
> diagnosed from `depth=95520` only because the ring was known from the source
> to hold 96000 frames; the same figure against a larger ring is an ordinary
> fill. Sampling twice a minute apart is what proves a backlog is *stuck*.

> **`depth` and `speed` are not sampled at the same instant, so do not check
> one against the other.** `depth` is read when you ask; `speed` is what the
> engine's one-second tick last decided, from the depth as it was then. The
> depth moves by a whole render pull between callbacks — 480 frames is normal —
> so a line reading `depth=1671|speed=0.9982` is perfectly consistent: the trim
> saw about 1157 a moment earlier and is topping the ring up. Neither figure is
> stale; they are answers to "how deep is it *now*" and "what is being done
> about it".

`speed` is published because the correction is inaudible by design — a few
hundredths of a percent for ordinary clock drift, up to ±5% at the extremes —
and a silent change to playback speed that cannot be observed from outside the
process is indistinguishable from a bug. Anything other than `1.0000` means the
engine has noticed and is already giving the latency back; the `stream` and
`mic` rings always report `1.0000`, because only channel captures have a second
hardware clock to drift against.

```text
MDXM_DIAGMIX|rate=48000|cushion=4800|cushionms=100|window=2880|peak=9600
```

| key | meaning |
|---|---|
| `rate` | the mix rate, to read the frame counts with |
| `cushion` | frames the mix waits for before draining a channel — **this is the latency** |
| `cushionms` | the same figure in milliseconds, for convenience |
| `window` | the largest pull in the **last 60 seconds**, which is what sizes the cushion |
| `peak` | the largest pull ever seen in this engine run; sizes nothing |

The cushion adapts to the biggest buffer the render has asked for, because a
pull larger than the cushion is an underrun. It is sized from the **rolling**
maximum: a monotonic high-water mark meant one oversized pull — a Bluetooth
hiccup, a device change, a stall — raised the latency for the rest of the
session with no way back down short of a restart. `window` falling below `peak`
is the normal, healthy picture.

---

## 2.1 `peak` — finding the row that is making the noise

`MDXM_CHAN` and `MDXM_DEVLVL` both carry a `peak`. It answers one question:
**which of these is making sound right now.**

That question has no other answer in the state. A channel that suddenly starts
blasting does not move its own fader, so nothing else in `MDXM_CHAN`
distinguishes it from the twenty-four quiet channels beside it. Sorting a list
by `peak` is the intended use; opening a list *on* the row that is currently
audible is the other.

### Read `-1` before you read anything else

```text
peak=-1     nobody can say
peak=0      there is a reading, and it is silence
```

These are different answers and the difference is the whole point. An
unhealthy channel has no capture to meter; an endpoint Windows has parked as
unplugged has no meter to activate. Reporting either as `0` would look
authoritative and sort it among the sources that are genuinely quiet. Treat
`-1` as *absent*, exactly as you already do for `battery`.

`-1` also appears when the mix itself has stopped — no render device at all,
so nothing is being pulled. A frozen meter is worse than a blank one, because
it points at the wrong row.

### It is a peak **hold**, not an instantaneous level

The value is the highest seen in the last **1.5 seconds**, then it releases.

An instantaneous read is useless here. The chain from this pipe to a phone
screen is mdxmixer → MDropDX12 → MDR_Android, and a transient is long gone by
the time any of that has redrawn; the row that blasted would look exactly like
the ones that did not. 1.5 s is long enough to survive that trip and short
enough that the meter still tracks the music rather than describing the recent
past — if it held for five seconds, two channels that took turns being loud
would both read loud and the ordering would say nothing.

A steady signal does not expire and re-latch, so a list ordered by `peak` does
not flicker between redraws.

### On a channel it is measured *before* the faders

`MDXM_CHAN|peak` is the channel's **source**, after its EQ and before both
gain stages. Three consequences:

- A channel muted on one side still reports its peak. "What is making sound"
  stays true of audio you have chosen not to hear.
- The number does not move when a fader does. It is a property of the signal,
  not of the mix.
- It is one number, not two. A channel has one source and two gains, so a
  post-fader peak would have to be a personal one and a streaming one.

`MDXM_DEVLVL|peak` is the opposite end: what Windows is actually putting on
that endpoint, after everything. Set against `vol`, which is only where the
slider sits, it separates *nothing is being sent here* from *something is
being sent and you cannot hear it*.

> **Do not compare a peak on one row against a peak on a different kind of
> row.** Measured here on 2026-10-04 with one radio stream playing: its Sonar
> source endpoint read `0.4457` while the headphones it was being mixed down
> to read `0.0027`, because the personal mix on this machine runs at a few
> percent. Both are correct and they are not on the same scale. Order channels
> against channels, and endpoints against endpoints.

### Sonar's channels carry a real peak

A `sonar:` channel's peak comes from its own virtual endpoint —
`sonar:aux` from "SteelSeries Sonar - Aux", `sonar:game` from "… - Gaming",
`sonar:chatCapture` from the *capture* side of "… - Microphone".

This is worth knowing because Sonar's write path famously lies: a Sonar
virtual endpoint accepts `SetMasterVolumeLevelScalar`, returns success, and
stays at 1.0, which is the whole reason §5 exists. The **read** path does not.
Measured on 2026-10-04 with a radio stream playing to Aux: Aux read `0.4457`
and Stream `0.3050` then `0.3608` across successive reads, both tracking the
music, while Media and Gaming — which had nothing routed to them — read a flat
`0.0000`.

`sonar:masters` always reports `-1`, and that is not a gap. Sonar's master is
an output rather than an input, and in streamer mode its two halves land on two
different devices — monitoring on the physical headphones, streaming on the
Stream endpoint — so no single number speaks for it.

### Cost

Nothing is started to produce this. Endpoint peaks come from the device sweep
that already runs to build `MDXM_DEVLVL`; channel peaks are a maximum taken
over a block the mix loop is already reading twice to sum. There is no metering
timer to turn on, so there is nothing to subscribe to and nothing to switch
off — unlike the audio feed in §8, which genuinely is off until asked.

---

## 3. Writing

All of these reply `MDXM_OK`, or a channel echo where noted, or `MDXM_ERR`.

| Request | Effect |
|---|---|
| `MDXM_SET\|<ch>\|<personal\|streaming>\|<0..1>` | move a fader → echoes `MDXM_CHAN` |
| `MDXM_MUTE\|<ch>\|<personal\|streaming>\|<0\|1>` | mute a fader → echoes `MDXM_CHAN` |
| `MDXM_EQ_SET\|<ch>\|<band>\|<freq>\|<gain>\|<q>` | one EQ band → echoes `MDXM_CHAN` |
| `MDXM_EQ_ENABLE\|<ch>\|<0\|1>` | channel EQ on/off → echoes `MDXM_CHAN` |
| `MDXM_DEVVOL\|<endpointId>\|<0..1>` | an endpoint's own Windows volume |
| `MDXM_DEVMUTE\|<endpointId>\|<0\|1>` | an endpoint's own mute |
| `MDXM_NAME\|<endpointId>\|<alias>` | our name for a device; empty clears it |
| `MDXM_HIDE\|<endpointId>\|<0\|1>` | file a device out of the list → echoes `MDXM_DEVLVL` |
| `MDXM_PIN\|<endpointId>\|<0\|1>` | hold a device at the top → echoes `MDXM_DEVLVL` |
| `MDXM_ROUTE_SET\|personal\|<endpointId>` | bind the personal output |
| `MDXM_DEFAULT\|<endpointId>` | move the **Windows** default output |
| `MDXM_ASSIGN\|<exePath>\|<ch or ->` | send one app to a channel |
| `MDXM_APPROUTE\|<exePath>\|<endpointId>` | send one app straight to an endpoint |
| `MDXM_CUSHION\|<ms>` | the mix cushion floor — **the latency** → echoes `MDXM_CUSHIONSTATE` |
| `MDXM_SHOW` / `MDXM_HOTKEYS` | raise a window |
| `MDXM_TAB\|<mixer\|routing\|eq\|devices\|vban\|options>` | bring one tab of the main window to the front |
| `MDXM_CAPTURE\|<pngPath>\|[main\|hotkeys\|overlay]` | write a PNG of one window as it stands |
| `MDXM_EXIT` | shut down through the real exit path |
| `MDXM_VBAN\|<key>=<value>\|…` | the network stream — see §11 → echoes `MDXM_VBANSTATE` |
| `MDXM_VBANPEERS` | who is subscribed to the network stream → `MDXM_VBANPEER` rows |
| `MDXM_AUTH\|pin=\|device=\|name=` | **VBAN-only**; on this pipe it answers `MDXM_ERR` — see §11 |

Writes are **optimistic**: the echo carries the value you asked for. The
subscription (§7) carries what actually happened.

`MDXM_ROUTE_SET` accepts two values that are not endpoint ids:

- `` (empty) — follow the Windows default output.
- `\x01followFailover` — do not bind at all; let the failover list decide,
  every time. A commit under this mode deliberately does not write the
  chosen device back, which would silently leave the mode.

`MDXM_DEFAULT` is a different thing from `MDXM_ROUTE_SET`: it moves the
system default that every *other* application follows. It can fail silently
in one known way — SteelSeries Sonar re-asserts itself as the default within
moments — so the reply reports what the default actually is afterwards.

### `MDXM_CUSHION` — the latency, live

```
MDXM_CUSHION                      → MDXM_CUSHIONSTATE|ms=30|headroom=50|flat=10
MDXM_CUSHION|12                   → the floor, in ms (clamped 5..200)
MDXM_CUSHION|flat=0               → one adaptive term, the others untouched
MDXM_CUSHION|8|headroom=100|flat=3  → all three at once
```

The cushion is what the mix waits for before it starts draining a channel, so
it **is** the latency. It is computed as:

```text
cushion = max( floor , pull + pull × headroom% + flat )
```

where `pull` is the largest the render has asked for in the last 60 seconds
(see `MDXM_DIAG`). So the floor is only a floor — once it is out of the way the
adaptive terms govern, and those are what actually set the latency on a healthy
link. Measured here: a WF-1000XM5 pulls 480 frames (10 ms), so the defaults of
50% and 10 ms give a 25 ms cushion no matter how low the floor goes.

| term | what it defends against |
|---|---|
| `headroom` | a pull that grows — proportional slack |
| `flat` | scheduling jitter, which does not scale with buffer size |
| floor (`ms`) | nothing in particular; a hard minimum you choose |

**The reply says what was applied, not what you asked for.** That matters for
the only sensible way to use this: lower it, watch `underruns` in `MDXM_DIAG`,
and keep going until they appear.

It is a verb rather than a config-file setting alone because finding the right
value is an experiment, and a restart would reset the very counter being
watched. The value is persisted as well, so a good number survives.

> **Setting it re-cushions the running channels.** Each one clears its ring and
> refills, so there is a gap of the new cushion's length — tens of
> milliseconds. That is the honest price of changing the latency of a graph
> that is already running, and it is why this is a deliberate command rather
> than something applied on a timer.

### `MDXM_HIDE` and `MDXM_PIN` — how the user has filed a device

`MDXM_DEVLVL` reports `hidden` and `pinned`; these set them. Windows publishes
far more endpoints than anyone mixes with — five headsets' hands-free twins,
every virtual Sonar device — so a list that cannot be cut down is a list
nobody reads.

```
MDXM_HIDE|{0.0.0.00000000}.{cc8e…}|1   → MDXM_DEVLVL|…|hidden=1|pinned=0|…
MDXM_PIN|{0.0.0.00000000}.{cc8e…}|1    → MDXM_DEVLVL|…|hidden=0|pinned=1|…
```

- **They echo the row**, not `MDXM_OK`, so the new state needs no second round
  trip — the way `MDXM_SET` echoes `MDXM_CHAN`.
- **The flags are mutually exclusive.** A device cannot be held at the top of
  a list and absent from it, so setting either to `1` clears the other.
  Clearing one says nothing about the other: unpinning a visible device does
  not hide it.
- **The id is enough.** The ContainerId and Windows name that let the flag
  survive the device returning under a new endpoint id are looked up here, as
  `MDXM_NAME` does it.
- **It goes both ways.** The same store backs mdxmixer's own right-click Hide
  and Pin, so a flag set over this pipe files the device away in mdxmixer's
  window immediately, and one set in that window reaches every subscriber as
  an `MDXM_DEVLVL` push (§7). There is one copy of this state and both
  surfaces drive it through one path.
- **A device in use is not refused.** Hiding the endpoint the personal mix is
  playing to does nothing to the audio — it is a view. (The Mixer tab hoists
  the live personal route to the top regardless, because that row is the
  answer to "where am I listening".) The one write mdxmixer does refuse is
  `MDXM_MUTE` on `sonar:masters`, and that is because it would do harm.

### `MDXM_TAB` — the counterpart of `MDXM_SHOW`

`MDXM_SHOW` raises the window; this says *which part of it* to raise.

```
MDXM_TAB|devices        → MDXM_OK
MDXM_TAB|3              → MDXM_ERR|msg=unknown tab: 3
```

**By name, never by index.** An index is a thing that silently means a
different tab the day one is inserted — which has now happened once: `vban` was
added before `options`. The six names are `mixer`, `routing`, `eq`, `devices`,
`vban` and `options`, matched case-insensitively; anything else is an
error rather than a guess. It does not raise the window on its own — send
`MDXM_SHOW` as well if the window may be in the tray.

### `MDXM_CAPTURE` — a picture of a window, from inside the process

```
MDXM_CAPTURE|C:\shots\mixer.png                  → MDXM_OK
MDXM_CAPTURE|C:\shots\overlay.png|overlay        → MDXM_OK
MDXM_CAPTURE|C:\shots\x.png|desktop              → MDXM_ERR
```

The second field names the window and defaults to `main`:

| value | window |
|---|---|
| `main` | the mixer window |
| `hotkeys` | the hotkey assignment window, when it is open |
| `overlay` | the battery readout |

It exists so documentation screenshots can be **regenerated** rather than
grabbed by hand once and left to rot as the window changes — the same reason
MDropDX12 carries `CAPTURE_WINDOW=`, and worth repeating here: a capture taken
from *inside* the process renders owner-drawn controls, where `PrintWindow`
from another process does not. mdxmixer's faders, meters and rows are all
owner-drawn. `overlay` has no other route at all: it is frameless,
click-through and always on top, so no window picker can select it and the
only alternative is a full-desktop grab that publishes whatever else is on
screen.

**This verb creates a file on disk at a path you choose**, so its constraints
are worth stating rather than discovering:

- The path is used exactly as given. Pass an **absolute** path — a relative
  one resolves against mdxmixer's working directory, which is wherever it
  happened to be started from.
- The **directory must already exist**. Nothing is created for you; a bad
  directory is `MDXM_ERR`, not a silent miss.
- An existing file is **overwritten** without asking.
- It is written with mdxmixer's own privileges, as the user running it.
- The **client area** is captured, not the frame, and at the window's current
  size. A window that is not open — `hotkeys` when the window is closed —
  is an error rather than an empty image.
- The window need **not** be in front. It does have to exist: `main` is
  created at startup, so it is always capturable, in the tray or not.

---

## 4. Failover

The rule lives in mdxmixer's config and is enforced by its watcher. A
front-end sets it and reads it; it does not run its own.

### `MDXM_FAILOVER`
→ framed, and carries **both** the rule and what it currently sees, because a
UI drawing this list needs both and two round trips could disagree.

```
MDXM_FO|armed=1|stability=3|dwell=10|gap=5
MDXM_FOENTRY|i=0|id={0.0.0...}|name=Headphones (WF-1000XM6)|alias=XM6
            |present=1|battery=62|seen=2026-09-26 05:04|known=1
```

| key | meaning |
|---|---|
| `armed` | the rule is allowed to commit |
| `stability` | seconds a replacement must be steady before it wins |
| `dwell` | least seconds between two switches of **this** route |
| `gap` | least seconds between **any** two endpoint reassignments |
| `i` | position in the list — **this is the preference order** |
| `id` `name` | the entry as stored; either may be empty |
| `alias` | our name for it, or the stored name when no device matches |
| `present` | `1` when a matching device is active right now |
| `battery` | only when `present=1`; `-1` otherwise, by design |
| `seen` | when that device was last here, absolute form |
| `known` | `0` when no device of this description exists on the machine |

The first entry that is `present` wins. A commit is permanent: reconnecting
the old device does not move the route back.

### `MDXM_FOSTATE` — what the watcher is actually doing

The same reply carries one of these. The rule above says what *should*
happen; this says what *is* happening, so a front-end showing the rule need
not infer it from the device list.

```
MDXM_FOSTATE|route=personal|state=searching|current={0.0.0…}|target={0.0.0…}
            |attempts=3|dwell=40000|since=12000|hold=0|audiodg=34552|restarts=2
            |reason=attempt 3 did not stick; waiting 40 s before retrying
```

| key | meaning |
|---|---|
| `route` | `personal` — the one route mdxmixer owns today |
| `state` | `idle` / `searching` / `arming` |
| `current` | the endpoint the state machine believes the route is on |
| `target` | the last endpoint it committed to |
| `attempts` | consecutive commits to the **same** target; `0` or `1` is healthy |
| `dwell` | ms in force for this route, **after** any back-off |
| `since` | ms since the last commit, `0` if there has not been one |
| `hold` | ms of hold remaining; `0` when not held |
| `audiodg` | AUDIODG.EXE's process id, `0` when it is not running |
| `restarts` | how many times it has restarted since mdxmixer started |
| `reason` | free text, and **last** in the record for that reason |

Three of these exist because of one outage (MDropDX12 #410), and they are
what to read when a route will not settle:

- **`current` disagreeing with `target` is the fault signature.** It means
  the move was made and something re-baselined the route off it before the
  next tick, which is how a route comes to re-commit the same move for ever.
- **`attempts` above 1 means the move is not sticking.** The dwell doubles
  per consecutive attempt — 10 s, 20, 40, 80, capped — so `dwell` is what is
  in force now rather than the configured value.
- **`reason` is the useful output when nothing is happening.** For hours the
  only readable thing said `idle` with no reason between attempts; it now
  names the back-off explicitly.

> **`hold` with a fresh `audiodg` pid is not a fault.** When AUDIODG restarts
> every endpoint reads absent at once, so the watcher deliberately stands down
> rather than moving routes onto devices that cannot accept them — that is the
> churn that took the audio engine down in the first place. The hold is a
> **ceiling**, not a duration: it lifts as soon as AUDIODG has been back and
> unchanged for a few seconds. Killed cleanly it is back in under a second;
> about a minute after Sonar's APO faults it. Both measured here.

`gap` exists separately from `dwell` because Sonar mishandles a personal
stream endpoint that changes too quickly, and `Sonar.APO.dll` runs inside
`AUDIODG.EXE` — faulting it takes every audio stream on the machine down.

### `MDXM_FAILOVER_SET|armed=<0|1>|stability=<s>|dwell=<s>|gap=<s>`

Any subset; whatever is absent is left alone. A call that sets nothing is an
error, not a no-op. Values are clamped (`stability` 1–120, the others 1–600).

### `MDXM_FAILOVER_LIST|dev=<id>~<name>|dev=<id>~<name>|…`

Replaces the whole ordered allowlist. Sending no `dev=` fields empties it,
which is legitimate — an armed rule with nothing allowed simply never
commits.

Each value is `<endpointId>~<windowsName>`, either half optional:

- **id and name** — the normal case; the id is exact and the name
  re-associates the device if it comes back under a new id.
- **name only** (`dev=~Headphones (2- WF-1000XM5)`) — the only way to list a
  device that is switched off right now, because it has no endpoint id to
  give. This is common: a pair of Bluetooth headphones is off far more often
  than on.

The list is replaced wholesale rather than edited with add/remove/move
because **the order is the rule**. A sequence of incremental edits has
intermediate states that are each a different rule, and a front-end that
died halfway through would leave one of them live.

---

## 5. Sonar's channels

Sonar's own channels appear in `MDXM_CHAN` with ids `sonar:masters`,
`sonar:game`, `sonar:chatRender`, `sonar:chatCapture`, `sonar:media`,
`sonar:aux`. `MDXM_SET` and `MDXM_MUTE` work on them exactly as on a native
channel; mdxmixer forwards to Sonar's local HTTP API.

They exist because Sonar's virtual endpoints **cannot** be driven through
Windows: `SteelSeries Sonar - Aux` accepts `SetMasterVolumeLevelScalar`,
returns success, and holds its level at 1.0. Only Sonar can move a Sonar
channel.

Two behaviours differ from a native channel:

- **`sonar:masters` refuses a mute.** Sonar's master mute rewrites every
  channel's mute and restores a stale snapshot on the way back, so unmuting
  the master unmutes channels that were muted on purpose. `MDXM_MUTE` on it
  returns an error rather than doing that. Its *volume* works normally.
- **The rows come and go.** When Sonar is disabled or crashes its channels
  disappear from `MDXM_CHAN`, and return when it does. A client should treat
  the channel list as the live truth rather than caching it.

---

## 6. Identifying a device

Four anchors, strongest first. mdxmixer matches a stored name against them in
this order, and a client holding device state should do the same.

| anchor | survives | field |
|---|---|---|
| Bluetooth address | a change of **adapter** | `bt` |
| ContainerId | a re-pair on the same adapter | `container` |
| endpoint id | nothing — it is minted per pairing | `id` |
| Windows name | a last resort; two devices can share one | `name` |

Endpoint ids and ContainerIds are both minted per pairing, so a new dongle
re-pairs everything and anything keyed on them stops matching at once. The
Bluetooth address belongs to the headset, which is why it leads.

> ContainerId has one trap. Every device with no real container gets the same
> placeholder GUID, `{00000000-0000-0000-FFFF-FFFFFFFFFFFF}`. Treating that
> as an anchor merges unrelated devices — it collapsed six Sonar endpoints
> into one here. Ignore it.

---

## 7. Staying in sync

`MDXM_SUBSCRIBE|1` turns this connection into a listener. Thereafter
mdxmixer pushes records to it unprompted, on the same connection.
`MDXM_SUBSCRIBE|0` stops it.

### Asking for a different push rate

```text
MDXM_SUBSCRIBE|1|100      ->  MDXM_OK|intervalMs=100
```

The optional second argument is **this connection's own push interval in
milliseconds**, and it applies to that connection alone — one client asking
for 100 ms does not change what anyone else receives.

- **250 ms is the default**, and what you get if you never ask.
- **100 ms is the floor.** It is the rate mdxmixer's own window refreshes at,
  and a push cannot carry anything the endpoint sweep behind it has not yet
  produced, so asking for less would send you the same numbers twice.
- **5000 ms is the ceiling**, because a subscription that reports in once a
  minute is indistinguishable from a dead one.
- **A rate outside that is clamped, never refused**, and the reply tells you
  what you actually got. A rate that cannot be read at all falls back to the
  default rather than failing the subscription.
- Ask **down**, not just up: a client that only wants to know when a device
  appears is better served by `MDXM_SUBSCRIBE|1|2000`, and it saves both ends
  the work of the pushes in between.

Changed records (`MDXM_CHAN`, `MDXM_ROUTE`) are **not** on this clock. They
arrive when something changes, as they always did; the rate governs the
tick-driven push below.

Prefer this to polling `MDXM_STATE`. A subscription costs nothing while
nothing is happening, and a poll fast enough to feel live is a full state
block many times a second. **Read `MDXM_STATE` once on connect for your
baseline, then subscribe** — the push is how state stays current, not how it
starts.

Three kinds of push arrive, and the difference matters:

| | when | what it means |
| --- | --- | --- |
| `MDXM_CHAN`, `MDXM_ROUTE` | something **changed** | a fader moved, a route switched, Sonar came back |
| `MDXM_DEVSET`, `MDXM_DEVLVL` | a device row **changed** | a volume, a mute, a battery, a hide, a device arriving or going |
| `MDXM_PEAK` | every **push interval** (250 ms unless you asked for another) | nothing changed; the levels moved |

Kept apart on purpose. Peaks move constantly and change nothing, so carrying
them on `MDXM_CHAN` would mean re-reading every fader position you already
know four times a second — and you could no longer tell a push that means
"someone moved a fader" from one that means "the music got louder".

### `MDXM_DEVSET` and the device rows

```text
MDXM_DEVSET|dev={0.0.0.00000000}.{cc8e…}|dev={0.0.0.00000000}.{3090…}|…
MDXM_DEVLVL|id={0.0.0.00000000}.{cc8e…}|…|vol=0.35|mute=0|active=1|…
```

A changed endpoint arrives as its own `MDXM_DEVLVL` — the identical record
`MDXM_STATE` would have given you, from the same formatter. Fields follow §2
exactly.

- **`MDXM_DEVSET` means rebuild.** It carries every endpoint id, in
  mdxmixer's own sort order, and arrives when the **set** of rows or their
  order changes — a headset switching on, one going away, a pin moving a row.
  Every row follows it. A client holding a control per device rebuilds its
  controls on this record and updates values otherwise.
- **It is the only way a removal can reach you.** A device that has gone has
  no row to push, so nothing else would say it had gone.
- **A row is sent only when something about it changed**, `peak` excluded —
  that one moves constantly and travels on `MDXM_PEAK` below. So a quiet
  machine with a subscriber sends one `MDXM_PEAK` per push interval and
  nothing else.
- **`seen` is minute resolution**, so a sighting a few seconds later is not a
  change and does not push.
- **Only while someone is subscribed**, like `MDXM_PEAK`: the endpoint sweep
  these come from is the one the peaks already pay for.

The latency is one 250 ms tick rather than instant, because that sweep is the
only thing on the machine that observes most of these changes — Windows does
not tell mdxmixer when a volume slider moves, a battery falls, or an endpoint
goes inactive. The sweep is the event.

### `MDXM_PEAK`

```text
MDXM_PEAK|chan=game~0.42|chan=sonar:aux~0.4457|chan=mic~-1
         |dev={0.0.0.00000000}.{cc8e…}~0.0027|dev={0.0.0.00000000}.{3090…}~-1
```

Repeated `chan=` and `dev=` keys, each `<id>~<peak>`, in one message. Values
follow §2.1 exactly, `-1` included.

- **Complete, never a delta.** Every channel and every endpoint appears every
  time, so you need not remember what you were last told, and a row that is
  absent genuinely no longer exists.
- **Only while someone is subscribed.** With no listener nothing is formatted
  and no endpoint sweep is run for it. This is the one ongoing cost the
  feature could have had, and it is paid only on request.
- **It arrives while mdxmixer is in the tray.** Which is the point: a client
  watching for the channel that just started blasting is watching precisely
  when nobody is looking at mdxmixer's own window.

Ignore the verb entirely if you do not want meters; nothing else depends on
it.

---

## 8. The audio feed is not here

Metering and visualisation do **not** go over this pipe. mdxmixer can publish
its streaming mix into a shared-memory ring, `Local\mdxmixer_stream_v1`.

### It is off until you ask

The ring is a **fallback**, for when a visualiser cannot get the audio any
other way — Sonar wedged, or not installed. When Sonar is working it already
mixes this audio and a reader can capture one of its endpoints, so running
the ring as well means two live sources for the same signal and a choice to
make on every start for no benefit.

So it does not run unless a client requests it:

```
MDXM_FEED            → MDXM_FEEDSTATE|on=0|name=Local\mdxmixer_stream_v1|rate=48000|channels=2
MDXM_FEED|1          turn it on  → MDXM_FEEDSTATE|on=1|…
MDXM_FEED|0          turn it off → MDXM_FEEDSTATE|on=0|…
```

While it is off the **section does not exist at all**, so a reader probing
for the name finds nothing — which is an honest answer, rather than a ring of
silence that looks like a broken writer. The query form tells a client the
rate and ring name without mapping anything.

This is deliberately a request and not a guess. mdxmixer does not try to
detect whether Sonar is healthy and publish accordingly: the consumer knows
whether it has audio and mdxmixer does not, and a heuristic that switches a
feed on and off underneath a reader is worse than no feed.

The state is **not** persisted. "Needed" is a property of the moment, so a
restarted mdxmixer starts with the feed off and a client that wants it asks
again on connect.

### The ring

```c

```c
struct StreamFeedHeader {
    uint32_t magic, version;
    uint32_t sampleRate, channels;     // 48000, 2 here
    uint32_t capacityFrames;           // 131072
    uint32_t reserved;
    uint64_t writeFrames;              // total frames ever written
    uint64_t writeTickMs;              // GetTickCount64 of the last write
};
// interleaved float samples follow, capacityFrames * channels of them
```

`magic` is the readiness flag and is published last on open and cleared first
on close: **`magic == 0` means do not read**, whatever the other fields say.

There is exactly **one writer**, and that is now enforced — opening the feed
when the name is already taken is refused rather than shared. Sharing a named
section means writing one geometry into a header describing another, and
whichever writer closes first clears the readiness flag out from under the
other. (mdxmixer#3 caught precisely that: a section reporting
`capacityFrames=4096`, a number this writer cannot produce, with `magic` at
zero and `writeFrames` still advancing at 48 kHz.)

A reader keeps its own read cursor, compares it with `writeFrames`, and takes
what is new. If it has fallen more than `capacityFrames` behind, the writer
has lapped it: resync to `writeFrames - capacityFrames` rather than replaying
a lap of stale audio. `writeTickMs` says whether the writer is alive at all.

### Why the control surface is a pipe and the audio is not

They have opposite shapes, and each mechanism suits one of them.

The audio feed is **continuous and one-way** — 48000 frames a second, forever,
with no reply and no acknowledgement. A pipe would mean a syscall and a copy
per block for data the reader may not even want. Shared memory is the right
answer and is what it uses.

Control traffic is **sporadic and request/response** — a human moves a fader,
a window opens, a rule changes. A few messages a second at the very most. The
pipe gives, for free, the things a ring would have to grow: request/response
correlation, a blocking wait with no polling, backpressure, and above all
*liveness* — when a client dies the server sees the pipe close, where a ring
has no idea anyone has gone. A shared-memory control channel would need its
own event object, its own framing, its own sequence numbers and its own
timeout heuristics to get back to where the pipe starts.

The saving would also be imaginary. A localhost named-pipe round trip is on
the order of tens of microseconds; the operations above happen at the speed
of a hand on a mouse. Trading a hundred microseconds a second for a
hand-rolled transport with no liveness detection is a bad trade.

The one case where it could matter is a client polling `MDXM_STATE` fast
enough to animate meters. Do not do that: subscribe (§7) for state, and read
the shared-memory ring for levels. Each mechanism then carries the traffic it
is shaped for.

---

## 9. A minimal client

```powershell
$p = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'mdxmixer', 'InOut')
$p.Connect(3000)

function Send($msg) {
    $b = [System.Text.Encoding]::Unicode.GetBytes($msg + [char]0)
    $p.Write($b, 0, $b.Length); $p.Flush()
    $buf = New-Object byte[] 65536
    $sb = New-Object System.Text.StringBuilder
    do {
        $n = $p.Read($buf, 0, $buf.Length)
        [void]$sb.Append([System.Text.Encoding]::Unicode.GetString($buf, 0, $n))
    } while ($sb.ToString() -match 'MDXM_BEGIN' -and $sb.ToString() -notmatch 'MDXM_END')
    $sb.ToString().Split([char]0) | Where-Object { $_ }
}

Send 'MDXM_PING'
Send 'MDXM_FAILOVER'
Send 'MDXM_SET|sonar:aux|personal|0.04'
Send 'MDXM_FAILOVER_LIST|dev={0.0.0.00000000}.{6d9c95fa-…}~Headphones (3- WF-1000XM6)'
```

## 10. When mdxmixer is not running

Connecting fails, which is the signal. A front-end should fall back to its
own path and retry the connection rather than queueing — there is no state to
hand over, and mdxmixer reads its own config at startup.

## 11. The network stream (VBAN)

mdxmixer can put the **monitor mix** on the network as a standards-compliant
[VBAN](https://vb-audio.com/Voicemeeter/vban.htm) stream, so a phone — or any
VBAN receiver on the LAN — can listen to what the headphones are hearing. The
design is `docs/specs/2026-10-07-vban-stream-server-design.md`; this section is
the wire surface.

Two things are deliberately separate:

- **The listener** is up whenever `on=1`, and that setting persists. The point
  of the feature is that a phone can subscribe at any moment without anyone
  touching the PC.
- **Emission** is on demand. Nothing leaves this machine until a peer has
  asked, by sending a VBAN `SERVICE`/PING0 packet to the port below; the stream
  stops when the last one stops asking. `emitting` says which state it is in.

Default port is **6980**, the VBAN default.

### `MDXM_VBAN` — set or query

The first **keyed-argument** verb in this protocol: every other verb above is
positional, this one takes `key=value` fields and applies them in order. With no
fields it is a query. Either way the reply is the whole state, so a caller never
needs a second round trip to find out what its write produced.

| key | values | notes |
| --- | --- | --- |
| `on` | `0`\|`1` | the listener socket; **persists** |
| `port` | 1..65535 | rebinds |
| `name` | 1..16 chars | the VBAN stream name; 16 is the field width on the wire |
| `source` | `personal`\|`streaming` | **`personal` is the monitor mix** — what the headphones get, per-channel balance and personal mutes included. `streaming` is the programme mix, which is a different thing. |
| `format` | `i16`\|`f32` | `i16` is half the bandwidth and what every receiver accepts |
| `gain` | 0..6400 (percent) | applied PC-side **before** the wire. It reaches 6400 because the personal mix on this machine runs at a few percent of full scale — the faders are the listening level — so sending it unamplified would put a 20–40 dB-down signal into a phone. |
| `fps` | 0.2..10 | display-capture rate |
| `open` | `0`\|`1` | `openSubscribe`: any pinger gets **audio and nothing else**. For standard VBAN tools, which cannot authenticate because the protocol has no notion of it. |
| `always` | `0`\|`1` | emit with no subscriber at all, to `target` |
| `alwaysframes` | `0`\|`1` | …and send display frames too |
| `target` | `ip:port` | empty, or unparseable, means inert — the reason appears in `error` |
| `pin` | any | **pipe-only.** Setting the secret over the channel it protects is circular, so this key is refused over VBAN-TXT. |

Two keys are **VBAN-only** and answer `MDXM_ERR` here, because they are grants
to one *peer* and the pipe is not a peer:

```text
MDXM_VBAN|frames=1   ->  MDXM_ERR|msg=frames is VBAN-only
MDXM_VBAN|audio=0    ->  MDXM_ERR|msg=audio is VBAN-only
```

Fields are applied in order; a rejected value stops processing there and the
reply names the key that failed. Fields **before** it in the same record have
already been applied, so the recovery is to re-send the whole record — every
set is an absolute value, so re-applying the ones that already took changes
nothing. (In practice the tab and the phone send one field per record, so a
record that half-applies is the exception, not the rule.)

### `MDXM_VBANSTATE` — the reply

```text
MDXM_VBANSTATE|on=1|emitting=1|port=6980|name=mdxmixer|peers=1|source=personal
  |format=i16|gain=100|fps=2|open=0|always=0|alwaysframes=0|target=
  |sent=18412|starved=0|dropped=0|depthms=120|behindms=0|srclatencyms=37
  |framessent=0|authpending=0|error=
```

The three latency numbers answer three different questions and are easy to
confuse:

- **`depthms`** is how far ahead the **producer** is. A Bluetooth render pulls
  seconds beyond what its own radio has played, so a large depth here is normal
  and is *not* delay the sender can remove.
- **`behindms`** is the sender falling behind its own schedule. This one **is**
  this machine or the network failing to keep up.
- **`srclatencyms`** is the PC's own contribution to what the listener hears
  late — the mix cushion plus a packet. A receiver adds its own jitter buffer
  and output latency to it and can then show a figure somebody can type into a
  video player's audio-delay box.

`starved` counts packets the sender had to fill with silence because the ring
was empty; `dropped` is the ring's own overflow count, which is the opposite
problem. `error` is free text and is **last**, for the reason `MDXM_FOSTATE`'s
`reason` is last.

### `MDXM_VBANPEERS`

```text
MDXM_BEGIN
MDXM_VBANPEER|addr=192.168.0.77:50001|device=Pixel 9|authed=1|since=1200|audio=1|frames=0|always=0
MDXM_END
```

`always=1` marks the configured always-stream target, which is **not** a
subscriber: it never pings, holds no table slot and is not counted in `peers` —
but something is being sent to it, so it has a row.

### `MDXM_AUTH` — VBAN-only

```text
MDXM_AUTH|pin=<pin>|device=<id>|name=<label>   ->  MDXM_ERR|msg=auth is VBAN-only
```

Authentication exists for the **network** side, where the transport has no ACL
of its own. It mirrors MDropDX12's model: a PIN plus a per-device approval
prompt on the PC, after which that device reconnects without asking again. This
pipe is already ACL'd to the interactive user and needs none of it.

### Over VBAN-TXT

Records travel as VBAN `TXT` packets, UTF-8, carrying the **same grammar** this
document describes — so a client that can already speak to the pipe needs no
second dialect. What differs:

- Every record must carry the configured **stream name**, or it is dropped: a
  record addressed to something else on the network is not ours to act on.
- **With no PIN configured, all of it is dropped** — `MDXM_AUTH` included. There
  is then no way to authenticate, so the remembered devices are inert too. This
  is "serve, but take no orders", and it is the default.
- A sender must authenticate before anything but `MDXM_AUTH` is answered.
  Unauthenticated records are dropped in silence.
- A single record never spans packets. Multi-record replies (`MDXM_BEGIN` …
  `MDXM_END`) span several, always splitting *between* records.
- `MDXM_SUBSCRIBE` is refused (`MDXM_ERR|msg=subscribe is pipe-only`): every
  broadcast site is wired to the pipe and the push direction has no
  transport-independent seam yet. Poll `MDXM_VBANSTATE` instead.
- `MDXM_VBAN|pin=` is refused (`MDXM_ERR|msg=pin is pipe-only`).
- `audio=0|1` and `frames=0|1` are **only** meaningful here, and apply to the
  sending peer: `audio=0` keeps a control session without the stream, which is
  what a phone sends when the user closes the player but keeps the mixer on
  screen.

`MDXM_AUTH` answers one of:

```text
MDXM_AUTHSTATE|ok=1                  authorised; the surface is open
MDXM_AUTHSTATE|pending=1             right PIN, unknown device: the PC has been asked
MDXM_AUTHSTATE|ok=0|err=badpin       wrong PIN (three strikes locks the address out)
MDXM_AUTHSTATE|ok=0|err=locked       too many wrong PINs from this address; wait 60 s
MDXM_AUTHSTATE|ok=0|err=denied       the person at the PC said no; terminal until restart
MDXM_AUTHSTATE|ok=0|err=nodevice     no device id in the request
```

A client that gets `pending=1` should keep sending `MDXM_AUTH` while it waits —
the PC is only prompted once per device, so re-sending costs nothing. `denied`
and `locked` are terminal: stop, and tell the user why.
