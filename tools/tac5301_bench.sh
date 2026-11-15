#!/usr/bin/env bash
# One command for the TAC5301 breadboard test on a plugged-in nRF5340 DK.
#   1. (with --build) build the bench firmware   2. flash it   3. boot   4. read the board's own log
#   5. print, in plain words, whether the chip answered and whether microphone-to-speaker is running
# Usage: tools/tac5301_bench.sh [--build] [--check-only]
#   --check-only: do not flash, just reboot the board and read the log again (after moving a wire).
# Needs: DK attached to WSL (from Windows: usbipd.exe attach --wsl --busid <id>), JLinkExe, ~/ncs with its venv.
# Guide: hardware_kicad/TAC5301_BENCH_GUIDE.md
set -u
APP="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${HAVEN_BUILD_DIR:-$HOME/.cache/haven_builds/dk_tac5301}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tac5301_bench.XXXXXX")"
MODE="${1:-}"

if [ "$MODE" = "--build" ] || [ ! -f "$BUILD/haven_zephyr_app/zephyr/zephyr.hex" ]; then
  echo "== build (a few minutes)"
  ( source ~/ncs/.venv/bin/activate && cd ~/ncs && west build -b nrf5340dk/nrf5340/cpuapp --sysbuild -d "$BUILD" "$APP" -- \
      -DBOARD_ROOT="$APP" -DEXTRA_DTC_OVERLAY_FILE=boards/nrf5340dk_nrf5340_cpuapp_tac5301.overlay \
      -DCONFIG_HAVEN_CODEC_TAC5301=y ) > "$WORK/build.log" 2>&1 || { echo "build failed, see $WORK/build.log"; exit 1; }
  echo "built"
fi
if [ "$MODE" != "--check-only" ]; then
  echo "== flash"
  ( source ~/ncs/.venv/bin/activate && cd ~/ncs && west flash -d "$BUILD" --runner jlink ) > "$WORK/flash.log" 2>&1 \
    || { echo "flash failed. Is the DK plugged in, switched on, and attached to WSL (usbipd.exe list)? Log: $WORK/flash.log"; exit 1; }
  echo "flashed"
fi

head -c 65536 /dev/zero > "$WORK/zero.bin"
printf 'device NRF5340_XXAA_APP\nif SWD\nspeed 4000\nconnect\nh\nloadbin %s 0x20000000\nr\ng\nexit\n' "$WORK/zero.bin" > "$WORK/boot.jlink"
printf 'device NRF5340_XXAA_APP\nif SWD\nspeed 4000\nconnect\nh\nsavebin %s 0x20000000 0x10000\ng\nexit\n' "$WORK/ram.bin" > "$WORK/dump.jlink"
echo "== boot and read the board's log"
timeout 90 JLinkExe -NoGui 1 -CommandFile "$WORK/boot.jlink" >/dev/null 2>&1 || { echo "could not reach the DK over its debug link"; exit 1; }
sleep 5
timeout 60 JLinkExe -NoGui 1 -CommandFile "$WORK/dump.jlink" >/dev/null 2>&1
strings -n 6 "$WORK/ram.bin" | sed 's/\x1b\[[0-9;]*m//g' > "$WORK/rtt.txt"

echo
grep -E "TAC5301|BENCH CHECK|Tone generator" "$WORK/rtt.txt" | sed 's/^/   /'
echo
if grep -q "BENCH CHECK: PASS" "$WORK/rtt.txt"; then
  echo "RESULT: RUNNING. Tap the microphone: you should hear it from the speaker."
  exit 0
elif grep -q "did not answer" "$WORK/rtt.txt"; then
  echo "RESULT: THE CHIP DID NOT ANSWER. Power, ground (the centre pad!), SDA / SCL or the two pull-up resistors."
elif grep -q "BENCH CHECK" "$WORK/rtt.txt"; then
  echo "RESULT: THE CHIP ANSWERS BUT IS NOT RUNNING. The BENCH CHECK line above says which part is missing."
else
  echo "RESULT: no bench lines in the log. Was the bench firmware flashed (run with --build)? Log: $WORK/rtt.txt"
fi
exit 1
