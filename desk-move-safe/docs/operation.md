# Operation

## What it does

The panel is in charge. Every key frame it sends is forwarded to the main
board unchanged, and the height comes back to the panel directly over a wire
this firmware does not touch — so the display works whatever happens here.

Two things change that.

**The ceiling.** An UP command that would carry the desk past `DESK_CEILING_MM`
is replaced with idle. Because the firmware is in the path, that is a
guarantee rather than a request: it holds against a button held down, and
against any other part of this firmware getting it wrong. A held UP is refused
a coast early (it runs on ~18 mm after release); a tap only if its 10 mm step
would end past the ceiling, so single taps reach 79.x cm under an 80 cm
ceiling. A refusal prints `[desk] UP refused at … — the ceiling is …` and
blinks the LED red. Preset recalls are not checked against it.

**The flap height.** A move that would carry the desk past it is stopped
there, the flap runs, and the move then continues to where it was going.

## How a preset is handled

A preset recall announces its destination about a second *before* the desk
moves, so the whole move is known in advance — there is no need to watch the
desk approach and try to stop it in time.

Once a preset's height has been seen, the recall is **not forwarded at all**.
The board never starts, there is nothing to abort, and nothing can overshoot a
height it has not begun travelling towards. The move is driven from here to the
flap height, and finished by re-issuing the recall, so the board lands on its
own stored height exactly and the user gets the position they asked for.

That re-issued recall is **one frame**, because that is what the code means to
the board: a real panel press puts `0x01` on the wire for a single frame and is
idle again on the next poll. Sent for two or three frames it is a button being
held, and the second frame cancels the recall the first one just started — the
desk sets off, travels about a centimetre and stops there. It survived some of
the time, depending on where the extra frame landed in the board's second of
start-up latency, which is exactly what an intermittent fault looks like.

The first press of each preset is the exception: its height is unknown, so the
recall is forwarded, the announcement is read, the desk is stopped, and the
height is learned and written to flash. Every press after that — including
after a reset — is the clean path. `eeprom set panel_stand_mm 803` skips even that.

**The presets are the only settings stored automatically**, and only when
they really change:

1. At startup the stored stand and sit heights are read once.
2. Every stand or sit recall that reaches the board is watched for *its own*
   announced destination — one that arrives after the recall. (The board's
   previous announcement is still held and must not be mistaken for it; that is
   how stand was once learned as the sit height.)
3. The announced height is compared with the stored one. **Different:**
   `[flap] preset stand: 803 -> 805 mm — stored`, written to flash once the
   desk is still. **Same:** nothing is written.
4. Saving a preset on the panel writes nothing either.
   The preset is only marked untrusted — no takeover relies on it — until its
   next recall, which either confirms it (`confirmed … nothing written`) or
   stores the new height.

This happens in dev mode, with the flap off and right after a flap move alike;
watching changes nothing on the bus. A recall issued while the desk is already
at that preset is not announced, so it teaches nothing.

Everything else is written only when asked: `eeprom set`, `calibrate`, `lim`,
`reset`. Two one-off exceptions, both after a firmware update: a record in an
older layout is rewritten in the new one, and the flap's step count is rescaled
if the microstepping changed.

**Whether the desk is stopped at all is read off the flap itself.** Above the
flap height the flap must be collapsed, below it expanded. A move heading across
is stopped only if the flap is not already at the end that side needs (the
encoder says, to within 3°) — however the desk got there, and with no memory of
earlier crossings to go stale.

**The crossing guard.** One thing must never happen: the desk above the flap
height with the flap not collapsed. The intercept above normally prevents it by
moving the flap. Where it does not act — `desk_flap_on 0`, dev mode, the
hands-off window after a flap move — a guard does, the same way the ceiling
works: a held UP, a tap or a preset recall that would carry the desk **up
across the flap height while the flap is not collapsed** is replaced with idle.
The console prints `[flap] REFUSED: going up past … with the flap not
collapsed`, the LED blinks red, and `desk` counts the refusals. A recall whose
height was never learned is refused too, since it cannot be judged. `go` refuses
the same move and says to run `mot go collapsed` first. Going **down** is never
refused: a collapsed flap below the flap height is out of the way.

**The flap height has a safe gap**, `FLAP_SAFE_GAP_MM` (20 mm): it is chosen
with room to spare, so anywhere within 2 cm past it still counts as "at the
flap height". A desk that landed there with the flap not yet set for the side it
is asked to go to — a stop a few millimetres long, or a flap move that failed —
gets the flap moved on the next press, right where it stands
(`[flap] at the flap height (… mm) with the flap not collapsed — moving it`).

**Further up than that** with the flap not collapsed — however it came about —
the desk may not go higher: every UP and every recall that does not lead down
is refused, with the same red blinks. Down is the way out; so is
`mot go collapsed`. The same applies inside the gap when the flap stop is off
or suspended, since nothing will move the flap then.

