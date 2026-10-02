#!/usr/bin/env bash
# Build (unless -n) and flash over USB *without* pressing the BOOTSEL button.
#
# How: picotool's -f/--force resets a running board into BOOTSEL over USB, loads,
# then -x runs it. This works because the firmware has USB stdio enabled
# (pico_enable_stdio_usb), which exposes the reset interface picotool uses.
#
# Caveat: the board must *currently* be running firmware that has that interface
# (i.e. one of ours). The very first flash onto a board running unrelated code
# still needs one BOOTSEL press; every flash after that is button-free.
#
# Usage:
#   ./flash.sh          # build, then flash
#   ./flash.sh -n       # flash whatever is already built
set -euo pipefail

PICOTOOL="${PICOTOOL:-$HOME/.pico-sdk/picotool/2.2.0/picotool/picotool}"
cd "$(dirname "$0")"

[ "${1:-}" = "-n" ] || ./build.sh

uf2="build/desk-move-safe.uf2"
[ -x "$PICOTOOL" ] || { echo "picotool not found at: $PICOTOOL (set \$PICOTOOL)"; exit 1; }
[ -f "$uf2" ]      || { echo "Not built: $uf2 — run ./build.sh first"; exit 1; }

echo "Flashing $uf2 over USB (no BOOTSEL button)..."
exec "$PICOTOOL" load -f -x -v "$uf2"
