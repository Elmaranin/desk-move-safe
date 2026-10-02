# Volundr X2DM-SE — CL103B-G Controller Protocol

Complete reverse-engineered protocol for the desk control board, verified by bus
capture. Everything here was confirmed empirically during analysis, not assumed.

**Board:** `DSK_CL103B_Main_V1.2`
**MCU:** Nations Technologies N32G031 (Cortex-M0, 3.3 V, STM32-compatible SWD)
**Panel link:** detachable control panel ↔ main board over UART

---

## 1. Physical link

- **UART, 9600 baud, 8N1, 3.3 V logic.**
- 4-pin header labelled (from the panel's viewpoint) `3.3V RX TX GND`.
- The panel is the *polling master* in normal use: it sends a request every
  poll tick; the board answers with status/height.
- **Panel poll cadence: ~104 ms** (measured 102–106 ms, ≈9.6 Hz, very stable).
  A second desk measured 103–105 ms steady, with single gaps stretching to
  115 ms around a button press — the panel's key scan costs it a few ms.
  Anything replacing the panel should poll at this rate, not faster.
- Board streams height in response to being polled — by the real panel OR by any
  device that sends the poll frames. With the panel removed, a controller that
  streams the idle poll keeps the board reporting height normally.
- **Stop polling and it stops answering.** This makes silence a bad way to stop
  the desk: it takes the height stream down with it, and a reading that has
  stopped changing is indistinguishable from a desk that has stopped moving.
  Anything that needs to watch the desk must keep sending the idle poll —
  which is equally "no command" for a held move, and costs nothing.

### Header notes (this board)
- A separate 5-pin black header is the **SWD debug port** (SWDIO/SWCLK/NRST/3V3/GND),
  not a UART. Serial attempts on it see nothing — expected; SWD is silent until a
  debugger drives it.
- The `3.3V RX TX GND` header is the panel UART used throughout this document.

### ⚠️ The board's power output is switched off while the desk moves

Measured 2026-10-02: the controller board's power output port loses its voltage
for as long as the desk's motors run, and gets it back when they stop. It is not
a tap on the supply input. Anything that has to work *during* a move must not
be powered from it — see [wiring.md](wiring.md#power).

### ⚠️ The panel header's RX and TX labels are SWAPPED

Photographed on the panel PCB of this desk (`docs/desk/2026-09-22 10-57-31.JPG`):

```
   3.3V    RX      TX     GND      <- silkscreen on the PANEL
            |       |
            |       └── the panel RECEIVES here (board -> panel, height)
            └────────── the panel TRANSMITS here (panel -> board, keys)
```

So the labels name the **main board's** role, not the panel's. The pin marked
`RX` is the one the panel drives.

The main board's own header is labelled by function and is not swapped
(`docs/desk/2026-09-22 10-56-03.JPG`): `TX to panel`, `RX from panel`.

The practical consequence, and it is the trap that costs an evening:

| you want to | use the panel pin marked |
|---|---|
| **listen** to the panel's key frames | `RX` |
| **drive** the main board (replace the panel) | `RX` |
| **listen** to the board's height frames | `TX` |
| **drive** the panel's display | `TX` |

Assuming the labels are from the panel's viewpoint puts both leads on the
wrong pins at once. The failure is silent in both directions — the input
landing on an output pin is fought and never received, the output landing on
an input pin drives nothing — while the panel and board carry on over the
wires that are still intact. The desk keeps working, and nothing is heard.

---

## 2. Frame format (both directions)

```
[ sync0 sync1 ] [ len ] [ type ] [ payload... ] [ cksum_hi cksum_lo ]
     2 bytes       1        1        variable          2 bytes
```

- **Sync encodes direction:**
  - `55 AA` = **main board → panel** (status / height)
  - `AA 55` = **panel → main board** (key reports / commands)
- **len** = total frame length in bytes = `0x08` for all frames observed.
- **type** = message id (`0x02` = height report; `0x01` = key report).
- **checksum** = 16-bit **big-endian sum of ALL preceding bytes**, sync included.

Verify / generate:

```python
def cksum_ok(f):                      # f = 8-byte frame
    s = sum(f[0:6])
    return (s >> 8) == f[6] and (s & 0xFF) == f[7]
```

Both directions use identical 8-byte framing.

---

## 3. Board → panel : height report  (`55 AA`, type 0x02)

```
55 AA 08 02  HH HL  CK CK
             └──┬─┘
     14 bits of millimetres, big-endian, with FLAGS in the top 2 bits
```

**The height field is not a plain u16.** Captured while saving a preset,
checksum valid:

```text
 121.989  105.0  board 55 AA 08 02 42 F5 02 40   0x42F5 = 17141 "mm"
```

`0x42F5 & 0x3FFF` = **757**, which was exactly the desk's height at that
moment. Bit 14 is a flag the board raises around a save — most likely the
"stored / beeping" acknowledgement, since it appeared only while save codes
were going out and cleared afterwards. Bit 15 has never been observed set.

Mask with `0x3FFF` before believing a height. Reading the field raw is not a
cosmetic bug: 17141 mm handed to a closed-loop move is a desk being told it is
sixteen metres too high, and the correction is downward and fast.

- Example: `55 AA 08 02 02 CC 01 D7` → 0x02CC = 716 = **71.6 cm**
  (checksum 55+AA+08+02+02+CC = 0x01D7 ✓).
- Streamed continuously; repeats the same frame while stationary.
- During motion, roughly one frame per mm of travel.
- Display truncates to whole cm; the bus carries full mm resolution.

### Motion characteristics
- **Down is faster than up.** Descending ≈ 2–3 mm/frame; ascending ≈ 1 mm/frame
  (gravity assists the motors). Size any timing margins against the *down* speed.
- **Post-command coast:** after the last motion command the desk keeps moving
  briefly in its direction of travel — roughly 8–15 mm depending on run length
  and load. Open-loop stops must aim short; closed-loop must re-read and settle.

### Target announcement (preset pre-move)
Before an autonomous preset move, the board briefly emits height frames carrying
the **target** value (a static height differing from current) before it starts
driving. Gives ~1 s of look-ahead: you can know the destination before motion.

Captured on an RP2350 emulating the panel, after a one-frame `0x01`:

```text
   0.000       -  board 55 AA 08 02 02 E6 01 F1  height 742 mm   <- resting
                  board  ... x50 more over 5.1 s
   5.310   110.0  board 55 AA 08 02 03 08 01 14  height 776 mm   <- TARGET
                  board  ... x9 more over 0.8 s
   6.340   100.0  board 55 AA 08 02 02 E8 01 F3  height 744 mm   <- real, climbing
   6.450   110.0  board 55 AA 08 02 02 E9 01 F4  height 745 mm
```

Three things this pins down:

- The **target is announced for ~0.8–1.0 s** as a static value, then the
  stream reverts to the real height and the desk starts moving.
- **It arrives as a jump no desk could make.** Captured with the real panel:
  the desk sitting at 743 mm, one frame later the stream says 803, a second
  after that 745 and climbing for real. That +60 mm in a single frame is how
  the announcement is told apart from motion — real travel is 2–3 mm per frame
  descending and 1 ascending, so anything past about 10 mm did not happen
  mechanically. Filter on that and the announcement stops being a hazard and
  becomes the longest look-ahead this bus offers.
- Treating it as a height is briefly wrong by six centimetres: direction flips
  twice, a ceiling check thinks the desk is above the limit, and an intercept
  computes its trigger against a height the desk never occupied.
- The board sat at 742 mm for **5.3 s after the command** before announcing
  anything. A recall is not refused quickly — "nothing happened yet" needs
  several seconds of patience before it means "ignored".
- Height then advances ~1 mm per frame climbing, matching the "up is slow"
  note above.

---

## 4. Panel → board : key report  (`AA 55`, type 0x01)

```
AA 55 08 01 00  KK  CK CK
                └┬┘
              key code (byte 5; byte 4 always 00)
```

### Confirmed code table

| Code | Action              | Behaviour                                             |
|------|---------------------|-------------------------------------------------------|
| 0x00 | idle / no key       | streamed constantly as poll / keep-alive              |
| 0x01 | recall **stand**    | one-shot; board self-drives to stored height          |
| 0x02 | recall **sit**      | one-shot; board self-drives to stored height          |
| 0x03 | save **stand**      | store current height → stand slot (5 s button hold)   |
| 0x04 | save **sit**        | store current height → sit slot (5 s button hold)     |
| 0x05 | **down** step       | one frame = -10 mm; also the press edge of 0x06        |
| 0x06 | **down**, held      | must repeat every poll tick to keep moving            |
| 0x07 | **up** step         | one frame = +10 mm; also the press edge of 0x08        |
| 0x08 | **up**, held        | must repeat every poll tick to keep moving            |
| 0x0E | panel **wake**      | panel coming out of sleep, as its display lights up   |

### A single edge frame is a 10 mm STEP command

`0x05` and `0x07` are not markers. One frame of either, with nothing else sent
before or after, moves the desk a fixed distance. Measured on an RP2350 in the
panel-TX path, injecting exactly one frame into the stream from a standstill:

```text
  79.919   ours AA 55 08 01 00 07 01 0F  key up edge     <- one frame
  80.024   ours AA 55 08 01 00 00 01 08  key idle
  80.679  board 55 AA 08 02 02 F2 01 FD  height 754 mm   <- 0.76 s later
   ...     1 mm per frame for 1.03 s ...
  81.709  board 55 AA 08 02 02 FB 02 06  height 763 mm   <- +10 mm exactly
```

| | up (`0x07`) | down (`0x05`) |
|---|---|---|
| travel | **+10 mm** | **−10 mm** |
| latency before motion | 0.76 s | 0.75 s |
| rate during the step | 1 mm/frame | 1 mm/frame |

Symmetric in both directions, unlike a held move, where down runs 2–3 mm/frame
against up's 1. This is the panel's fine-adjust: a tap of the button steps a
centimetre.

**Consequences for anything driving this bus:**

- **A tap is never free.** Sending an edge code to "interrupt" something costs
  a centimetre of travel in that direction. Interrupting a descent with a Down
  tap asks the desk to descend another centimetre.
- **0.75 s of latency before anything happens**, which is around seven poll
  frames. Code that sends a tap and looks for movement on the next frame will
  conclude it was ignored.
- **It is a precise positioning primitive.** Ten millimetres, repeatable, no
  coast to allow for — the creep phase of a closed-loop move can use taps
  instead of timed pulses.

### The motion codes are two pairs, edge then held

```text
        edge   held
down    0x05   0x06
up      0x07   0x08
```

Both edge codes were confirmed by hand on an RP2350 tap: a single Down tap
gives `0x05`, a single Up tap gives `0x07`. That closes out the older reading
of `0x07` as an "up transient, exact meaning not pinned down" — earlier
captures only ever caught it at press edges during a held move, so it looked
like a side effect rather than a code in its own right. Read the group as
`0x05..0x08` = {down, up} x {edge, held} and nothing in it is unexplained.

Consequence for anything that watches the bus: **a tap can move the desk
without a held code ever appearing.** Logic that treats only `0x06`/`0x08` as
"the user is driving the desk" misses short presses entirely.

### 0x0E — panel wake

Emitted by the panel as it comes out of sleep and starts displaying a height,
not in response to a button. Not yet known whether the board requires it, or
merely tolerates it, before it will answer polls — which matters for
panel-free operation (§6): a controller replacing the panel may need to send
it once at startup. Untested.

### Prebuilt frames (checksums verified)

```
idle        AA 55 08 01 00 00 01 08
recall stand AA 55 08 01 00 01 01 09
recall sit   AA 55 08 01 00 02 01 0A
save stand   AA 55 08 01 00 03 01 0B     # overwrites stored stand height
save sit     AA 55 08 01 00 04 01 0C     # overwrites stored sit height
down edge    AA 55 08 01 00 05 01 0D
down (hold)  AA 55 08 01 00 06 01 0E
up   edge    AA 55 08 01 00 07 01 0F
up   (hold)  AA 55 08 01 00 08 01 10
panel wake   AA 55 08 01 00 0E 01 16
```

Note the trap in that list: the *checksum low byte* of `down (hold)` is `0E`,
the same value as the `panel wake` **key code**. A key code of `0x0E` read out
of a misaligned parse would look identical to a real panel-wake frame. The
checksum is what tells them apart, so validate it before believing any code —
`AA 55 08 01 00 0E 01 16` is a wake, and anything else ending in `0E` is not.

### Behaviour notes
- **Up / Down are HELD commands:** motion continues only while the code streams
  at the poll cadence; revert to idle (0x00) or stop sending to halt.
- **Recall presets (0x01/0x02) are one-shot, and "once" means ONE FRAME.**
  Captured from a real panel on an RP2350 tap:

  ```text
     0.000       -  panel AA 55 08 01 00 00 01 08  key idle
                    panel  ... x58 more over 5.9 s
     6.160   115.0  panel AA 55 08 01 00 01 01 09  key recall stand
     6.265   105.0  panel AA 55 08 01 00 00 01 08  key idle
  ```

  A press of the stand button puts `0x01` on the wire for exactly one frame,
  and the next poll 105 ms later is already idle. Same for sit. No edge code
  precedes it — unlike up/down, the preset buttons emit a single code — and no
  `0x0E` is involved.

  **This is the thing to get right when emulating the panel.** A controller
  that streams the recall code for several frames is not sending a tap, it is
  holding the button, and a held preset button is the *save* gesture on this
  panel. A laptop script that streamed it for three frames could save presets
  but could never recall one.

  **A single edge frame aborts an in-progress preset.** Captured end to end: a
  stand recall heading for 803 mm, one `0x07` frame injected at 749 mm, and the
  desk stopped at 764 and stayed — 39 mm short of its target. One frame is
  enough; it does not have to be held.

  The abort is not instant. From 749 the desk travelled 15 mm over 3 s,
  decelerating — visible as the height repeating each value more often as it
  slows. Size any stop-short margin against that 15 mm, not against zero.

  So autonomous moves are both *suppressible* (don't forward 0x01/0x02) and
  *interruptible* (one injected edge frame).
- **Save (0x03/0x04) is ONE FRAME, like recall.** The ~5 s button hold is a
  *panel-side* gesture: the panel times the press locally and puts a single
  save code on the wire at the end of it. How long the button was held never
  appears on the bus.

  A controller that streams the save code for five seconds instead sends ~48
  save commands, and the board stores and beeps for a good number of them —
  measured at about ten beeps for one intended save. The board beeps once per
  save it accepts, so **more than one beep means more than one frame.**

  This is the same rule that governs recall, learned twice: **the panel emits
  one-shot codes exactly once.**
- **0x07** appears at the start/edge of up-presses (a tap transient). The reliable
  motion codes are the *held* values 0x06 / 0x08; treat 0x07 as an edge marker.

### Recall drive speed (important for tracking completion)
A recall self-drives at the normal motion speed — **upward is slow (~1 mm/frame)**.
Code that waits for a recall to finish must require *sustained* stability (≥ ~2 s
of no change) after motion is seen, or a brief plateau during the slow climb can
be mistaken for arrival.

---

## 5. Electrical / tapping rules

- 3.3 V logic. The header's `3.3V` pin is **power, not signal level** — never feed
  it into a signal line; use a 3.3 V USB-UART adapter.
- **Listen-only tapping:** input pins only. Do not drive a line the panel or board
  is already driving — two push-pull outputs on one wire corrupt frames and can
  freeze the panel (observed).
- Put a **1 k–10 k series resistor** in every probe / inject lead. Reads fine at
  9600 baud but prevents a short or drive-fight from disturbing the bus.
- **To inject commands you must be the sole driver of the board's RX line:**
  either remove the panel (replace it), or man-in-the-middle it (cut the panel's
  TX so it reaches only your controller's RX; your controller's TX drives the
  board). Do not inject while the panel is still wired straight to the board.
- Single-adapter software must **serialize port access** (one lock guarding read
  and write); concurrent read+write on one pyserial port raises "device reports
  readiness to read but returned no data" on macOS.
- Power the controller from ONE supply (its own USB, or the board's 5 V — not both);
  share ground.

---

## 6. Panel-free operation (replace the panel)

Confirmed working: a controller that streams the idle poll keeps the board
reporting height, so the panel is not required.

- **Poll continuously at ~104 ms** with the idle frame so height keeps flowing.
- To move: stream UP / DOWN at the poll cadence; stop by reverting to idle.
- To recall: send the one-shot recall frame, then track the self-driven move.
- Reading height back and closed-loop positioning both work with the panel absent.

---

## 7. Duty cycle, sleep, and the other things the manual documents

From the Völundr X2DM-SE manual (`docs/desk/2026-07-07 18-11-27.JPG` and
`18-11-37.JPG`). None of this is on the bus, but all of it changes how the bus
behaves.

### Thermal protection
**4 minutes of continuous running inside an 18-minute window** trips it — not
the "2 min on / 18 min off" estimated earlier. The display shows `HOT`, every
panel function is blocked and the buttons stop responding. It clears by itself
after 18 minutes, or on a power cycle. A lockout refuses the physical panel
too, which is how it is told apart from a logic bug.

### Energy-saving mode — and it ignores movement
After **30 seconds with no button press** the panel's display goes dark and the
controller drops to low power. Any button wakes it. The manual is explicit:

> **In energy-saving mode, movement commands are not executed.**

This matters for anything driving the bus. A controller that has been streaming
idle polls for a while and then sends a move may have that move ignored, with
the frame serving only to wake the desk. It is the most likely explanation for
the **~0.75–1.0 s** measured between an injected step frame and any movement
(§4) — the first frame wakes, the desk acts after. Untested: whether idle poll
frames count as activity and hold sleep off, or whether only key codes do.

**A move injected toward the board does not wake the panel's display.**
Observed 2026-10-02 with desk-move-safe's `go`: the desk moved and the board
kept sending height frames on the uncut height wire, but a sleeping panel stayed
dark throughout. Woken by one of its own buttons first, it followed the move
correctly. The panel's sleep is decided by the panel's own key presses; nothing
sent to the board changes it. Waking it from firmware would mean driving the
panel's `TX`-marked pin, which needs that wire cut first.

### Child lock
`stand + sit + ▲` held 5 s shows `LOC` and blocks raise/lower;
`stand + sit + ▼` held 5 s releases it. Worth knowing before concluding the
desk is ignoring the bus.

### Error codes
On error: 2 beeps, then lockout; 2 beeps again when it clears.

| | | | |
|---|---|---|---|
| `E01` motor 1 open | `E02` motor 2 open | `E03` motor 1 overload | `E04` motor 2 overload |
| `E05` motor 1 Hall unplugged | `E06` motor 2 Hall unplugged | `E08` obstacle detected | `E09` large position discrepancy |
| `E10` high voltage | `E11` low voltage | `E12` motor 1 Hall open | `E13` motor 2 Hall open |
| `E14` overload protection | | | |

Most clear with the Down button held 5 s, or a power cycle. Whether any of
these appear on the UART is not known — the height field's flag bits (§2) are
the obvious place to look.

## 8. Settings menu (P00–P06) — what can be changed in the desk itself

Hold `▼ + ▲` for 5 s to enter. `▲`/`▼` steps through P00…P06, the **stand**
button enters and confirms a parameter, and 5 s of inactivity saves and exits.

| Menu | Param | Range | Default | Notes |
|---|---|---|---|---|
| Units | `P00` | 0/1 | 0 | 0 = cm, 1 = inches |
| Min height | `P01` | 73 cm … (P02−10) | 73 cm | 1 cm steps |
| Max height | `P02` | (P01+10) … 122 cm | 122 cm | 1 cm steps |
| Tabletop thickness | `P03` | 0.0 … 10.0 cm | 0.0 | offsets the displayed height |
| Rise sensitivity | `P04` | 0–10 | 5 | obstacle protection; 0 = off, **higher number = less sensitive** |
| Descent sensitivity | `P05` | 0–10 | 5 | as P04 |
| **Buzzer** | `P06` | 0/1 | 1 | **0 = off except alarms** |

Three of these are directly useful to a project that drives the bus:

- **`P06 = 0` silences the beep.** The desk beeps once per one-shot code it
  accepts, which is unavoidable on the wire — but it does not have to be
  audible. Alarms still sound.
- **`P01`/`P02` are the desk's own travel limits**, and they are what
  `DESK_MIN_MM` / `DESK_MAX_MM` should mirror. Defaults are 730–1220 mm.
- **`P03` shifts the displayed height.** If it is non-zero, the number on the
  bus and the physical height of the top differ by that much. It was 0.0 on
  this desk.

### Confirmed: a short press is 10 mm

The manual states it outright — short press on `▲`/`▼` raises or lowers by
**10 mm**, long press moves continuously. That is the same 10 mm measured on
the wire for a single `0x07`/`0x05` frame (§4), from the other direction.

### Reset / calibration
Needed if the displayed height stops matching reality, or a leg is replaced.
Hold `▼` 5 s → display shows `RST`; press `▼` again to run it. The desk drives
to its lowest point, rises 10 mm, beeps, and shows the minimum height.

---

## 8. Positioning approach (open-loop coast → closed-loop creep)

Because of post-command coast, reliable positioning uses two phases:

1. **Coarse:** drive fast toward the target, stop short by the coast distance
   (~15 mm) so momentum carries in. Skip this phase entirely if the move is
   smaller than the coast distance (otherwise the desk overshoots or no-ops).
2. **Creep:** short pulses (≥ ~2 poll periods each ≈ 0.22 s) with a re-read
   between each, approaching at near-zero speed until within a few mm. No coast;
   works for moves of any size and cleans up the final error.

Report the settled height only after the desk is stably quiet, and take a final
fresh reading so it matches what the next command will see (accounts for coast).

### Key parameters (this desk)
- Poll / command cadence: **0.104 s**
- Coast allowance: **~15 mm** each direction (tune per run length)
- Creep pulse: **~0.22 s** (≥ 2 poll periods)
- Settle: require ~1.2 s continuous no-change; presets need ≥ 2 s after motion

---

## 9. Complete command summary (copy-paste)

```
Link:   9600 8N1, 3.3V, poll every ~104 ms
Frame:  55AA/AA55 | 08 len | type | payload | 16-bit BE sum over all prior bytes

Board→panel (55 AA):
  type 0x02  height = u16 BE mm

Panel→board (AA 55, type 0x01, code in byte 5):
  0x00 idle      0x01 recall stand   0x02 recall sit
  0x03 save stand 0x04 save sit
  0x05 down edge  0x06 down(held)     0x07 up edge   0x08 up(held)
  0x0E panel wake

Prebuilt AA 55 frames:
  idle  aa5508010000 0108      recall stand aa5508010001 0109
  recall sit aa5508010002 010a save stand aa5508010003 010b
  save sit  aa5508010004 010c
  down  aa5508010006 010e      up  aa5508010008 0110
  down edge aa5508010005 010d  up edge aa5508010007 010f
  panel wake aa550801000e 0116
```
