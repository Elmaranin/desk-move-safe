# Operation

## What it does

The panel is in charge. Every key frame it sends is forwarded to the main
board unchanged, and the height comes back to the panel directly over a wire
this firmware does not touch — so the display works whatever happens here.

Two things change that.

**The ceiling.** An UP command that would carry the desk past `DESK_CEILING_MM`
is replaced with idle. Because the firmware is in the path, that is a
guarantee rather than a request: it holds against a button held down, and
against any other part of this firmware getting it wrong.

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
after a reset — is the clean path. `presets stand 80.3` skips even that.

A **held** up/down press has no announcement, since nobody knows where the user
will let go, so that one is stopped as it approaches.

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
| `limits` | are min and max stored in flash |

A check can say **`not in this build`**, which is not a failure. "Not built" and
"broken" need different reactions, and collapsing them into one bool is how a
firmware ends up refusing to work because of a feature nobody has written yet.

**A failed check switches the flap off, not the desk.** Stopping a move at the
flap height and then failing to move the flap is strictly worse than never
stopping it — the user gets an interrupted move and a beep in exchange for
nothing. So the intercept goes quiet, `status` says why, and the panel drives the
desk exactly as it did before this board was fitted.

While `FLAP_DRIVES_MOTOR` is 0 the flap is a two-second dwell that touches no
motor, so none of the checks gate it; they are reported and nothing more. Setting
it to 1 in step 4 makes all three hard requirements at the same moment they start
to matter.

### `dev start`

Unlocks the commands that drive something directly or poke at a register:
`mot`, `tmc`, `go`, `forget`. Everything an installer needs — `flap`, `ceiling`,
`presets`, `coast`, `status`, `stop` — stays available in working mode, because
working mode is not a reduced console, it is the console for a board doing its
job.

Dev mode also **suspends the flap intercept**, for the same reason a failed check
does, from the other side: someone hand-driving the motor must not have the desk
take the bus out from under them because a preset was pressed in the next room.
Entering is refused mid-sequence; leaving stops the motor and releases it.

It is gone at the next reset.

## Console (USB CDC)

```text
status              height, what it is doing, every setting
flap [<cm>]         the height the desk is stopped at for the flap
flap on|off         whether it stops there at all
ceiling [on|off]    refuse UP past the safe limit, whatever the panel asks
presets             the stand and sit heights
presets stand|sit <cm>   set one (0 = forget it and re-learn)
coast <up> <down>   how far the desk runs on after being told to stop
go <cm>             move the desk to a height
stop                stop now
forget              erase the stored settings
```

Every setting shows itself when given no argument and changes nothing.

### What is stored in flash

The preset heights, both coast figures, the flap height and whether the flap
stop is enabled — one CRC-checked record in the last flash sector, restored at
boot before the bus runs.

Written once the settings have been untouched for a couple of seconds and
never mid-move: a flash write blanks interrupts for tens of milliseconds.

Not stored, and from `board_config.h` every boot: proxying (always on), the
ceiling, and the protocol constants.

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
3. `presets stand <cm>` and `presets sit <cm>` if you know them — otherwise
   press each one once and let it learn.
4. `flap 77` to set the height, `flap on`, and try a preset each way.

## Tuning

`coast` is how far the desk runs on after being told to stop. It is not the
same in both directions, and it does not need to be exact — anything that
lands inside `DESK_NEAR_MM` (10 mm) of the mark is close enough, and stopping
*short* is the safe side, since the flap then runs before the desk has crossed.

`go` reports where it actually landed, which is the number to tune against.

## Silencing the beep

The desk beeps once for every one-shot code it accepts, which is unavoidable
on the wire — but the buzzer can be turned off in the desk itself. On the
panel: hold `▼ + ▲` for 5 s, step to **`P06`**, press the **stand** button to
enter, set it to **0**, press **stand** to confirm. Alarms still sound.

Full parameter menu: [../../docs/CL103B-G_protocol.md](../../docs/CL103B-G_protocol.md) §8.

## Where the stepper goes

`run_flap()` in [`../src/flap_task.c`](../src/flap_task.c) is a dwell. It is
the only place that changes: everything else exists to get the desk stopped at
the right height with the bus in hand. The stepper's stored values have a
section waiting for them in `settings_t`, laid out field for field like the
bench rig's `limits.c` record.
