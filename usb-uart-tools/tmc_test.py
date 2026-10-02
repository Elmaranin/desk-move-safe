#!/usr/bin/env python3
"""
TMC2209 / TMC2226 UART test (NO MICROCONTROLLER).

The smallest possible "is this driver alive on its UART" check: one read
datagram, from a USB-UART adapter, decoded byte by byte. If this answers, the
driver, the wiring and the carrier's jumper are all good and the problem is in
firmware. If it does not, nothing written in firmware can help.

Wiring — the driver has ONE UART pad, so this is half duplex:

  adapter TX --[1k]--> PDN_UART     the resistor is NOT optional
  adapter RX <-------- PDN_UART     same pad, directly
  GND ---------------- GND          3.3 V logic, 115200 8N1

Before blaming anything, three things that produce total silence:

  * VM (the motor supply) must be LIVE. The chip does not answer on logic
    power alone — not slowly, not partially. This is the usual one.
  * The carrier's UART jumper must be bridged. Watterott SilentStepSticks
    ship with the UART line NOT connected to the header.
  * MS1/MS2 set the node address (MS1 = bit 0, MS2 = bit 1). --scan tries all
    four so you do not have to reason about how your board straps them.

YOU WILL SEE YOUR OWN REQUEST COME BACK FIRST. RX sits on the pad we transmit
into, so the echo always arrives before the reply. That is not a fault — it is
the most useful signal here, because it separates a broken adapter/resistor
(no echo) from a silent driver (echo, no reply).

  ./tmc_test.py --port /dev/tty.usbserial-XXXX
  ./tmc_test.py --scan
  ./tmc_test.py --reg 0x6F          # DRV_STATUS
"""
import argparse
import sys

try:
    import serial
except ImportError:
    sys.exit("needs pyserial:  pip install pyserial")

SYNC = 0x05
REPLY_ADDR = 0xFF

REGS = {
    0x00: "GCONF",
    0x01: "GSTAT",
    0x02: "IFCNT",
    0x06: "IOIN",
    0x6C: "CHOPCONF",
    0x6F: "DRV_STATUS",
}


def crc8(data):
    """The datasheet's own routine: x^8 + x^2 + x + 1, fed LSB first."""
    crc = 0
    for byte in data:
        b = byte & 0xFF
        for _ in range(8):
            if (crc >> 7) ^ (b & 1):
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
            b >>= 1
    return crc


def read_request(node, reg):
    f = [SYNC, node & 0xFF, reg & 0x7F]
    f.append(crc8(f))
    return bytes(f)


def hexs(b):
    return " ".join("%02X" % x for x in b)


def read_register(port, node, reg, quiet=False):
    """Returns the 32-bit value, or None with an explanation printed."""
    req = read_request(node, reg)
    port.reset_input_buffer()
    port.write(req)
    port.flush()

    echo = port.read(len(req))
    if not quiet:
        print("  sent  %s" % hexs(req))
        print("  echo  %s" % (hexs(echo) if echo else "(nothing)"))

    if len(echo) < len(req):
        if not quiet:
            print("\n  NO ECHO. This is OUR side of the wire — the loop closes at"
                  "\n  the adapter, through the resistor, with or without a driver"
                  "\n  attached. So: wrong serial port, TX and RX not joined, the"
                  "\n  1k missing, or the pad is held low (a PDN strapped to GND"
                  "\n  for standalone mode, or an unpowered driver clamping it"
                  "\n  through its protection diodes).")
        return None
    if echo != req:
        if not quiet:
            print("\n  ECHO ALTERED. Something else is driving the line at the"
                  "\n  same time — almost always the missing 1k, which shorts our"
                  "\n  transmitter to the driver's.")
        return None

    reply = port.read(8)
    if not quiet:
        print("  reply %s" % (hexs(reply) if reply else "(nothing)"))

    if len(reply) < 8:
        if not quiet:
            print("\n  SILENT DRIVER, but our side is proven good — the echo came"
                  "\n  back clean. In order: VM not live, then the carrier's UART"
                  "\n  jumper, then VDD/VIO, then a shared ground, then the node"
                  "\n  address (try --scan).")
        return None
    if reply[0] != SYNC or reply[1] != REPLY_ADDR or reply[2] != (reg & 0x7F):
        if not quiet:
            print("\n  MALFORMED REPLY: expected %02X %02X %02X ..."
                  % (SYNC, REPLY_ADDR, reg & 0x7F))
        return None
    if reply[7] != crc8(reply[:7]):
        if not quiet:
            print("\n  BAD CRC (%02X, expected %02X). It is answering, so this is"
                  "\n  signal integrity: the 1k, the lead length, or the baud."
                  % (reply[7], crc8(reply[:7])))
        return None

    return int.from_bytes(reply[3:7], "big")


