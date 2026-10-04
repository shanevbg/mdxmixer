# Rollout on the target machine

The spec's ordered rollout, as a checklist. Ordered so audio never goes
silent without a way back; rollback at any step = reassign apps back to their
previous devices (Sonar's engine stays installed and working).

- [ ] **1. Install cables.** VB-CABLE plus the A+B and C+D packs — five
  cables: one per app channel (three to start) plus streaming out plus mic.
  Fewer cables just means fewer app channels at first; the two fixed roles
  always cost two.
  > The **base VB-CABLE is free** and is a full cable with no limits. The
  > **A+B** and **C+D** packs are the other four and come with a donation,
  > so the five-cable layout below is not the free configuration. Two ways
  > to need fewer: the **streaming out** role disappears if the thing
  > consuming it is MDropDX12, which takes the mix from shared memory
  > (`MDXM_FEED`) instead of a device; and the **mic** role is only needed
  > if voice apps must see a processed microphone. With both of those off
  > the list, one free cable buys one app channel.
- [ ] **2. Set every cable to 48 kHz** shared-mode default format:
  `mmsys.cpl` → cable → Properties → Advanced → "24 bit, 48000 Hz" (or
  16/48000) for BOTH the render and capture sides of each cable.
- [ ] **3. Configure mdxmixer channels** bound to the cables (Devices tab or
  `mdxmixer.json`); run the tone test
  (`mdxmixer_test.exe --audio` with the env vars from the README).
- [ ] **4. Assign one low-stakes app** to a channel (Routing tab); verify it
  is heard through the personal mix.
- [ ] **5. Migrate the rest**: remaining apps, then voice apps to the virtual
  mic (select the mic cable's capture side as their microphone), then point
  the recorder (OBS) at the streaming cable's capture side.
- [ ] **6. Idle Sonar** (stop GG autostart). Keep it installed as rollback.

## Cable → channel table (fill in during step 3)

| role | cable render endpoint | cable capture endpoint |
| --- | --- | --- |
| channel: Game | | |
| channel: Media | | |
| channel: Aux | | |
| streaming out | | (recorder captures this) |
| virtual mic | | (voice apps capture this) |

Endpoint ids come from `mdxmixer.exe --devices`.

## Notes

- Autostart at login is the tray menu's "Start with Windows" — tick it once
  the rollout sticks (apps routed to cables are silent while mdxmixer is not
  running; Sonar has the identical property).
- Personal-output failover: arm it on the Devices tab and add fallback
  devices in preference order. Failover commits forward only — reconnecting
  the old device never moves the route back; a brief Bluetooth dropout inside
  the stability window moves nothing.
- NVIDIA Broadcast chains upstream unchanged: bind its output endpoint as the
  mic input on the Devices tab.
