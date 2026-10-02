# The flap motor

A NEMA 17 through a reduction gearbox, on a TMC2209 in the driver's standalone
**STEP / DIR / EN** mode — the same way the bench rig at
`../../../test/nema_MT6835` runs it, so the port is a copy.

## Wiring

```
   TMC2209 carrier          RP2350
   ---------------          ------------------
   VDD                      3V3          not 5V — GPIOs are not 5 V tolerant
   GND                      ground node  common with the motor supply
   DIR                      GP6
   STEP                     GP7
   EN                       GP8          active low
   MS1, MS2                 strapped     microstep select, see below
   VREF                     trimmer      THE CURRENT LIMIT. Set it first.
   PDN_UART                 —            unconnected
   VM, GND                  12-24 V PSU
   1A 1B 2A 2B              motor coils  one pair per coil, ohm them out first
```

GP6–GP8 are three consecutive pads wrapping the bottom-right corner — GP7 ends
the right edge, GP8 begins the bottom — so the driver harness is one connector.
GP9 to GP11 continue the run for the optional UART pair and DIAG.

GP0, GP1, GP4 and GP5 belong to the desk bus; GP26–GP29 (SPI1) are reserved for
the MT6835, at the far end of the board so the sensor cable runs as far from the
driver as it can. On the RP2350A Supermini GP16 drives the onboard
WS2812 and GP17–GP25 are back-side inner pads, so the bench rig's SPI0 group at
GP16–GP19 does not exist here at all — the full map is in
[../../docs/wiring.md](../../docs/wiring.md).

## Two things to get right before the first run

**Microstepping is strapping, and the firmware has to be told.** MS1/MS2 both
low or floating is 1/8 on the TMC2209 — not the A4988 table, and there is no
full-step mode.

| MS2 | MS1 | microsteps |
|---|---|---|
| 0 | 0 | 1/8 ← power-on default |
| 0 | 1 | 1/2 |
| 1 | 0 | 1/4 |
| 1 | 1 | 1/16 |

`TMC_MICROSTEPS` must match. Nothing detects a mismatch: every revolution
figure, and later every travel limit, is off by that ratio and silent about it.

**VREF is the current limit.** Software cannot see it or change it in this mode.
On a 0.11 Ω carrier, `I_rms ≈ 0.71 × VREF`. Check the sense resistor while you
are there — 0.10 Ω, 0.11 Ω and 0.15 Ω all ship on boards sold under the same
description, and it scales everything.

## Bring-up

Every command below is **development only** — the board boots into working mode
and refuses them until you say so:

```text
dev start               unlock the motor commands; gone at the next reset
```

Working mode is not a reduced console, it is the console for a board under a
desk: `status`, `stop` and all the desk configuration stay available. What is
behind the gate is everything that drives something directly, because none of it
has any business happening because of a mistyped line on a desk in use. See
[operation.md](operation.md).

```text
mot                     what the motor is doing and how it is set
mot on                  coils live
mot rev 0.25            a quarter turn of the flap shaft
mot speed 2000          slower, if it is skipping
mot accel 2000          a deliberate ramp you can hear wind up
mot accel 10000000      no ramp at all — usually a stall and a whine
```

That last pair is the whole argument for the ramp engine: the driver turns one
STEP edge into one microstep and has no idea any of this is happening.

If nothing moves: EN is active low, so GP8 has to go **low** to energise the
coils, which `mot on` does. Then VM. Then the coil pairs — a NEMA 17 wired
A1/B1/A2/B2 instead of A1/A2/B1/B2 buzzes and does not turn.

## Verified register dump

Read off the replacement driver with `usb-uart-tools/tmc_test.py` on 2026-09-27,
with VM live and nothing having configured the chip — so this is what a healthy,
untouched TMC2209 in standalone mode actually reads. Useful as a reference when
something looks wrong.