So with `desk_flap_on 0` the desk is not free to roam: it stays on the side its
flap allows, until the flap is moved by hand (`mot go collapsed`) or the flap
stop is switched back on.

A **held** up/down press has no announcement, since nobody knows where the user
will let go, so that one is stopped as it approaches. Then the flap moves, and
the bus goes back to the panel (`[flap] a held move — the flap, then the panel
has it back`): keep holding, or press again, and the desk carries on.

A **recall that slipped past the takeover** — pressed during the hands-off
window after a flap move, say — is still stopped as it approaches, and then
treated as the takeover would have treated it: the flap runs and the recall is
sent again to finish the move (`[flap] it is a recall to … — the flap, then the
recall again`). Before 2026-10-02 it was stopped like a held press and left
there, so the desk stayed at the flap height.

A **tap** of up/down is a fixed 10 mm step that ends by itself, and the only
way to stop one is a tap the other way — which is a step back. So a tap is
judged by where it will end, not by how close it gets:

- ending short of the flap height: left alone, however close it gets;
- ending past it, with the flap not yet at the end that side needs: **not
  sent** — the flap is moved instead, the desk standing where it is
  (`[flap] a step from … would pass … — moving the flap first`). The next tap
  finds the flap in place and goes through. If the press was the start of a
  hold, the desk sets off once the flap has moved.

Until 2026-10-02 a manual crossing never moved the flap: a second tap simply
went through, and a held move was stopped and released, so the desk could be
walked past the flap height with the flap still expanded.

Before this, a tap from 742 mm toward a flap at 770 was "stopped" at 752 with a
down tap, and the desk stepped straight back to 742.

## Working mode and dev mode

**A reset always produces working mode.** There is no stored mode, no jumper and
no boot flag — the mode is a static bool that reset zeroes. A mode that can be
stored is a mode a board can be left in, and the board that gets left in it is
always the one nobody is watching.

At boot the firmware asks three questions, and `dev` or `status` repeats them:

| check | what it means |
|---|---|
| `driver` | can the stepper driver be reached — only askable over UART |
| `encoder` | does the MT6835 answer |
| `limits` | are both ends stored in flash |

A check can say **`not in this build`**, which is not a failure. "Not built" and
"broken" need different reactions, and collapsing them into one bool is how a
firmware ends up refusing to work because of a feature nobody has written yet.

