# Roadmap

Where this rig is going, and what each step has to prove before the next one is
worth starting.

## Phase 0 — done, on other hardware

- The desk protocol is reverse-engineered and bus-verified:
  [CL103B-G_protocol.md](CL103B-G_protocol.md). Framing, height reports, the
  full key-code table, injection rules, panel-free operation.
- Stepper and MT6835 encoder are proven on the bench rig at
  `../test/nema_MT6835`: motion profile, measured gearbox ratio, lost-step
  detection, encoder-defined travel limits.
- The sniffer firmware exists and is listen-only by construction.

What is *not* proven: the stepper and the encoder on the RP2350 Supermini, and
the flap against the real desk.

## Phase 1 — listen, and move the flap

*Bus half done, flap half not started.* The taps work against the real desk:
height and keys decode, the trace and the `bus` measurements run. The stepper
has not been wired to this board yet, so steps 3–5 below are still open, and
the timing question they answer is still open with them.

Wire the taps and the driver per [wiring.md](wiring.md); leave the encoder
unfitted. The desk keeps its panel and stays in charge throughout.

**Steps**

1. Ground tap + board tap only. `p` shows a live height; `w` streams it while
   the desk moves. Nothing else matters until this works.
2. Add the panel tap. Press keys, watch them decode.
3. Add the driver with the motor *uncoupled*. `home`, then `x` / `c` by hand.
4. Couple the flap. Find the real hysteresis band with `th` + `w`, then bake it
   into `FLAP_EXPAND_BELOW_MM` / `FLAP_COLLAPSE_ABOVE_MM`.
5. `auto on`, and drive the desk around for a while.

**The measurement that decides phase 2.** On the numbers currently in
`board_config.h` a full flap swing is about **2 s** — 90° of output through
48.435:1 at 1/8 microstepping is 19 400 microsteps, and at 10 000 steps/s that
is 1.94 s plus the ramp. The desk descends at roughly 25 mm/s and coasts 8–15 mm
after the last command. So by the time the flap has finished moving, the desk
has travelled something like **50 mm** past the height that triggered it, plus
coast.

(Confirm the ratio first: `GEAR_RATIO` here is 48.435, the *old* gearbox. The
bench rig now runs 17.23:1, which would cut the swing to ~0.7 s and the margin
to ~20 mm. Which box is on the flap changes the answer by a factor of three, so
measure before trusting either number.)

That number is the whole question. Time the flap and the desk for real, then:

- If the threshold can lead the collision point by that margin and still sit
  inside the desk's useful range — **listening is enough, and phase 2 is
  optional.** Preset recalls even announce their target about a second ahead
  (protocol §3), which buys more lead than a manual hold does.
- If the margin does not fit, the flap has to be able to *stop the desk*, which
  is phase 2.

**Exit criteria:** height decodes with no checksum errors over a long run, the
flap tracks the band without oscillating, and the timing above is a measured
number rather than an estimate.

## Phase 2a — replace the panel  — **done and working**

Panel unplugged, RP2350 in its place: `emul on` streams the poll at 104 ms and
the board reports height with no panel attached. `go`, `sit`, `stand`, `save`
and raw `key` all work. No rewiring was needed — GP5 is the panel tap while
sniffing and a PIO transmitter while emulating, because the wire the board
listens on is the wire the panel used to drive.

Four things the real desk taught us here, all now in
[the protocol doc](CL103B-G_protocol.md):

- **One-shot codes are sent exactly once.** One frame for a recall, one for a
  save. The panel's press durations — a tap, a five-second hold — are timed
  *inside the panel* and never reach the bus. Streaming a recall for three
  frames is why a laptop script could save presets but never recall one;
  streaming a save for five seconds stores it about ten times.
- **The height field is 14 bits plus flags.** Bit 14 is raised around a save.
  Read raw, it reports a 17-metre desk, which a closed-loop move would try
  hard to correct.
