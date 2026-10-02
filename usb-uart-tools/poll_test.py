#!/usr/bin/env python3
"""
Poll-and-listen test (NO PANEL).

Streams the idle poll frame to the board continuously and prints whatever the
board sends back. Answers: does the board stream height when WE poll it?

Wiring (single adapter, panel unplugged):
  adapter TX --[1k]--> board RX   (we send idle polls here)
  adapter RX <--------- board TX   (we listen for 55 AA height frames)
  GND ---------------- GND
  3.3V logic, 9600 8N1

Ctrl-C to stop.
"""
import serial, time, threading, sys

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/tty.usbserial-0001'
BAUD = 9600
TICK = 0.02          # poll cadence; match the panel's period if known

IDLE = bytes.fromhex('aa5508010000010 8'.replace(' ', ''))

ser = serial.Serial(PORT, BAUD, timeout=0)
lock = threading.Lock()
run = True

def poller():
    """Continuously send idle so the board keeps talking."""
    while run:
        with lock:
            ser.write(IDLE)
        time.sleep(TICK)

def ok(f):
    s = sum(f[0:6])
    return (s >> 8) == f[6] and (s & 0xFF) == f[7]

threading.Thread(target=poller, daemon=True).start()
print(f"polling {PORT} with idle frames, listening for board replies...")
print("(Ctrl-C to stop)\n")

buf = bytearray()
raw_seen = False
last_print = 0.0
try:
    while True:
        with lock:
            data = ser.read(64)
        if data:
            if not raw_seen:
                print("first raw bytes:", data.hex())
                raw_seen = True
            buf.extend(data)
            while len(buf) >= 8:
                # accept either direction's sync so we see everything
                if buf[0] in (0x55, 0xAA) and buf[1] in (0x55, 0xAA) and buf[0] != buf[1]:
                    f = buf[0:8]
                    if ok(f):
                        if f[0] == 0x55 and f[3] == 0x02:
                            mm = (f[4] << 8) | f[5]
                            now = time.time()
                            if now - last_print > 0.1:
                                print(f"height: {mm/10:.1f} cm   ({f.hex()})")
                                last_print = now
                        del buf[0:8]
                    else:
                        buf.pop(0)
                else:
                    buf.pop(0)
        else:
            time.sleep(0.002)
except KeyboardInterrupt:
    run = False
    time.sleep(0.1)
    print("\nstopped.")
