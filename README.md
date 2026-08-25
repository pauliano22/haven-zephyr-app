# haven_zephyr_app

Production firmware for **Project Haven**: an nRF5340 acts as the BLE
peripheral for the companion app and as I2C control-port master + I2S audio
source/sink to an **ADAU1860** audio DSP. Successor to the validated
`teensy_hearing_shield` prototype.

**Target hardware not final** — the production PCB and CAD haven't landed
yet. Everything here targets the nRF5340 DK with standard dev-kit pin
defaults so the repo is ready to build and flash the moment real hardware
arrives; bus assignments and pin mappings will need a pass once the Haven
dev board schematic exists (see the `NOTE`s in
`boards/nrf5340dk_nrf5340_cpuapp.overlay`).

## Architecture

```
Mobile app (react-native-ble-plx)
    │  Nordic UART Service, newline-terminated JSON, MTU 247
    ▼
ble_transport.c      — NUS peripheral "Haven", line reassembly,
    │                   BLE connect/disconnect callbacks
    ▼
protocol.c            — fixed-schema JSON parser (MULTI_FILTER / BYPASS), clamps
    ▼
adau1860_control.c    — biquad coefficient math + I2C1 control-port writes
    ▼
ADAU1860               — cascade of ≤5 notch / peaking-cut biquads, I2S0 audio

gatt_audio_service.c  — Haven Audio Control Service (custom GATT, separate
    │                    from NUS above): Volume + FreqRange characteristics
    ▼
mock_audio_pipeline.c — bench-only: simulates a filtered signal so parameter
    │                    changes are observable without real DSP hardware
    ▼
settings_store.c      — NVS: Volume/FreqRange persist across reboots
```

nRF5340 is dual-core: this application runs on the **app core** (cpuapp).
The BLE **Controller** runs on the **network core** (cpunet) as NCS's
unified `ipc_radio` image (configured for HCI serialization —
`SB_CONFIG_NETCORE_IPC_RADIO_BT_HCI_IPC`), built automatically by
sysbuild — see `sysbuild.conf`. You don't write anything for the net core
yourself; just build with `--sysbuild` (below).

### Wire protocol (shared with the app)

```json
{"type":"MULTI_FILTER","bands":[{"f0":4500,"Q":10.0}]}\n
{"type":"BYPASS","enabled":true}\n
```

Up to **5 bands**. Bands may optionally include `"atten_db"` (positive dB of
reduction; omitted = full notch). All parameters are clamped on-device
(`src/protocol.h`).

### Haven Audio Control Service (bench/prototyping — not the production path)

A second, independent control surface added while the real DSP board is
being fabricated, so the nRF5340 DK can serve as a full prototyping bench
in the meantime. Unlike the JSON path above, out-of-range writes are
**rejected** (ATT error), not clamped:

| | UUID | Format |
|---|---|---|
| Service | `7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10` | — |
| Volume | `7a1e0002-4b5c-4e8a-9c1a-2f6b8d3c9a10` | 1 byte, uint8 0–100 (%) |
| FreqRange | `7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10` | 4 bytes, little-endian uint16 pair (lower_hz, upper_hz), bounded [200, 8000] Hz |

Both characteristics are read/write/notify. Not advertised in the
scan-response payload (no room left alongside NUS's own 128-bit UUID) —
discover by connecting and doing GATT service discovery, not by
service-UUID scan filter. `tools/ble_bench_test.html` is a standalone Web
Bluetooth test page for poking at this directly from a desktop browser,
no app or phone needed.

## Building locally (nRF Connect SDK / Zephyr)

1. Install the [nRF Connect SDK toolchain](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/installation.html)
   (nRF Connect for VS Code's toolchain manager is the easiest path — installs
   `west`, the Zephyr SDK, and `nrfutil` together).
2. From a directory that will become your west workspace root (a level
   *above* this repo — `west init -l` uses this repo as the manifest):

   ```sh
   west init -l haven_zephyr_app
   cd haven_zephyr_app/..
   west update
   west zephyr-export
   ```

3. Build — **`--sysbuild` is required**, it's what builds the network-core
   BLE controller image alongside this app:

   ```sh
   west build --board nrf5340dk/nrf5340/cpuapp --sysbuild haven_zephyr_app
   ```

4. Flash (with the DK connected over USB):

   ```sh
   west flash
   ```

   If `west flash` fails with `nrfutil not found`, it isn't installed —
   pass `--runner jlink` instead (works fine, just a different flash tool):

   ```sh
   west flash --runner jlink
   ```

5. Watch logs over the DK's on-board J-Link RTT/USB-CDC serial (115200 8N1,
   or `west build -t rtt_console` / nRF Connect for VS Code's RTT viewer).

`boards/nrf5340dk_nrf5340_cpuapp.overlay` holds the dev-kit pin assignments
for I2C1 (ADAU1860 control) and I2S0 (audio data) — replace with the
production PCB netlist once it lands, and double-check the bus numbering
against your exact DK revision first (see the overlay's own comments).

## CI

`.github/workflows/build.yml` runs this same build (nRF5340 DK, app core,
with sysbuild) on every push to `master` (this repo's default branch) and
on pull requests targeting it.

## Status

**Verified on real nRF5340 DK hardware** (not just build-tested) as of
2026-08-25:

- [x] NUS peripheral, advertising, auto re-advertise on disconnect
- [x] BLE connect/disconnect lifecycle wired to `adau1860_control_on_ble_*`
      callback stubs
- [x] Newline framing + JSON parsing with parameter clamping
- [x] RBJ notch / peaking-cut coefficient math (ported from Teensy prototype)
- [x] nRF5340 DT overlay (I2C1 control port, I2S0 audio) + sysbuild net-core
      (`ipc_radio`) image config
- [x] CI: automated build on push/PR
- [x] Haven Audio Control Service — custom GATT characteristics for
      Volume/FreqRange, validated writes (rejected, not clamped, unlike
      the JSON path)
- [x] Mock audio pipeline — software-in-the-loop biquad filter reacting to
      live BLE parameter changes, standing in for real DSP output
- [x] NVS settings persistence — Volume/FreqRange survive a reboot

### Not done — and "get the DSP board" alone won't finish these

The item below is the actual core function of a hearing-protection
device; everything above it is control-plane / bench tooling around a
DSP path that doesn't process real audio yet:

- [ ] **Firmware-side independent safety ceiling.** Only the app has a
      soft 85 dB cap (`haven-app`'s `src/constants/safety.ts`) — the
      device itself has no hard limit of its own. Tracked as a blocker in
      `haven-app`'s `docs/safety.md`/`docs/roadmap.md`; do this before any
      real-ear testing, not after.
- [ ] Real I2C register access + device ID check (`adau1860_control_init`
      confirms the bus is ready; no register transactions yet)
- [ ] SigmaStudio+ program export → parameter RAM address map
- [ ] Safeload coefficient writes (`adau1860_control_apply_filters`)
- [ ] I2S audio path driver init (format depends on ADAU1860 datasheet specifics)
- [ ] Device → app acks over NUS TX (`ble_transport_send` is ready)
- [ ] Confirm I2C1 is in fact the intended control-port bus once real PCB
      routing is known (see overlay `NOTE`) — DK pin assignments are
      placeholders, not guaranteed to carry over
- [ ] Wire the production `haven-app` to the Haven Audio Control Service
      above (currently only exercised by `tools/ble_bench_test.html`)
