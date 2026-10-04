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
→ framed. `MDXM_RING` per ring buffer (fill, overruns, underruns) and
`MDXM_DIAGDEV` with the live personal device and whether it is on a fallback.
For troubleshooting, not for driving a UI.

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
| `MDXM_ROUTE_SET\|personal\|<endpointId>` | bind the personal output |
| `MDXM_DEFAULT\|<endpointId>` | move the **Windows** default output |
| `MDXM_ASSIGN\|<exePath>\|<ch or ->` | send one app to a channel |
| `MDXM_APPROUTE\|<exePath>\|<endpointId>` | send one app straight to an endpoint |
| `MDXM_SHOW` / `MDXM_HOTKEYS` | raise a window |
| `MDXM_EXIT` | shut down through the real exit path |

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

Prefer this to polling `MDXM_STATE`. A subscription costs nothing while
nothing is happening, and a poll fast enough to feel live is a full state
block many times a second.

Two kinds of push arrive, and the difference matters:

| | when | what it means |
| --- | --- | --- |
| `MDXM_CHAN`, `MDXM_ROUTE` | something **changed** | a fader moved, a route switched, Sonar came back |
| `MDXM_PEAK` | every **250 ms** | nothing changed; the levels moved |

Kept apart on purpose. Peaks move constantly and change nothing, so carrying
them on `MDXM_CHAN` would mean re-reading every fader position you already
know four times a second — and you could no longer tell a push that means
"someone moved a fader" from one that means "the music got louder".

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
