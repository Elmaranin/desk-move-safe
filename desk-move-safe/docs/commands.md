# Console commands

The complete command reference for the USB console (USB CDC; any terminal).
`?` prints the short form on the board.

> **Kept in sync by the build.** Every command `help()` prints in
> [`../src/console.c`](../src/console.c) must have a row in the command tables
> below, with the same signature, and every parameter in
> [`../src/params.c`](../src/params.c) must have a row in
> [Stored parameters](#stored-parameters) — and nothing may be documented that
> no longer exists. [`../tools/check_commands.sh`](../tools/check_commands.sh)
> runs at the start of `./build.sh` and fails the build on any difference.

## How the console is laid out

A few words at the top level. Everything else sits under a **group word** —
`desk`, `debug`, `lim`, `mot`, `tmc`, `enc` — and every stored value is read and
changed with **`eeprom`**. A group word on its own prints that group's state.

**Mode** says where a command works. *working* commands are available after
every reboot. *dev* commands need `dev start` first; they drive something
directly, poke a register or change the flap's stored range. See
[operation.md](operation.md#working-mode-and-dev-mode).

## Top level

| Command | Mode | What it does |
|---|---|---|
| `status` | working | Everything at a glance: the self-check (mode, driver, encoder, limits, and whether the desk is locked), then what `desk` prints, whether the settings came from flash, and the motor's state. Printed at boot too. |
| `calibrate` | working | Calibrate the flap, step by step: clears the calibration (what `reset` clears), then asks for the flap at its expanded end and at its collapsed end and stores each on `done`. Unlocks `mot` and `enc` while it runs. See [Calibrating](#calibrating). |
| `done` | working | During `calibrate`: store the position the flap is at now, and go on to the next step. |
| `abort` | working | During `calibrate`: leave it. The calibration stays cleared, so the desk stays locked until `calibrate` is run to the end. |
| `reset` | working | Clear the **flap calibration** — exactly the parameters the desk and the motor need to move (see [What it needs to move](#what-it-needs-to-move)) — and nothing else. Afterwards the desk is locked and the motor only jogs until the flap is calibrated again — `calibrate` does that. Presets, coast, the flap height, backlash and approach are kept. Not to be confused with resetting the board, which clears no parameter. |
| `stop` | working | Stop whatever the firmware is driving, now: a `go`, a flap takeover, the flap motor. |
| `dev` | working | Working or dev mode, and the boot self-check, repeated. |
| `dev start\|stop` | working | Unlock the dev commands, or lock them again. Dev mode suspends the flap intercept; leaving it stops and releases the motor. Never stored: every reboot comes up in working mode. |
| `eeprom` | working | Every stored parameter with its value and unit, grouped by section. The first line says whether RAM matches flash, has a change waiting to be written, or nothing is stored yet. |
| `eeprom <name>` | working | One parameter, e.g. `eeprom desk_flap_mm`. |
| `eeprom set <name> <value>` | working | Change one parameter, in its stored unit (mm, not cm). Validated by the module that owns it; written to flash once the desk and the motor have been still for a few seconds. `lim_*`, `mot_*` and `desk_early_resume` need dev mode, and the measured ones are read-only — the reply names the command that sets them. |
| `?` | working | The short command list. |
| `go <cm>` | dev | Drive the desk to a height, bypassing the panel. Refused while the desk is locked (see [What it needs to move](#what-it-needs-to-move)). From far away it drives and releases a coast early; within a coast of the target, where a drive would overshoot, it steps up or down with 10 mm taps until it is within 10 mm. Panel buttons are ignored during the move; `stop` ends it. `go` does not move the flap: a target above the flap height is refused while the flap is not collapsed. |

## desk — the desk side

| Command | Mode | What it does |
|---|---|---|
| `desk` | working | Live height, bus frame counts, what the firmware is doing, the flap stop (height, on/off), the ceiling, the presets and the coast. |
| `desk ceiling [on\|off]` | working | Replace UP with idle near `DESK_CEILING_MM`, whatever the panel asks: a held UP a coast early, a tap only if its 10 mm step would end past the ceiling. Says so (`[desk] UP refused …`) and blinks red. Preset recalls are not checked. Not stored: on at every boot. |

The desk's stored values — presets, coast, flap height and switch — are
parameters: `eeprom set panel_stand_mm 1035`, `eeprom set desk_flap_mm 770`.

## debug — tracing what the desk side decides

| Command | Mode | What it does |
|---|---|---|
| `debug` | working | Whether the trace is running, and how many events are waiting to print. |
| `debug start\|stop` | working | Start printing every change that steers a move, timestamped from `debug start`, or stop. Read-only — it changes nothing the firmware does — and off at every boot. |

Each line is `[dbg <seconds>] <what> <detail>`, printed when something
**changes** — a quiet desk prints nothing:

| Line | Means |
|---|---|
| `state A -> B` | the flap state machine: `PASS` (panel in charge), `PLANNING` (a recall seen, waiting for its destination), `STOPPING` (stopping the desk), `MOVING` (the flap job owns the desk) |
| `panel sends …` | the key the panel put on the wire — every button press shows here |
| `recall … : …` | what was decided about a preset press, and **why**: taken over, passed through (and the reason — flap off, hands-off, dev mode, no height), waiting for the board's announcement, or caught late by the approach stop |
| `board hears …` | the key the board actually got — the panel's, or ours in its place |
| `height … mm (going up/down)` | the desk's height, from the board |
| `board announces destination … mm` | where a recall is taking the desk — printed for every announcement, repeats included |
| `flag hands-off / armed / desk locked / intercept = yes/no` | hands-off: the flap ignores the bus while a resumed move finishes; armed: bench builds only (`FLAP_DRIVES_MOTOR 0`) — with a motor, the desk is stopped whenever the flap is not at the end the far side needs; intercept: whether the flap may act at all |
| `job …` | the flap job's phases: takeover (or `manual crossing` for a held key or a tap — no recall afterwards), at the flap height, flap moving to EXPANDED/COLLAPSED, flap there (or move FAILED — the desk stays put), resume (the recall re-sent once), whether it went out, done. With `desk_early_resume 1`, `resume` comes before `flap there` |
| `… events LOST` | the console could not print fast enough; the record has a gap |

**Reading a stand → sit move that stops at the flap.** The sequence should be:
`panel sends SIT recall` → `recall SIT recall: crosses the flap — taken over` →
`job flap takeover` → `state … -> MOVING` → heights
going down → `job at the flap height` → `job flap moving to EXPANDED` →
`job flap there` →
`job resume: re-sending SIT recall once` → `board hears 0x02 SIT recall` →
`job recall went out on the wire` → `board announces destination …` → heights
going down again. The last line before it goes quiet says where it stopped:

| Last thing in the trace | Points at |
|---|---|
| `recall …: PASSED: …` | the press was not taken over; the reason is on the line. Since 2026-10-02 the approach stop catches such a move at the flap height and still finishes it (`caught by the approach stop`) |
| no `at the flap height` | the approach: `[desk]` lines say stalled, timed out, or no height |
| `recall NEVER went out` | the panel stopped polling, so our one-shot frame had nothing to ride on |
| `recall went out`, but no `announces destination` | the board ignored the recall — it was too soon after the stop, or asleep |
| `announces destination`, but no height change | the board accepted it and did not move |

## lim — the flap's travel range

Stored as the MT6835's raw angle at each end plus the microsteps between them.
Until **both** ends are stored the motor only jogs **and the desk refuses every
move** (see [What it needs to move](#what-it-needs-to-move)). `reset` clears
them.

| Command | Mode | What it does |
|---|---|---|
| `lim` | working | The stored range field by field, whether both ends are stored, where the shaft is in the range, how far the step counter and the encoder disagree. With nothing stored, it says how to store it. |
| `lim expanded` | dev | Store the shaft's current position as the flap's expanded end. |
| `lim collapsed` | dev | Store it as its collapsed end. Must be in the same session as `lim expanded`, with no `mot zero`, `enc zero` or `mot halt` in between, so the steps between the ends are counted. The range must fit inside one turn of the magnet's shaft. With a range already stored, `lim expanded`/`lim collapsed` move that end to where the shaft is now (shrinking only; to widen, `lim free`, jog past, then mark). |
| `lim play [<deg>]` | dev | Measure the gearbox backlash by creeping each way until the encoder sees the output move `<deg>` (default 1), and store it as `lim_backlash`. Must be done **before** storing the ends, since the span is corrected by it — on a calibrated flap, `reset` first. |
| `lim span [<n>]` | dev | With no argument: after `mot go expanded` then `mot go collapsed`, re-measure the steps between the ends from the step counter. With a number: set it. |
| `lim sync` | dev | Re-seed the step counter from the encoder's absolute angle and say where the shaft is. Happens automatically at boot and before every move. |
| `lim free` | dev | Toggle: ignore the range and the guard until reboot or `lim free` again. For measurements that need whole turns. Nothing then stops the flap hitting its ends. |

## mot — the flap motor

Every move goes through the travel range. Without a stored range only
`mot jog` moves; with one, nothing may leave it, and the encoder hard-stops the
motor `LIMIT_GUARD_DEG` past either end if something does anyway.

| Command | Mode | What it does |
|---|---|---|
| `mot` | dev | Motor state, position, speed, the motion profile, and the driver's status line. The cruise speed is a parameter: `eeprom set mot_speed_sps <n>`. |
| `mot on\|off` | dev | Energise the coils, or release them so the flap turns by hand. |
| `mot accel <sps2>` | dev | Ramp rate, microsteps per second squared. Session only. |
| `mot jog <steps>` | dev | A small signed move, at most one motor revolution. The only move allowed with no range stored, for finding the ends. With a range stored it is range-checked, and from outside the range it may only head back in. |
| `mot go expanded\|collapsed\|<n>` | dev | Go to the expanded or collapsed end, or `n` microsteps from expanded: fast most of the way, then slowly for the last `lim_approach_deg` degrees while the encoder is watched, stopping within 0.1° of the target (see [How mot go arrives](#how-mot-go-arrives)). Any key stops it. |
| `mot move <steps>` | dev | Signed relative move in microsteps, refused if it would end outside the range. |
| `mot rev <revs>` | dev | Signed revolutions of the flap shaft, through `GEAR_RATIO`. Same range check. |
| `mot run fwd\|back` | dev | With a range: run to the end of travel that way and settle. Under `lim free`: run until `mot stop`. |
| `mot stop` | dev | Ramp down. |
| `mot halt` | dev | Cut the pulses now. The step count no longer matches the shaft; the next move re-seeds it from the encoder. |
| `mot zero` | dev | Call this position zero (the step counter only; the stored range is in absolute angle and unaffected). |

### How mot go arrives

A `mot go` cannot just run the counted number of steps and stop. Between motor
and flap there is a gearbox with some play, and the flap has weight: near the
point where its weight changes sides, it can drop through that play and land a
few degrees **further than the motor moved**. Run the whole distance at speed
and the flap overshoots — at an end, into the end stop.

So a move is done in two parts:

1. **Fast, by step count**, to `lim_approach_deg` degrees *short* of the target.
2. **Creep**: a few microsteps at a time, reading the encoder after each, until
   the flap is within 0.1° (`LIMIT_SETTLE_DEG`) of the target.

The flap therefore always arrives slowly, from the inside of its range, and the
encoder decides when it is there. Example, 179.6° of travel, approach 4°:
`mot go collapsed` runs the first 175.6° at speed and creeps the last 4°.

| lim_approach_deg | Effect |
|---|---|
| larger | safer against a bigger gap, a slower finish |
| 4 (default) | the bench rig's measured worst gap was 3.6° |
| 0 | the whole distance at speed, then corrected afterwards — the flap **can overshoot by the size of the gap first** |

Rule of thumb: bigger than the largest jump the flap makes on its own. Only
`mot go` (and `mot run` with a range) use it; jogs and `mot move` do not.

## tmc — the driver (TMC2209)

Most of these need the UART link (`TMC_UART_ENABLED`). In standalone mode they
explain what the link would give and change nothing.

| Command | Mode | What it does |
|---|---|---|
| `tmc` | dev | How the driver is set up and what it can report: version, wiring, current, microstepping, chopper, registers. Standalone: what the strapping implies. |
| `tmc init` | dev | Re-establish the link after fixing the wiring. |
| `tmc wire` | dev | Loopback test of our own TX and RX pins through the 1 k resistor. No driver needed. |
| `tmc scan` | dev | Ask all four UART addresses which one answers, and use it. |
| `tmc current <run> [hold]` | dev | RMS milliamps per phase; hold defaults to half of run. |
| `tmc micro <n>` | dev | Microsteps, 1..256. Not while moving. |
| `tmc chop stealth\|spread` | dev | StealthChop (quiet) or SpreadCycle (more torque at speed). |
| `tmc reg <hex> [hex]` | dev | Read one register, or write it. |

## enc — the encoder (MT6835)

| Command | Mode | What it does |
|---|---|---|
| `enc` | dev | Is it answering, the raw angle, position since `enc zero`, gearing, and its health counters with what each one means. |
| `enc watch` | dev | Print the angle while the shaft is turned by hand, only when it moves. `z` zeroes, any other key stops. |
| `enc zero` | dev | Call this shaft position zero. The raw angle, which the range is stored in, is unaffected. |
| `enc dir [0\|1]` | dev | Which way the angle counts (a sensor register, volatile). |
| `enc gear [<n>]` | dev | The reducer between motor and magnet, N:1. Session only; `GEAR_RATIO` at reboot. |
| `enc reg <hex> [hex]` | dev | Read or write one sensor register. |

## What it needs to move

Two things, checked at every panel frame and before every motor move:

1. **The MT6835 answers.** Live hardware, not a stored value: the encoder task
   must have had a valid frame from it.
2. **The flap is calibrated** — these stored parameters are set:

   | Parameter | What it is |
   |---|---|
   | `lim_flags` | both bits set: both ends are stored |
   | `lim_expanded_raw` | the flap shaft's angle at its expanded end |
   | `lim_collapsed_raw` | its angle at its collapsed end |
   | `lim_enc_span` | sensor counts from expanded to collapsed |
   | `lim_span_steps` | motor microsteps from expanded to collapsed |
   | `lim_steps_per_rev` | the microstepping `lim_span_steps` was counted in |

   They are measured together — by `calibrate`, or `lim expanded` then `lim collapsed` in dev
   mode — and only make sense as a set. They are read-only through `eeprom`, and **`reset` clears exactly
   these six** — nothing else.

Every other parameter has a working value from the start and never blocks a
move: the presets are learned, the coast and flap height come from
`board_config.h`, backlash defaults to none and approach to
`LIMIT_APPROACH_DEG`.

**If either check fails:**

| | What happens |
|---|---|
| desk | **every move is refused**: the panel's UP, DOWN and preset recalls are replaced with idle before they reach the board, and `go` says why. **The board's LED blinks red three times** for each refused press, so whoever is at the panel can see it was heard and refused on purpose. Saving a preset and the panel's wake code still pass; a move already being stopped finishes stopping. `status` shows `DESK LOCKED`, the reason, and how many moves were refused. |
| flap motor | only `mot jog` moves it, at most one motor turn per command, so the ends can be found. Everything else is refused. |

The reason for locking the desk: the flap sits in the desk's path. Until the
firmware can see the flap and knows where its ends are, it cannot tell whether
moving the desk is safe. Decided 2026-10-02 while the flap is being fitted; see
[operation.md](operation.md#working-mode-and-dev-mode).

## Calibrating

`calibrate` is how a board gets its calibration: on a fresh board, after `reset`, or
whenever the flap's ends have moved. It needs no `dev start` — a board with a
locked desk must be fixable by whoever is standing at it — and the boot banner
suggests it whenever the flap is not calibrated.

```text
> calibrate
flap calibration cleared — ...           # step 0: the six parameters above
CALIBRATE 1/2 — move the flap to its EXPANDED position with the motor:
calibrate expanded> mot jog -400                   # signed microsteps, one motor turn max
calibrate expanded>                                # Enter repeats the last jog
calibrate expanded> mot jog 50                     # fine-tune
calibrate expanded> done                           # lim_expanded_raw stored
CALIBRATE 2/2 — move the flap to its COLLAPSED position ...
calibrate collapsed> mot jog 400
calibrate collapsed>                                # ... Enter, Enter, ...
calibrate collapsed> done                           # lim_collapsed_raw stored, the rest worked out
CALIBRATION COMPLETE — the desk is unlocked.
```

- **Only `lim_expanded_raw` and `lim_collapsed_raw` are positions you set.** The other
  four — `lim_flags`, `lim_enc_span`, `lim_span_steps`, `lim_steps_per_rev` —
  are worked out from the two marks: the sensor counts between the angles, and
  the motor steps the jogs took to get from one to the other.
- **Move the flap with the motor, not by hand.** The steps between the ends are
  counted from the motor's own moves; a hand-turned end has none behind it, and
  the span comes out wrong (`lim collapsed` warns when it disagrees with the gear
  ratio by more than 20%).
- **Jogs are capped** at one motor revolution each, because there is no range
  yet to check a bigger move against. With the gearbox that is a small turn of
  the flap, so Enter-to-repeat does the travelling.
- While `calibrate` runs, `mot` and `enc` work without `dev start`; everything else
  keeps its usual mode. `status` and `?` work as normal.
- If the two marks do not make a usable range — the same place, or more than
  one turn of the shaft apart — `calibrate` says so and starts again from expanded.
- `abort` leaves it with the calibration cleared and the desk locked.
- Backlash: `lim play` measures it, and must run before the ends are stored —
  `dev start`, `reset`, `lim play`, then `calibrate`.

The record is written to flash a couple of seconds after `done`, once the motor
is still (`[settings] saved: ... limits stored`). `eeprom` shows what was stored.

## Stored parameters

What `eeprom` lists: one CRC-checked record in the last flash sector, owned by
[`../src/settings.c`](../src/settings.c). Each name is `<section>_<field>`, and
the section is the console group it belongs to (`mot_*` is the flap motor).
**Needed** marks the calibration the desk and the motor cannot move without —
the six `reset` clears. [Parameters explained](#parameters-explained) below says
what each one means in practice.

| Parameter | Unit | Meaning | `eeprom set` | Needed |
|---|---|---|---|---|
| `version` | — | the record's layout; an older one is still read and upgraded | read-only | |
| `panel_stand_mm` | mm | the stand preset's height; 0 = not known yet | working | |
| `panel_sit_mm` | mm | the sit preset's height; 0 = not known yet | working | |
| `desk_coast_up_mm` | mm | how far the desk keeps moving up after being told to stop | working | |
| `desk_coast_down_mm` | mm | the same, going down | working | |
| `desk_flap_mm` | mm | the height the desk is stopped at so the flap can move | working | |
| `desk_flap_on` | 0\|1 | whether the desk is stopped there and the flap moved. With 0 the flap is never moved, and the crossing guard refuses any move up past the flap height while the flap is not collapsed | working | |
| `desk_early_resume` | 0\|1 | when the desk moves on after the flap: 0 = once the flap has settled on its end, 1 = as soon as its fast move is over and it starts to creep | dev | |
| `lim_flags` | bits | which ends are stored: 1 = expanded, 2 = collapsed, 3 = both | read-only: `lim expanded`, `lim collapsed` | **yes** — `reset` clears it |
| `lim_expanded_raw` | raw | the encoder's reading with the flap at its expanded end | read-only: `lim expanded` | **yes** — `reset` clears it |
| `lim_collapsed_raw` | raw | the encoder's reading with the flap at its collapsed end | read-only: `lim collapsed` | **yes** — `reset` clears it |
| `lim_enc_span` | counts | encoder counts from expanded to collapsed, with direction | read-only: `lim collapsed` | **yes** — `reset` clears it |
| `lim_span_steps` | microsteps | motor microsteps from expanded to collapsed, with direction | read-only: `lim collapsed`, `lim span` | **yes** — `reset` clears it |
| `lim_steps_per_rev` | microsteps | microsteps per motor turn when the span was counted | read-only | **yes** — `reset` clears it |
| `lim_backlash` | microsteps | the gearbox's play, added to every change of direction | dev, before the ends (`lim play` measures it) | |
| `lim_approach_deg` | deg | how far before its target a `mot go` slows to a creep | dev | |
| `mot_speed_sps` | steps/s | the flap motor's cruise speed, 100..15000 | dev | |
| `mot_early_start` | 0\|1 | when the flap starts: 0 = once the desk has stopped at the flap height, 1 = the moment the desk is told to stop | dev | |

Not stored, from `board_config.h` at every boot: proxying (always on), the
ceiling switch, `mot accel`, `enc gear`, `enc dir`, and the mode.

The record carries a layout `version`. New fields are only ever added at the
end, so a record written by older firmware is still read: its values are kept,
the new fields take their defaults, and it is rewritten in the new layout — a
firmware update never costs the flap calibration.

## Parameters explained

### Desk heights: `desk_flap_mm`, `panel_*_mm`, `desk_coast_*_mm`

All in **millimetres of desk height**, the same number the panel shows ×10
(74.3 on the panel = 743).

- **`desk_flap_mm`** — the height the flap needs the desk to be at before it can
  move. Any move that would carry the desk across it — a preset, a held key or
  single taps — is stopped there first and the flap is moved.
  `desk_flap_on 0` turns that off; the flap is then never moved, and the desk
  is refused any move **up** past the flap height — or further up, if it is
  already above it — while the flap is not collapsed (see the crossing guard in
  [operation.md](operation.md#how-a-preset-is-handled)).
- **`panel_stand_mm`, `panel_sit_mm`** — where the panel's two preset buttons
  take the desk. Learned by watching: when a preset is pressed, the board
  announces its destination a second before it moves; that is compared with
  the stored height and written **only if it differs**. Known in advance, a
  preset that would cross the flap height is caught before the board even hears
  it. 0 = not learned yet. These two are the only parameters stored without a
  command — [operation.md](operation.md#how-a-preset-is-handled) has the rules.
- **`desk_coast_up_mm`, `desk_coast_down_mm`** — **coast** is how far the desk
  keeps moving after it has been told to stop: the board ramps the motors down
  rather than stopping dead. To stop *at* a height, the stop has to go out that
  much *early*:

  ```text
  going up to the flap at 770, coast up 18:

     752 ─── stop sent here (770 − 18)
      │      the desk coasts on ...
     770 ─── ... and settles about here
  ```

  Used by the flap stop and by `go`. When the desk is already closer than a
  coast to where it has to stop, a release would overshoot, so it steps there
  instead with 10 mm taps (a tap is a fixed step and cannot coast). It does not
  need to be exact:
  landing within 10 mm (`DESK_NEAR_MM`) counts as there. If stops keep landing
  short or long by the same amount, change it by that amount; `go` prints where
  it actually stopped.

### The flap's calibration: `lim_*`

Measured by `calibrate` (or `lim expanded` / `lim collapsed`). The flap shaft carries a magnet,
and the MT6835 reads its angle as a number from 0 to 2 097 151 for one full
turn — 5 825 counts per degree. That number is absolute: it is the same after
a power cut, which is why the ends are stored as angles.

Your calibration of 2026-10-02, as an example:

| Parameter | Value | Read as |
|---|---|---|
| `lim_expanded_raw` | 1 382 270 | the shaft at 237.28° when the flap is at its expanded end |
| `lim_collapsed_raw` | 336 164 | at 57.71° when it is at its collapsed end |
| `lim_enc_span` | −1 046 106 | expanded → collapsed is 179.6° of shaft, and the reading goes **down** on the way (the sign) |
| `lim_span_steps` | −13 600 | the motor turned 13 600 microsteps to get there, in its **backward** direction (the sign) |
| `lim_steps_per_rev` | 1 600 | counted at 8 microsteps × 200 steps per motor turn |
| `lim_flags` | 3 | both ends stored |

- **Two measures of the same distance.** `lim_enc_span` is the distance as the
  *encoder* sees it — the truth, read at any time. `lim_span_steps` is the same
  distance as the *motor* moves it — exact as long as no step is lost. A move
  is made of steps and checked against the angle; if the two disagree by more
  than 1° (`LIMIT_RESYNC_DEG`), the step counter is re-seeded from the encoder.
- **Cross-check.** 179.6° of flap at the 17.23:1 gearbox should take
  179.6 / 360 × 17.23 × 1 600 ≈ 13 750 microsteps. 13 600 is 1.1% short —
  normal for gearbox play while jogging. Over 20% off, `lim collapsed` warns: steps
  were lost, or the flap was moved by hand.
- **`lim_steps_per_rev`** exists so a change of microstepping does not break
  the span: at boot, if the driver's microstepping differs, `lim_span_steps` is
  scaled to match.
- **The range must fit within one turn of the shaft**, since the encoder cannot
  tell one turn from the next.

### Gearbox play: `lim_backlash`

Gears have a little slack between their teeth. When the motor **changes
direction**, it first turns a few microsteps through that slack before the flap
moves at all.

```text
motor:  ──────►  stop  ◄── 0 … backlash … ──◄◄◄ flap starts moving
                        (motor turns, flap does not)
```

`lim_backlash` is that slack in microsteps. Every move that reverses direction
is made that much longer, so the *flap* travels the distance asked for. 0 (the
default) means none is compensated — moves after a reversal come up short by
the slack, which the encoder then corrects on a `mot go`.

`lim play` measures it: it creeps one way, then the other, a few microsteps at
a time with the coils released between steps, and notes when the encoder sees
the flap start to move. It must be measured **before** the ends are stored,
because the counted span depends on it: `reset`, `lim play`, then `calibrate`.

### Arriving without overshooting: `lim_approach_deg`

How far before its target a `mot go` stops running at speed and starts to
creep. See [How mot go arrives](#how-mot-go-arrives). In short: the flap's own
weight can carry it through the gearbox play past where the motor stopped, so
the last `lim_approach_deg` degrees are crept a few microsteps at a time, the
encoder read after each, until it is within 0.1°. Default 4°; larger is safer
and slower; 0 = no creep, and the flap may overshoot first.

### Motor speed: `mot_speed_sps`

How fast the flap motor turns once it has accelerated, in microsteps per
second. With 1 600 microsteps per motor turn and the 17.23:1 gearbox, the
default 5 000 is about 3.1 motor turns a second = **65° of flap per second**, so
your 179.6° of travel takes about 3 s plus the ramp and the creep. The motor was
measured stable to 15 000 without load; keep well under that with the flap on
it. Acceleration (`mot accel`) is not stored.

### When the flap starts: `mot_early_start`

- **0 (default)** — the flap starts once the desk has stopped at the flap
  height (the height unchanged for 1.2 s).
- **1** — the flap starts the moment the desk is told to stop: at the release
  point of the approach, or when the stop key goes out. The desk's coast and
  the flap's travel overlap, which saves about a second per crossing.

Either way (and unless `desk_early_resume` is set) the recall is only re-sent once the desk is still **and** the
encoder confirms the flap is at its end. With 1, the flap motor runs while the
desk's motors are still running, so the driver needs a supply that stays on
while the desk moves — the controller board's power output does not (below).

### When the desk moves on: `desk_early_resume`

A flap move has a fast part and then a short creep onto its end
([How mot go arrives](#how-mot-go-arrives)).

- **0 (default)** — the recall is re-sent once the flap has **settled on its
  end**, confirmed by the encoder to within 0.1°. If the flap does not get
  there, the desk stays at the flap height.
- **1** — the recall is re-sent as soon as the **fast part is over**, and the
  creep finishes while the desk is already moving. It saves the creep time
  (about a second) per crossing.

With 1 the encoder is still asked first: the desk only goes on if the flap
really is within `lim_approach_deg` + 2° of its end. If it is not — the motor
stalled — the desk waits for the whole flap move, as with 0.

The price of 1: **if the creep then fails, the desk cannot be held back** — it
has already resumed, with the flap a few degrees short of its end. The console
says `FLAP MOVE FAILED in the creep — the desk had ALREADY resumed` and the LED
blinks red. And the creep runs while the desk's motors do, so, like
`mot_early_start`, it needs the driver on a supply that stays on during a move.

**A stall means a long creep.** If the motor cannot turn the flap during the
fast part it skips steps: the flap barely moves, and the creep — a few
microsteps at a time — then covers the whole travel, 20–30 s instead of 3. The
flap still arrives, and the firmware warns:

```text
[flap] STALL: the motor skipped most of the fast part (12896 steps crept).
```

Seen on 2026-10-02, and the cause was **the driver's supply**: the TMC2209 was
powered from the controller board's power output port, and the board **cuts
that output while the desk is moving** (confirmed with a meter). The motor lost
power mid-move, dropped out of step and could not recover at speed. Powered
from a separate supply it runs at the default 5000 without trouble —
[wiring.md](../../docs/wiring.md#power). If a stall shows up with a good supply, lower the speed
(`eeprom set mot_speed_sps 2500`) or soften the ramp (`mot accel 10000`).
