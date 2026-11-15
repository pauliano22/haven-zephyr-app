#!/usr/bin/env bash
# One-command regression on a plugged-in nRF5340 DK (WSL2 + usbipd + Windows Bluetooth).
#   1. (optional) build  2. flash  3. zero RAM and boot  4. run the Windows Bluetooth test
#   5. read the board's RTT log from RAM and check it saw what the test did  6. reset the board
# Usage: tools/dk_regression.sh [--build]
# Needs: DK attached to WSL (usbipd.exe attach --wsl --busid <id>), JLinkExe, ~/ncs with its venv,
#        a Windows venv with `bleak` (see ble_dk_windows_test.py). Override with HAVEN_WIN_PY.
set -u
APP="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${HAVEN_BUILD_DIR:-$HOME/.cache/haven_builds/dk_master}"
WIN_PY="${HAVEN_WIN_PY:-C:\\Users\\pmi\\claude-bridge\\files\\blevenv\\Scripts\\python.exe}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dk_regression.XXXXXX")"
FAIL=0
step() { echo; echo "== $*"; }

if [ "${1:-}" = "--build" ]; then
  step "build"
  ( source ~/ncs/.venv/bin/activate && cd ~/ncs && west build -b nrf5340dk/nrf5340/cpuapp --sysbuild \
      -d "$BUILD" --pristine always "$APP" -- -DBOARD_ROOT="$APP" ) > "$WORK/build.log" 2>&1 \
    || { echo "build failed, see $WORK/build.log"; exit 1; }
  echo "built"
fi

step "flash"
( source ~/ncs/.venv/bin/activate && cd ~/ncs && west flash -d "$BUILD" --runner jlink ) > "$WORK/flash.log" 2>&1 \
  || { echo "flash failed (is the DK attached to WSL? usbipd.exe list), see $WORK/flash.log"; exit 1; }
echo "flashed"

cat > "$WORK/boot.jlink" <<J
device NRF5340_XXAA_APP
if SWD
speed 4000
connect
h
loadbin $WORK/zero.bin 0x20000000
r
g
exit
J
head -c 65536 /dev/zero > "$WORK/zero.bin"
cat > "$WORK/dump.jlink" <<J
device NRF5340_XXAA_APP
if SWD
speed 4000
connect
h
savebin $WORK/ram.bin 0x20000000 0x10000
g
exit
J
cat > "$WORK/reset.jlink" <<J
device NRF5340_XXAA_APP
if SWD
speed 4000
connect
r
g
exit
J

step "boot with cleared RAM, then Windows Bluetooth test"
timeout 90 JLinkExe -NoGui 1 -CommandFile "$WORK/boot.jlink" >/dev/null 2>&1
sleep 3
TEST_WIN="$(wslpath -w "$APP/tools/ble_dk_windows_test.py")"
( cd /mnt/c && powershell.exe -NoProfile -Command "& '$WIN_PY' '$TEST_WIN' 2>&1" ) | tee "$WORK/ble.log"
grep -q "SUMMARY .* 0 failed" "$WORK/ble.log" || { echo "BLUETOOTH TEST FAILED"; FAIL=1; }

step "what the board's own log shows"
timeout 60 JLinkExe -NoGui 1 -CommandFile "$WORK/dump.jlink" >/dev/null 2>&1
strings -n 6 "$WORK/ram.bin" | sed 's/\x1b\[[0-9;]*m//g' > "$WORK/rtt.txt"
check() { # description, pattern, minimum count
  n=$(grep -cE "$2" "$WORK/rtt.txt"); if [ "$n" -ge "$3" ]; then echo "   ok    $1 ($n)"; else echo "   MISSING $1 (found $n, wanted $3)"; FAIL=1; fi; }
check "boots and advertises"                       'Advertising as "Haven"' 1
check "restores saved settings from flash"          'Restored volume from flash' 1
check "sees a phone connect"                        'Phone connected' 1
check "applies the volume write"                    'Volume set: 30%' 1
check "applies the band write"                      'FreqRange set: \[500, 2000\]' 1
check "rejects bad volume"                          'Volume write rejected' 1
check "rejects bad bands (2 kinds)"                 'FreqRange write rejected' 2
check "rejects bad length"                          'bad length' 1
check "handles the NUS command"                     'Bypass: disabled' 1
if grep -q "<err>" "$WORK/rtt.txt"; then echo "   errors in log (the ADAU1860 one is expected on a bare DK):"; grep "<err>" "$WORK/rtt.txt" | sed 's/^/     /'; fi

step "reset the board so it advertises again"
timeout 60 JLinkExe -NoGui 1 -CommandFile "$WORK/reset.jlink" >/dev/null 2>&1
echo "(a Windows central can leave the board 'connected'; the reset clears it)"

echo; [ $FAIL = 0 ] && echo "RESULT: pass" || echo "RESULT: FAIL"
echo "logs: $WORK"
exit $FAIL
