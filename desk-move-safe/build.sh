#!/usr/bin/env bash
# Build the desk-move-safe firmware with the verified-working toolchain/SDK
# paths. Override any of these by exporting them before running.
set -euo pipefail

export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/.pico-sdk/sdk/2.2.0}"
export PICO_TOOLCHAIN_PATH="${PICO_TOOLCHAIN_PATH:-/Applications/ArmGNUToolchain/13.2.Rel1/arm-none-eabi}"
export FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH:-$HOME/work/diy/rp2350/FreeRTOS-Kernel}"

# Prefer the cmake/ninja the Pico VS Code extension installed, and fall back to
# whatever is on PATH.
CMAKE="${CMAKE:-$HOME/.pico-sdk/cmake/v3.31.5/bin/cmake}"
NINJA="${NINJA:-$HOME/.pico-sdk/ninja/v1.12.1/ninja}"
[ -x "$CMAKE" ] || CMAKE=cmake
[ -x "$NINJA" ] || NINJA=ninja
# CMake resolves the generator's build tool through PATH, so ninja has to be on
# it too, not just invoked by absolute path below.
case "$NINJA" in /*) PATH="$(dirname "$NINJA"):$PATH" ;; esac
export PATH="$PICO_TOOLCHAIN_PATH/bin:$PATH"

cd "$(dirname "$0")"

# The console's help() and docs/commands.md must list the same commands.
tools/check_commands.sh

mkdir -p build
cd build
"$CMAKE" -G Ninja .. "$@"
"$NINJA"

echo
echo "Built: $(pwd)/desk-move-safe.uf2"
echo "Flash: ./flash.sh   (no BOOTSEL button needed after the first time)"
