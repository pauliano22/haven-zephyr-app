# nrf52_acousticshield_fw

Production firmware for **Project AcousticShield**: an nRF52 acts as the BLE
peripheral for the companion app and as I2C/SPI control-port master to an
**ADAU1860** audio DSP. Successor to the validated `teensy_hearing_shield`
prototype.

## Architecture

```
Mobile app (react-native-ble-plx)
    │  Nordic UART Service, newline-terminated JSON, MTU 247
    ▼
ble_transport.c   — NUS peripheral "AcousticShield", line reassembly
    ▼
protocol.c        — fixed-schema JSON parser (MULTI_FILTER / BYPASS), clamps
    ▼
adau1860.c        — biquad coefficient math + [PLACEHOLDER] I2C/SPI writes
    ▼
ADAU1860          — cascade of ≤5 notch / peaking-cut biquads
```

### Wire protocol (shared with the app)

```json
{"type":"MULTI_FILTER","bands":[{"f0":4500,"Q":10.0}]}\n
{"type":"BYPASS","enabled":true}\n
```

Up to **5 bands**. Bands may optionally include `"atten_db"` (positive dB of
reduction; omitted = full notch). All parameters are clamped on-device
(`src/protocol.h`).

## Building (nRF Connect SDK / Zephyr)

Inside an NCS workspace (tested layout: NCS v2.x):

```sh
west build -b nrf52840dk/nrf52840 .
west flash
```

`boards/nrf52840dk_nrf52840.overlay` holds devkit pin assignments for the
ADAU1860 buses — replace with the production PCB netlist when it lands.

## Status

- [x] NUS peripheral, advertising, auto re-advertise on disconnect
- [x] Newline framing + JSON parsing with parameter clamping
- [x] RBJ notch / peaking-cut coefficient math (ported from Teensy prototype)
- [ ] Real I2C register access + device ID check (`adau1860_init`)
- [ ] SigmaStudio+ program export → parameter RAM address map
- [ ] Safeload coefficient writes (`adau1860_apply_filters`)
- [ ] SPI bulk program download
- [ ] Device → app acks over NUS TX (`ble_transport_send` is ready)
