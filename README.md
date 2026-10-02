# table-controller

A standing desk with a folding flap on it. The flap has to be moved out of the
way at a particular height, so something has to notice the desk approaching
that height and stop it there.

The desk is a **Völundr X2DM-SE** with a **CL103B-G** control board. Its panel
talks to that board over a 4-wire UART, and the whole project rests on having
worked out what they say to each other.

```
docs/                 what was learned — protocol, wiring, roadmap, photos
desk-move-safe/       PRODUCTION firmware. The one job, small console
usb-uart-tools/       Python that drives the desk from a laptop, no MCU
3d-models/            printed parts
```

## Where to start

| you want to | read |
|---|---|
| build and install the thing | [desk-move-safe/README.md](desk-move-safe/README.md) |
| wire it up | [docs/wiring.md](docs/wiring.md) |
| understand the desk's protocol | [docs/CL103B-G_protocol.md](docs/CL103B-G_protocol.md) |
| try something without flashing | [usb-uart-tools/README.md](usb-uart-tools/README.md) |
| know what is next | [docs/roadmap.md](docs/roadmap.md) |

## The two traps

**The panel header's `RX`/`TX` labels are swapped.** The pin marked `RX` is the
one the panel transmits on. Trust them and both leads land on the wrong pins,
and the failure is silent in both directions while the desk carries on working.

**A board that is not proxying is a desk that cannot move.** The panel-TX wire
is cut, so everything the main board hears comes from the firmware. That is
why proxying is on at boot — and why the failure mode is a stationary desk
rather than a runaway one.
