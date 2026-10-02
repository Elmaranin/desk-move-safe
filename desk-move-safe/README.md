# desk-move-safe

Production firmware. Keeps a standing desk from carrying a folding flap past
the height the flap has to be moved at.

It sits in the cut panel-TX wire between the desk's control panel and its
CL103B-G main board, and forwards both directions. **The panel drives the desk
exactly as it did before.** When a move would take the desk past the flap
height, this takes the bus, stops it there, runs the flap, and lets the move
finish.

For the bench rig this was worked out on — frame tracing, raw byte dumps,
cadence and coast measurement, manual injection, panel emulation — see
desk-lab, which is kept locally and not published. None of that is here.

## Documentation

- [../docs/wiring.md](../docs/wiring.md) — board, pin map, **what to cut**
- [../docs/CL103B-G_protocol.md](../docs/CL103B-G_protocol.md) — the desk protocol
- [docs/operation.md](docs/operation.md) — what it does at the desk, modes, bring-up
- [docs/commands.md](docs/commands.md) — **every console command**, and every stored parameter

## Build

Needs **Pico SDK 2.2.0** under `~/.pico-sdk` (what the Raspberry Pi Pico VS Code
extension installs) and a **FreeRTOS-Kernel** checkout pointed at by
`FREERTOS_KERNEL_PATH` (`build.sh` carries a default).

```sh
./build.sh          # -> build/desk-move-safe.uf2
./flash.sh          # build + flash over USB, no BOOTSEL button
./flash.sh -n       # flash what is already built
```

The first flash onto a board running unrelated firmware needs one BOOTSEL
press; every flash after that is button-free.

## Two things that will bite

**The panel header's `RX`/`TX` labels are swapped** — the pin marked `RX` is
the one the panel transmits on. Both leads end up on the wrong pins if you
trust them, and the failure is silent in both directions while the desk keeps
working normally. [../docs/wiring.md](../docs/wiring.md) has the photograph.

**A board that is not proxying is a desk that cannot move.** The wire is cut,
so everything the board hears comes from here. That is why proxying is on at
boot and cannot be switched off from the console, and why the failure mode is
a stationary desk rather than a runaway one.
