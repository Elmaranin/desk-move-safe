#!/usr/bin/env python3
"""
CL103B-G desk control — drive-to-target, panel-free.

Replaces the panel: a background poller streams the idle frame continuously so
the board keeps reporting height; a background reader parses that height.
goto()/preset() temporarily override the streamed frame to drive the desk.

Wiring (single adapter, panel unplugged):
  adapter TX --[1k]--> board RX   (poller sends here)
  adapter RX <--------- board TX   (reader listens here)
  GND ---------------- GND
  3.3V logic, 9600 8N1

Two-adapter passive tap (panel stays connected): set TWO_PORT=True and point
HEIGHT_PORT at the listener.

Protocol summary (all frames: 8 bytes, 16-bit BE sum checksum over bytes 0..5):
  board->panel  55 AA 08 02  HH HL  CK CK   height = HHHL mm
  panel->board  AA 55 08 01 00 KK  CK CK    key code KK:
     00 idle | 01 recall stand | 02 recall sit
     06 down(hold) | 08 up(hold) | 03 SAVE stand | 04 SAVE sit
"""

import serial, time, threading, sys, argparse

# ---------------- config ----------------
CMD_PORT    = '/dev/tty.usbserial-0001'
TWO_PORT    = False
HEIGHT_PORT = '/dev/tty.usbserial-0002'

BAUD        = 9600
TICK        = 0.1       # frame cadence (s). Measured panel poll period ~104ms.
WINDOW_MM   = 4           # stop this far short; let momentum coast in
COAST_UP_MM   = 15        # desk drifts up ~this far after last UP cmd; aim short
COAST_DOWN_MM = 15        # desk drifts down ~this far after last DOWN cmd; aim short
TIMEOUT_S   = 40          # give up if target not reached in time
STALL_S     = 1.5         # give up if height stops changing while driving
MIN_MM      = 600         # refuse targets outside this range (tune to your desk)
MAX_MM      = 1300

# ---------------- frames ----------------
IDLE  = bytes.fromhex('aa5508010000010 8'.replace(' ', ''))
UP    = bytes.fromhex('aa550801000801 10'.replace(' ', ''))
DOWN  = bytes.fromhex('aa5508010006010e')
SIT   = bytes.fromhex('aa5508010002010a')   # recall sit
STAND = bytes.fromhex('aa550801000101 09'.replace(' ', ''))  # recall stand
# SAVE frames: one-shot, emitted by a 5s long-press on the panel. MANUAL ONLY —
# never place these in an automated path or you overwrite the user's presets.
SAVE_STAND = bytes.fromhex('aa5508010003010b')                 # 0x03 -> stand
SAVE_SIT   = bytes.fromhex('aa550801000401 0c'.replace(' ', ''))  # 0x04 -> sit


def _cksum_ok(f):
    s = sum(f[0:6])
    return (s >> 8) == f[6] and (s & 0xFF) == f[7]


