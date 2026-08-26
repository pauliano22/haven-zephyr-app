# Haven — feature proposals

A handful of concrete ideas for where the project could go next, grounded
in what actually exists today: nRF5340 + ADAU1860 DSP hardware in fab,
Zephyr firmware with BLE GATT bench controls, an independent tone-level
safety ceiling, NVS settings persistence, and adaptive-advertising power
management; a React Native app with an LDL guided hearing test; and a new
offline ML tool (`ml/`) that finds a troublesome frequency band from a
`.wav` recording.

This is a one-time, bounded list meant to be skimmed periodically, not a
tracked backlog — nothing here is scheduled or committed to.

## Firmware

- **Auto-apply the ML tool's output over BLE.** `ml/` already produces the
  exact bytes the FreqRange characteristic expects, but nothing writes them
  yet. A small bench script that runs `analyze.py` on a recording and
  pushes the result straight to a connected board (over the same Web
  Bluetooth path `tools/ble_bench_test.html` already uses) would close the
  loop from "recorded a problem sound" to "board is filtering for it" in
  one step.
- **CONFIG_PM_DEVICE for the I2C/I2S buses.** Once the real ADAU1860 driver
  work starts, suspending those peripherals between transactions (rather
  than just the CPU-idle savings `CONFIG_PM=y` gives today) would matter
  more for a battery-powered wearable than it does on the DK.

## App / UX

- **Surface a "quiet mode" toggle tied to the new adaptive advertising.**
  Right now the fast→slow advertising transition is invisible to the user.
  A small indicator (or an explicit low-power mode the user can force) would
  make the tradeoff — slower reconnect, longer battery life — something the
  wearer chooses rather than something that just happens.
- **LDL result history + trend view.** The app already runs the guided LDL
  test and gets a result; nothing currently stores more than the most
  recent one. Even a simple list of past runs would make hearing changes
  over time visible, which is the actual point of periodic hearing tests.

## Hardware

- **Resolve the BMX160/BMI160 schematic-vs-BOM mismatch.** Already flagged
  earlier this session as real but currently dormant (no firmware code
  touches IMU/accelerometer/gyro functionality yet) — worth fixing before
  it's forgotten and someone orders the wrong part for a production run.

## ML / DSP

- **Compare the -3dB peak-bandwidth heuristic against real problem
  recordings.** `ml/analyze.py`'s heuristic is deliberately simple and
  untrained; once there's a small set of recordings the user actually
  considers "troublesome" vs. "fine," it'd be worth checking whether the
  half-power-bandwidth heuristic actually agrees with human judgment before
  investing in anything more elaborate (e.g. a learned classifier).
- **Time-varying bands, not just one static range.** The current
  FreqRange characteristic (and `ml/`'s output) describes a single fixed
  band. Real problem noise (e.g. a siren) can sweep or shift over time —
  worth deciding whether that's a real use case before building for it.

## Safety

- **Extend the independent tone-safety ceiling's pattern to the real
  filter path.** `tone_safety.c`'s validation + watchdog approach (clamp
  + timeout, enforced firmware-side regardless of what the app sends) only
  covers the LDL test tone today. Once real ADAU1860 filtering is wired
  up, the same "don't trust the app alone" principle plausibly applies
  there too — worth revisiting once that driver work starts, not before.