| register | value | what it says |
|---|---|---|
| `IOIN` | `0x21000041` | VERSION **0x21 = TMC2209**. ENN=1 (disabled), MS1=0, MS2=0, DIAG=0 |
| `GCONF` | `0x00000101` | `I_scale_analog`=1 → **current from VREF**; `mstep_reg_select`=0 → **microsteps from MS1/MS2**; `en_spreadCycle`=0 → **StealthChop** |
| `CHOPCONF` | `0x15010053` | TOFF=3, **MRES=5 → 1/8 microstepping**, `intpol`=1, TBL=2, HSTRT=5, HEND=0 |
| `DRV_STATUS` | `0xC01F0000` | standstill, StealthChop active, **no faults at all** — no over-temperature, no short, no open load. CS_ACTUAL 31/31 |
| `IFCNT` | `0x00000000` | no register write has ever been accepted, as expected |
| `GSTAT` | `0x00000001` | `reset`=1 — it has been reset since GSTAT was last cleared. Not a fault |

Two things worth taking from that:

- **MRES=5 confirms 1/8**, read back from the chip rather than assumed from the
  strapping. `TMC_MICROSTEPS 8` in board_config.h is correct for this board.
- **StealthChop is what MS/SPREAD strapping selects by default**, and it has
  noticeably less torque at speed than SpreadCycle. If the flap ever needs to
  move faster than it comfortably will, that is the first lever — tie SPREAD
  high, or turn the UART on and write one register.

And one thing the dump settles: **the UART link on this board works.** The
wiring, the 1 kΩ and the carrier's jumper were all correct; the silence earlier
was the driver that later turned out to be dead. So the switch below is a genuine
one-line decision, not a project.

## A dying driver looks like a slow motor

Worth knowing before you spend an evening on torque, current and resonance: a
**partly failed TMC2209 presents as a low speed ceiling, not as a dead motor.**
One phase driver degrades, the motor still turns, still holds, still sounds
normal at low speed — and then stalls somewhere well below where it used to run.

The give-away is that a torque ceiling should be *monotonic*. If a speed fails
that a lower speed and a higher speed both survive, or if a speed that used to
work no longer does with nothing else changed, suspect the carrier before the
tuning. Swapping in another driver takes a minute and answers it outright.

A dead driver is also silent on UART, which is worth remembering because it
looks exactly like a wiring fault: `usb-uart-tools/tmc_test.py` reads one
register from a laptop and settles it in seconds, with no firmware involved.

Two of these in one project is not bad luck, it is a cause. The usual ones, in
order: **unplugging the motor while VM is live** (the inductive kick has nowhere
to go), **no bulk capacitor at the driver** so every acceleration is a supply
transient, and a supply whose turn-on overshoots. The four rules above exist for
exactly this.

## The MT6835

Four wires to SPI1, at the far end of the board from the driver:

```
   MT6835 breakout          RP2350
   ---------------          ------------------
   VCC                      3V3          3.3 V, not 5 V
   GND                      ground node
   CLK                      GP26         SPI1 SCK
   MOSI                     GP27         SPI1 TX
   MISO                     GP28         SPI1 RX
   CS                       GP29         plain GPIO, driven by hand
```

The bottom (SPI) header is all that is used. The top header — A/B/Z, U/V/W, PWM
— and HVPP stay empty: the angle arrives over SPI, so those are redundant here.

The magnet must be **diametrically** magnetised, 0.5–3 mm above the package and
centred on the shaft axis. That mounting is most of the work, and the sensor
tells you when you have it wrong — see WEAK FIELD below.

### Checking it (dev mode)

```text
dev start
enc                 is it there, where is the shaft, and every health counter
enc watch           stream the angle while you turn the shaft by hand
enc zero            call this position zero
enc dir [0|1]       flip which way the angle counts
```

`enc watch` prints a line five times a second until you press a key. **Standing
still is informative too**: the jitter you see is the noise floor, and more than
a few hundredths of a degree means the magnet is too far away or off axis.

Two things that version got wrong the first time, both worth knowing if you
write anything similar:

- **It drains the input buffer before watching.** A terminal sending CRLF gives
  two characters; the console dispatches on the `\r` and leaves the `\n` queued,
  so an "any key stops" test reads it immediately and the command returns having
  printed nothing. The twin `>` prompts after every command are the same `\n`
  showing up as an empty line.
- **It prints whole lines, not `\r` in place.** Rewriting one line looks neater
  in a terminal that honours carriage return and is unreadable in one that does
  not, and the VS Code serial monitor is the second kind.