class Desk:
    """Owns the serial port(s), a persistent poller, and a height reader."""

    def __init__(self, cmd_port=CMD_PORT, two_port=TWO_PORT,
                 height_port=HEIGHT_PORT, tick=TICK):
        self.tick = tick
        self.cmd = serial.Serial(cmd_port, BAUD, timeout=0)
        self.lock = threading.Lock()

        if two_port:
            self.hser = serial.Serial(height_port, BAUD, timeout=0)
            self.hlock = threading.Lock()
        else:
            self.hser = self.cmd
            self.hlock = self.lock

        self.mm = None
        self.ts = 0.0
        self._default = IDLE
        self._buf = bytearray()
        self._run = True

        threading.Thread(target=self._read_loop, daemon=True).start()
        threading.Thread(target=self._poll_loop, daemon=True).start()

    def _poll_loop(self):
        while self._run:
            with self.lock:
                self.cmd.write(self._default)
            time.sleep(self.tick)

    def _read_loop(self):
        while self._run:
            with self.hlock:
                data = self.hser.read(64)
            if not data:
                time.sleep(0.002); continue
            self._buf.extend(data)
            while len(self._buf) >= 8:
                if self._buf[0] != 0x55 or self._buf[1] != 0xAA:
                    self._buf.pop(0); continue
                f = self._buf[0:8]
                if _cksum_ok(f):
                    if f[3] == 0x02:
                        self.mm = (f[4] << 8) | f[5]
                        self.ts = time.time()
                    del self._buf[0:8]
                else:
                    self._buf.pop(0)

    def wait_height(self, timeout=3.0):
        t0 = time.time()
        while self.mm is None:
            if time.time() - t0 > timeout:
                raise RuntimeError("no height frames — check board TX wiring / polling")
            time.sleep(0.02)
        return self.mm

    def stop(self):
        self._default = IDLE

    def close(self):
        self._run = False
        time.sleep(0.1)

    # ---- closed-loop move ----
    def goto(self, target_mm, verbose=True):
        if not (MIN_MM <= target_mm <= MAX_MM):
            raise ValueError(f"target {target_mm} outside [{MIN_MM},{MAX_MM}]")
        self.wait_height()

        start = time.time()

        # ---- COARSE PHASE ----
        # Only run it if the move is big enough that coasting helps. For small
        # moves (<= max coast) skip straight to creep, or we'd overshoot / no-op.
        start_mm = self.mm
        going_up = start_mm < target_mm
        gap = abs(target_mm - start_mm)
        coast = COAST_UP_MM if going_up else COAST_DOWN_MM

        if gap > coast + WINDOW_MM:
            stop_at = target_mm - coast if going_up else target_mm + coast
            last_mm = self.mm
            last_change = time.time()
            try:
                while True:
                    cur = self.mm
                    now = time.time()
                    if (going_up and cur >= stop_at) or (not going_up and cur <= stop_at):
                        break
                    self._default = UP if going_up else DOWN
                    if now - start > TIMEOUT_S:
                        self._default = IDLE
                        if verbose: print(f"\n[goto] TIMEOUT at {cur/10:.1f}cm")
                        return False
                    if cur != last_mm:
                        last_mm = cur; last_change = now
                    elif now - last_change > STALL_S:
                        self._default = IDLE
                        if verbose: print(f"\n[goto] STALL at {cur/10:.1f}cm (limit/obstruction?)")
                        return abs(self._settle() - target_mm) <= WINDOW_MM
                    if verbose:
                        print(f"\r[goto] cur={cur/10:.1f}cm -> {target_mm/10:.1f}cm  "
                              f"{'UP  ' if going_up else 'DOWN'}   ", end='')
                    time.sleep(self.tick)
            finally:
                self._default = IDLE
            self._settle()   # let the coast finish before creeping

        # ---- CREEP PHASE ----
        # Short pulses with a re-read between each; approaches at ~zero speed so
        # there's no coast. Handles small moves and the final few mm exactly.
        self._creep(target_mm, verbose=verbose, deadline=start + TIMEOUT_S)

        time.sleep(0.5)                 # quiesce
        confirmed = self.mm
        err = confirmed - target_mm
        if verbose:
            print(f"\n[goto] settled at {confirmed/10:.1f}cm  (target {target_mm/10:.1f}, "
                  f"off by {err/10:+.1f})")
            print(f"current height: {confirmed/10:.1f}cm")
        return True

    def _creep(self, target_mm, verbose=False, deadline=None, pulse=0.22):
        """Nudge toward target with short pulses until within WINDOW_MM.
        pulse should be >= ~2 poll periods (2*0.104) so each nudge reliably
        delivers command frames. Re-reads between pulses; ~zero coast."""
        stale = 0
        while True:
            cur = self.mm
            err = target_mm - cur
            if abs(err) <= WINDOW_MM:
                break
            if deadline and time.time() > deadline:
                if verbose: print(f"\n[goto] creep TIMEOUT at {cur/10:.1f}cm")
                break
            frame = UP if err > 0 else DOWN
            self._default = frame
            time.sleep(pulse)            # a brief nudge
            self._default = IDLE
            time.sleep(0.25)             # let it stop and report before re-reading
            if self.mm == cur:           # no change -> maybe at a limit
                stale += 1
                if stale >= 4:
                    if verbose: print(f"\n[goto] creep stuck at {cur/10:.1f}cm")
                    break
            else:
                stale = 0
            if verbose:
                print(f"\r[goto] creep {cur/10:.1f}cm -> {target_mm/10:.1f}cm  "
                      f"{'UP  ' if frame is UP else 'DOWN'}   ", end='')
        self._default = IDLE

    def _settle(self, quiet_s=1.2, timeout=6.0):
        """Poll until height is continuously stable for quiet_s; return resting mm.
        Re-arms whenever height moves, so it captures the full post-command coast."""
        # give the coast a moment to even start before we begin timing stability
        time.sleep(0.3)
        last = self.mm
        stable_since = time.time()
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(0.08)
            cur = self.mm
            if cur != last:
                last = cur
                stable_since = time.time()   # moved -> restart the quiet timer
            elif time.time() - stable_since >= quiet_s:
                break
        return self.mm

    # ---- one-shot recall (board self-drives to stored height) ----
    def preset(self, which, verbose=True):
        start_mm = self.mm
        f = SIT if which == 'sit' else STAND
        # send the one-shot recall a few times to be sure it's seen, then idle
        self._default = f
        time.sleep(self.tick * 3)
        self._default = IDLE

        # The board self-drives. Trace height live so we can see the real motion.
        t0 = time.time()
        moving = False
        last = self.mm
        last_change = time.time()
        peak = last
        while time.time() - t0 < 35.0:
            time.sleep(0.1)
            cur = self.mm
            if verbose:
                print(f"\r[{which}] tracking {cur/10:.1f}cm  "
                      f"(start {start_mm/10:.1f})   ", end='')
            if cur != last:
                moving = True
                last = cur
                last_change = time.time()
            else:
                quiet = time.time() - last_change
                if moving and quiet >= 2.0:
                    break
                if not moving and quiet >= 2.5 and start_mm is not None \
                        and abs((cur or 0) - start_mm) <= 3:
                    break
        time.sleep(0.5)
        confirmed = self.mm
        if verbose and confirmed is not None:
            print(f"\n[{which}] settled at {confirmed/10:.1f}cm  "
                  f"(moved: {'yes' if moving else 'NO'})")
            print(f"current height: {confirmed/10:.1f}cm")
        return confirmed

    # ---- save CURRENT height into a slot. OVERWRITES. confirm=True required ----
    def save_preset(self, which, confirm=False):
        """Store the desk's current height as sit/stand (emits 0x04/0x03).
        Guarded so automated code can't fire it accidentally."""
        if not confirm:
            raise PermissionError(
                "save_preset overwrites a stored position; call with confirm=True")
        f = SAVE_SIT if which == 'sit' else SAVE_STAND
        self._default = f
        time.sleep(self.tick * 3)
        self._default = IDLE