def describe_ioin(value):
    version = (value >> 24) & 0xFF
    chip = {0x20: "TMC2208 or TMC2224", 0x21: "TMC2209"}.get(version)
    if chip is None:
        chip = ("unrecognised. 0x20 is a TMC2208/2224 and 0x21 a TMC2209; a "
                "TMC2226 shares the register map but its VERSION byte is not "
                "something this script has confirmed")
    print("\n  IOIN = 0x%08X" % value)
    print("  VERSION 0x%02X — %s" % (version, chip))
    print("  pins: ENN=%d MS1=%d MS2=%d DIAG=%d PDN_UART=%d STEP=%d DIR=%d"
          % (value & 1, (value >> 2) & 1, (value >> 3) & 1, (value >> 4) & 1,
             (value >> 6) & 1, (value >> 7) & 1, (value >> 9) & 1))
    print("  MS1/MS2 above are the node address bits: %d"
          % (((value >> 2) & 1) | (((value >> 3) & 1) << 1)))


def describe_drv_status(value):
    print("\n  DRV_STATUS = 0x%08X" % value)
    flags = [
        (1 << 0,  "OVERTEMPERATURE SHUTDOWN"),
        (1 << 1,  "over-temperature warning"),
        (1 << 2,  "SHORT TO GROUND, phase A"),
        (1 << 3,  "SHORT TO GROUND, phase B"),
        (1 << 10, "SHORT, low side A"),
        (1 << 11, "SHORT, low side B"),
        (1 << 6,  "open load A"),
        (1 << 7,  "open load B"),
        (1 << 31, "standstill"),
    ]
    named = [text for bit, text in flags if value & bit]
    print("  %s" % (" | ".join(named) if named else "no flags set"))
    print("  CS_ACTUAL %d of 31 (the current it says it is using)"
          % ((value >> 16) & 0x1F))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="e.g. /dev/tty.usbserial-A1B2")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--addr", type=int, default=0, help="node address, 0-3")
    ap.add_argument("--reg", default="0x06",
                    help="register to read (default 0x06, IOIN)")
    ap.add_argument("--scan", action="store_true",
                    help="read IOIN at all four node addresses")
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        if args.scan:
            print("scanning all four node addresses at %d baud\n" % args.baud)
            found = []
            for addr in range(4):
                value = read_register(port, addr, 0x06, quiet=True)
                print("  address %d  %s" % (
                    addr,
                    "IOIN = 0x%08X" % value if value is not None else "silent"))
                if value is not None:
                    found.append((addr, value))
            if not found:
                print("\nnothing answered. Run without --scan for the full"
                      " diagnosis of one attempt.")
                return 1
            for addr, value in found:
                print("\naddress %d:" % addr)
                describe_ioin(value)
            return 0

        reg = int(args.reg, 0)
        print("reading %s (0x%02X) from node %d at %d baud\n"
              % (REGS.get(reg, "register"), reg, args.addr, args.baud))
        value = read_register(port, args.addr, reg)
        if value is None:
            return 1
        if reg == 0x06:
            describe_ioin(value)
        elif reg == 0x6F:
            describe_drv_status(value)
        else:
            print("\n  0x%02X = 0x%08X" % (reg, value))
        return 0


if __name__ == "__main__":
    sys.exit(main())