A missing sensor is a definite answer rather than a maybe. Every byte reads as
`0xFF` on a floating, pulled-up MISO, and the CRC of three `0xFF` bytes is `0x0F`
— so "no sensor" fails the CRC deterministically instead of being read as a
plausible angle.

The three counters in `enc` mean different things and have different fixes:

| counter | what it means |
|---|---|
| **CRC failures** | wiring — a lead, a missing ground, or a clock too fast for flying leads. Not magnets. |
| **weak field** | the magnet: too far from the package, too small, or not diametrically magnetised. Reported as a fraction of all samples, with the age of the most recent one — because a count that stopped climbing minutes ago was you positioning the magnet, not a fault. |
| **jumps** | a warning line, not an error: a sample saw more than a quarter turn. The unwrap stays correct up to *half* a turn, so a handful alongside a similar `late` count is one delayed sample covering a longer interval. Only a count climbing during normal movement means the sampling cannot keep up. |

If the count runs backwards relative to the motor, `enc dir 1` fixes it — on
this part direction is a register bit, not a strapped pin. It is volatile until
burnt to the sensor's EEPROM.

### What it does not do yet

Nothing gates on it. `mode_check()` now asks the sensor a real question and
`dev` reports the answer, but `FLAP_DRIVES_MOTOR` is still 0, so a missing
encoder does not stop the desk half working. That becomes a hard requirement in
step 4 — see [operation.md](operation.md).

## What this mode cannot tell you

Over-temperature, a shorted phase and an open coil all look exactly like a
working motor. That is what the UART would have reported for nothing.

A stall used to be on that list. It is not any more — open-loop STEP/DIR still
emits an identical pulse train whether the motor followed it or not, but the
shaft is measured now, so the difference is a number in degrees.

## The UART option, and why it is off

The driver can also run on a serial link over PDN_UART. It is built and it
works — `TMC_UART_ENABLED` in `board_config.h` turns it on, and
`TMC_USE_DIR_PIN` / `TMC_USE_EN_PIN` then move DIR and EN into registers. GP9
and GP10 are reserved for it: TX through a 1 kΩ, RX on the same node, joined at
this board so a single wire leaves for the driver. Both configurations are kept
compiling, and an incoherent one — no UART *and* no DIR/EN pins — is a compile
error rather than a motor that will not move.

**What it buys:** current and microstepping as numbers instead of a trimmer and
jumpers; over-temperature, short and open-coil reporting; StallGuard; and three
wires down to two.

**What it costs**, which is why one wire did not pay for it:

1. **VM must be live before the chip answers at all.** Not a brown-out, not a
   marginal case — with the motor supply off the UART is silent, permanently,
   and the symptom is identical to a broken wire.
2. **A solder jumper on the carrier ships unbridged.** Watterott
   SilentStepSticks need ["the jumper on the driver … bridged from the middle to
   the respective
   position"](https://learn.watterott.com/silentstepstick/pinconfig/tmc2209/);
   other boards break out a pad marked `UART` or `USART` that is connected to
   nothing. Until that blob exists, perfect wiring and perfect firmware produce
   perfect silence.
3. **The 1 kΩ is not optional** — both ends drive the one pad, and without it no
   reply survives its CRC.
4. **MS1/MS2 become the UART address**, the same pins that used to select
   microstepping. A driver on the wrong address is silent exactly like a driver
   that is not there.

Three pins that either work or visibly do not, against two pins plus four new
ways to be silently wrong. Turning it on is a one-line change whenever that
trade looks different — and the diagnostics survive: `tmc` names which of five
failures happened, `tmc wire` loops back on our own two pins with no driver
attached, and `tmc scan` asks all four addresses.

## Where this is going

1. **Rotation** — done. The AVR446 ramp out of an alarm ISR, `mot` and `tmc`.
2. **MT6835** — done. The 21-bit encoder on SPI1, `enc` and `enc watch`, and
   the boot self-check now asks it a real question.
3. **Travel limits** — `limits.c` from the rig: min and max stored in flash,
   backlash, and the creep onto a target under the encoder. Its values go in
   `settings()->flap` and it marks the settings dirty like everything else —
   `nvs` keeps exactly one record and `settings.c` is the only thing that may
   write it.
4. **The desk** — `run_flap()` in `flap_task.c` is a two-second dwell today.
   That is the line that becomes a real move.