# ---------------- CLI ----------------
def parse_target(s):
    """Accept '73', '73cm', '730mm', '73.0' -> millimetres (int)."""
    s = s.strip().lower().replace(' ', '')
    if s.endswith('mm'): return int(round(float(s[:-2])))
    if s.endswith('cm'): return int(round(float(s[:-2]) * 10))
    v = float(s)
    return int(round(v * 10)) if v < 300 else int(round(v))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description="Drive CL103B-G desk to a height (panel-free).")
    p.add_argument('target', nargs='?',
                   help="height, e.g. 73, 73cm, 730mm. Omit to stream live height.")
    p.add_argument('--sit',   action='store_true', help="recall the sit preset")
    p.add_argument('--stand', action='store_true', help="recall the stand preset")
    p.add_argument('--save-sit',   action='store_true',
                   help="OVERWRITE the sit preset with the current height")
    p.add_argument('--save-stand', action='store_true',
                   help="OVERWRITE the stand preset with the current height")
    p.add_argument('--port',  default=CMD_PORT, help="command serial port")
    p.add_argument('--quiet', action='store_true')
    args = p.parse_args()

    desk = Desk(cmd_port=args.port)
    time.sleep(0.5)
    cur = "unknown" if desk.mm is None else f"{desk.mm/10:.1f}cm"
    print(f"current height: {cur}")

    try:
        if args.save_sit or args.save_stand:
            which = 'sit' if args.save_sit else 'stand'
            desk.wait_height()
            here = f"{desk.mm/10:.1f}cm"
            desk.save_preset(which, confirm=True)
            print(f"saved current height ({here}) as {which} preset")
        elif args.sit:
            desk.preset('sit')
        elif args.stand:
            desk.preset('stand')
        elif args.target is not None:
            try:
                mm = parse_target(args.target)
            except ValueError:
                print(f"bad target: {args.target!r}"); sys.exit(2)
            ok = desk.goto(mm, verbose=not args.quiet)
            sys.exit(0 if ok else 1)
        else:
            print("(streaming height; Ctrl-C to stop)")
            while True:
                if desk.mm is not None:
                    print(f"\rheight: {desk.mm/10:.1f} cm   ", end='')
                time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        desk.stop()
        desk.close()