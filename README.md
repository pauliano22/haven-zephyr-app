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
```

nRF5340 is dual-core: this application runs on the **app core** (cpuapp).
The BLE **Controller** runs on the **network core** (cpunet) as the
standard NCS `hci_rpmsg` image, built automatically by sysbuild — see
`sysbuild.conf`. You don't write anything for the net core yourself; just
build with `--sysbuild` (below).

### Wire protocol (shared with the app)

```json
{"type":"MULTI_FILTER","bands":[{"f0":4500,"Q":10.0}]}\n
{"type":"BYPASS","enabled":true}\n
```

Up to **5 bands**. Bands may optionally include `"atten_db"` (positive dB of
reduction; omitted = full notch). All parameters are clamped on-device
(`src/protocol.h`).

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

- [x] NUS peripheral, advertising, auto re-advertise on disconnect
- [x] BLE connect/disconnect lifecycle wired to `adau1860_control_on_ble_*`
      callback stubs
- [x] Newline framing + JSON parsing with parameter clamping
- [x] RBJ notch / peaking-cut coefficient math (ported from Teensy prototype)
- [x] nRF5340 DT overlay (I2C1 control port, I2S0 audio) + sysbuild net-core
      image config
- [x] CI: automated build on push/PR
- [ ] Real I2C register access + device ID check (`adau1860_control_init`
      confirms the bus is ready; no register transactions yet)
- [ ] SigmaStudio+ program export → parameter RAM address map
- [ ] Safeload coefficient writes (`adau1860_control_apply_filters`)
- [ ] I2S audio path driver init (format depends on ADAU1860 datasheet specifics)
- [ ] Device → app acks over NUS TX (`ble_transport_send` is ready)
- [ ] Confirm I2C1 is in fact the intended control-port bus once real PCB
      routing is known (see overlay `NOTE`)