- **`0x05`/`0x07` are the down/up press edges**, completing the motion group.
- **A recall takes its time**: ~5 s before the board reacts at all, then ~1 s
  announcing the target as a static height before it starts driving.

## Phase 2b — man in the middle: **ruled out**

Splitting the panel's TX from the board's RX needs a break in that wire, and
on this desk the header cannot be passed through — there is no way to get
between the panel and the board without cutting. So a true MITM is off the
table, and with it the ability to *suppress* what the panel sends.

**Frame-gap injection does not rescue it either.** The idea — transmit in the
~95 ms the line sits idle between panel frames — is sound in timing and wrong
in electronics. The panel's TX is push-pull and actively drives the line high
when idle. Sinking against it gives a divider of roughly equal output
impedances, so the line lands near 1.6 V: not a valid logic low, so the board
does not read our bits, while both output stages pass tens of milliamps. There
is no series resistor that fixes this — one large enough to be safe makes the
panel win by more.

Either the wire is broken or we cannot talk over the panel. Those are the only
two states.

### What replaces it

Nothing needs to. The two jobs the flap actually has are both reachable
without cutting:

**Knowing early.** Three lead-time signals are already on the taps, passively:

| Signal | Lead time |
|---|---|
| a key frame (`0x05`/`0x06` down, `0x07`/`0x08` up) | the instant the user presses, before the height has moved |
| a preset recall's target announcement | ~5 s of board silence, then ~1 s of announced target, before motion |
| the height stream itself | continuous, ~1 mm per frame |

The flap currently waits for the *height* to cross a threshold, which is the
last and latest of the three. Triggering on intent instead is a firmware
change and buys back most of the swing time.

**Not needing to stop the desk.** Phase 2a already proved the RP2350 can *be*
the panel. With the panel unplugged for good, the desk only ever moves when we
tell it to — so the flap goes first and the desk follows, and there is nothing
to stop. The cost is the physical buttons, which GP12–GP15 and GP26–GP29 are
free to replace.

Cutting one wire stays available as the fallback, and is genuinely small — but
it should be the answer only after the timing measurement in phase 1 says the
lead time above is not enough.

## Phase 3 — close the flap loop with the encoder

Fit the MT6835 on GP8–GP11 and port `task_encoder.c` / `limits.c` from the
bench rig.

This is what makes the limit switch redundant: the MT6835 is absolute, so the
flap knows where it is at power-on with no homing sweep at all. Travel ends
become stored angles (`lim min` / `lim max`) instead of a switch, and a stall
becomes a number instead of silence — open-loop step/dir emits the same pulses
whether or not the motor followed them.

Bring the gearbox ratio over by *measuring* it with `cal`, not by copying
`GEAR_RATIO`: the bench rig's box measured 17.23:1 and the one before it,
sold as "50:1", measured 48.435:1.

**Exit criteria:** no homing sweep at boot, position correct after a power
cycle, and a deliberately jammed flap reports lost steps instead of silently
drifting.

## Phase 4 — permanent install

- Power from a 24 V → 5 V buck into the `5V` pad, USB disconnected — see the
  power section in [wiring.md](wiring.md) about not doing both at once.
- The onboard WS2812 on GP16 becomes the status light: height streaming,
  height stale, flap moving, fault.
- Connectors and strain relief on the desk tap. A tap that falls off mid-move
  is the failure mode most likely to happen twice.
- Enclosure, and the 3D-printed parts in `3d-models/`.

## Deliberately not planned

- **TMC2209 UART.** Both hardware UARTs belong to the desk; the driver's UART
  would have to be a PIO one. Worth it only if software current control or
  StallGuard turns out to be needed, and phase 3 makes StallGuard largely
  redundant.
- **Driving the desk to positions.** `usb-uart-tools/desk_goto.py` already does
  closed-loop positioning from a laptop. Putting it in the firmware is a
  different project from "flap follows desk".
