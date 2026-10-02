# usb-uart-tools — driving the desk from a laptop

Python that talks to the desk's bus through a **USB-UART adapter**, with no
microcontroller involved. This is where the protocol work started, and it is
still the quickest way to try something without flashing anything.

The panel is unplugged and the adapter takes its place:

```
  adapter TX --[1k]--> board RX     (we send the poll and the key frames)
  adapter RX <-------- board TX     (we read the height)
  GND ---------------- GND          3.3 V logic, 9600 8N1
```

Mind the panel header's labels — `RX` and `TX` on it are swapped relative to
the panel's own role. [docs/wiring.md](../docs/wiring.md) has the detail.

| script | what it does |
|---|---|
| `poll_test.py` | streams the idle poll and prints the height. The "is anything alive" test |
| `desk_goto.py` | closed-loop positioning: `./desk_goto.py 73cm`, `--sit`, `--stand`, `--save-sit` |
| `tmc_test.py` | one UART read to the stepper driver, decoded. `--scan` tries all four node addresses. Verified against a real TMC2209 |

`tmc_test.py` talks to the **stepper driver**, not the desk, and wants its own
wiring — 115200 8N1, and the driver has a single UART pad so it is half duplex:

```
  adapter TX --[1k]--> PDN_UART     the resistor is not optional
  adapter RX <-------- PDN_UART     the same pad, directly
  GND ---------------- GND
```

You will see your own request echoed before the reply, because RX sits on the
pad we transmit into. That echo is the point: no echo means the fault is at our
end, an echo with no reply means the driver is silent, and those need completely
different fixes.

Both take `--port`; edit `CMD_PORT` at the top of `desk_goto.py` for a default.
Protocol reference: [docs/CL103B-G_protocol.md](../docs/CL103B-G_protocol.md).

> `desk_goto.py` predates most of what is now known about this desk — in
> particular that a single edge frame is a 10 mm step, and that a recall must
> be sent as exactly ONE frame. Its `preset()` streams three, which is why it
> could save presets but never recall one. Read the protocol doc before
> trusting its timings.