**A failed encoder or limits check locks the desk.** The flap sits in the
desk's path, and a flap left open is in the way: until the firmware can see the
flap and knows where its ends are, it cannot tell whether a move is safe. So
every UP, DOWN and preset recall — from the panel or from `go` — is replaced with
idle, the board's LED blinks red at each refused press, and `status` shows
`DESK LOCKED` and why. A stuck desk is the cheaper
failure. Decided 2026-10-02 while the flap is being fitted; the earlier rule
(switch the flap off and let the panel drive) may return once the flap is in
use. The driver check does not lock the desk — in standalone mode it cannot be
asked. [commands.md](commands.md#what-it-needs-to-move) lists exactly what it needs and how to calibrate; `reset` clears just that.

The same checks gate the flap move itself — and with a UART-linked driver, the
driver check too (`FLAP_DRIVES_MOTOR`, 1 since 2026-10-02).

### `dev start`

Unlocks the commands that drive something directly, poke at a register or
change the flap's stored range: `mot`, `tmc`, `enc`, `lim expanded`/`collapsed` and the
rest of `lim`, `go`, and `eeprom set` for the `lim_*`, `mot_*` and `desk_early_resume`
parameters. Everything an installer needs — `status`, `calibrate`, `reset`, `desk`, `debug`,
`eeprom` and `eeprom set` for the `panel_*` and `desk_*` parameters, `lim`,
`stop` — stays available in working mode, because
working mode is not a reduced console, it is the console for a board doing its
job.

Dev mode also **suspends the flap intercept**, for the same reason a failed check
does, from the other side: someone hand-driving the motor must not have the desk
take the bus out from under them because a preset was pressed in the next room.
Entering is refused mid-sequence; leaving stops the motor and releases it.

It is gone at the next reset.

## Console (USB CDC)

Every command, with what it does and which mode it needs:
**[commands.md](commands.md)**. `?` on the board prints the short list; the
build fails if the two disagree.

### What is stored in flash

The preset heights, both coast figures, the flap height and whether the flap
stop is enabled, the flap's calibration (both ends angle, the steps between
them, backlash, approach) and the flap motor's speed — one CRC-checked record
in the last flash sector, restored at boot before the bus runs. `eeprom` lists
every field; [commands.md](commands.md#parameters-explained) explains each one.

Written once the settings have been untouched for a couple of seconds **and
the desk has been still for three**: no height change and no move key or recall
sent, whoever started the move — a flash write blanks interrupts for tens of
milliseconds. Before 2026-10-02 only the firmware's own moves held it off, so a
recall passed to the board could be interrupted by a save at mid-height.

Not stored, and from `board_config.h` every boot: proxying (always on), the
ceiling switch, `mot accel`, and the protocol constants.

## Bring-up

1. Wire it per [../../docs/wiring.md](../../docs/wiring.md) — **one cut wire**,
   and mind the swapped `RX`/`TX` labels on the panel.
2. Power up with a terminal attached. `status` should show a live height and
   frames on both lines.
   - `NOT TRANSMITTING` means the panel has not been heard, so GP4 is still
     high-Z and the desk cannot move. Check the ground first: one missing
     ground leaves both lines silent while the panel and board still talk to
     each other perfectly.
   - `height never seen` with the panel heard means GP1 is on the wrong pin.
3. **Calibrate the flap, or the desk will not move:** `calibrate`, and follow it —
   jog the flap to expanded, `done`, to collapsed, `done`. `status` should no longer say
   `DESK LOCKED`. [commands.md](commands.md#calibrating) has the detail.
4. `eeprom set panel_stand_mm <mm>` and `eeprom set panel_sit_mm <mm>` if you
   know them — otherwise press each one once and let it learn.
5. `eeprom set desk_flap_mm 770` for the height, `eeprom set desk_flap_on 1`,
   and try a preset each way.

## Tuning

Coast (`desk_coast_up_mm`, `desk_coast_down_mm`) is how far the desk keeps
moving after being told to stop, so a stop meant to land at a height is sent
that much early — [commands.md](commands.md#parameters-explained) has a picture.
It is not the same in both directions, and it does not need to be exact —
anything that lands inside `DESK_NEAR_MM` (10 mm) of the mark is close enough,
and stopping *short* is the safe side, since the flap then runs before the desk
has crossed.

`go` reports where it actually landed, which is the number to tune against.

## Silencing the beep

The desk beeps once for every one-shot code it accepts, which is unavoidable
on the wire — but the buzzer can be turned off in the desk itself. On the
panel: hold `▼ + ▲` for 5 s, step to **`P06`**, press the **stand** button to
enter, set it to **0**, press **stand** to confirm. Alarms still sound.

Full parameter menu: [../../docs/CL103B-G_protocol.md](../../docs/CL103B-G_protocol.md) §8.

## The flap move

The flap has two ends, set by `calibrate`: **expanded** and **collapsed**. Below the
flap height (`desk_flap_mm`) it is expanded; above it, collapsed. So when a
move crosses the flap height:

| Desk going | Flap moves to |
|---|---|
| up (e.g. sit → stand) | COLLAPSED |
| down (e.g. stand → sit) | EXPANDED |

The direction comes from where the desk is headed — the recall's destination.
Without one (a recall caught late, whose height was never learned) the flap goes
to whichever end it is further from.

**When the flap starts is a stored parameter, `mot_early_start`.** 0 (the
default): once the desk has stopped at the flap height. 1: the moment the desk
is told to stop, so its coast and the flap's travel overlap and the crossing is
about a second shorter. 1 needs the flap driver on a supply that stays on while
the desk moves — the controller board's power output is cut during a move, which
stalled the motor (2026-10-02).
[commands.md](commands.md#when-the-flap-starts-mot_early_start) has the detail.

[`../src/flap_task.c`](../src/flap_task.c) moves the flap like `mot go`:
range-checked, fast to within `lim_approach_deg` of the end, then crept onto it
under the encoder, with the guard watching
([commands.md](commands.md#how-mot-go-arrives)). The console prints
`[flap] moving the flap to COLLAPSED`, then, once the flap is still,
`[flap] COLLAPSED (+0.05 deg from the end)` — and only
then is the recall re-sent to finish the desk's move. If the desk does not
reach the flap height (stalled, could not be stopped, `stop`), the flap move is
stopped too.

**When the desk moves on is a stored parameter too, `desk_early_resume`.** 0
(the default): once the flap has settled on its end. 1: as soon as the flap's
fast move is over and the encoder puts it within reach of its end; the creep
then finishes while the desk moves. With 1 a creep that fails can no longer hold
the desk back. [commands.md](commands.md#when-the-desk-moves-on-desk_early_resume)
has the detail.

**If the flap does not get there** — the move is refused, times out after
`FLAP_MOVE_TIMEOUT_MS` (20 s), `stop` is typed, or it does not settle within
0.1° — **the desk stays at the flap height** and the recall is not re-sent: the
flap is somewhere in the desk's path, and moving on could hit it. The console
says `FLAP MOVE FAILED`, the LED blinks red, and the panel has the desk back.
`lim` says where the flap actually is.

A flap that only gets there by creeping the whole way is reported as
`[flap] STALL` ([commands.md](commands.md#motor-speed-mot_speed_sps)).

The motor is left energised after a move, holding the flap where it is.

`FLAP_DRIVES_MOTOR 0` in `board_config.h` brings back the two-second dwell that
stood in for the move, for bench work on the desk side alone.
