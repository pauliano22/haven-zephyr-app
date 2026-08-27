# Host-side unit tests

Plain host-gcc tests, no Zephyr build system, no native_sim, no twister.

## Why host-gcc instead of native_sim/twister

Checked feasibility first rather than assuming: `~/ncs` has an initialized
NCS/Zephyr workspace with `zephyr/boards/native/native_sim` present, and
`west`/`gcc` both work in this sandbox, so a real native_sim+twister+ztest
setup is *possible* here in principle. But the actual application
(`boards/nrf5340dk_nrf5340_cpuapp.overlay`, I2C1/I2S0 peripherals, the real
BLE stack) is written against real nRF5340 hardware, and every module this
pass needed to test either has zero Zephyr dependency at all (`protocol.c`)
or pulls in just a couple of Zephyr subsystems (kernel work-queue, BT GATT,
settings) rather than needing the full board. Standing up a twister test
app with the right board/overlay/Kconfig combination to make those
subsystems behave under native_sim, for four fairly small modules, was a
larger and riskier lift than directly compiling the real `.c` files under
host gcc against small fake headers -- and the second approach still tests
the actual production code, not a reimplementation of it. That's the
approach used here.

## How this actually works

Each `test_*.c` file `#include`s the real production `.c` file from
`../../src/` directly into its own translation unit -- not by modifying
that file (no `static` was removed from anything in `src/`), but because
`#include`-ing a `.c` file puts its `static` functions in the *same*
translation unit as the test code, making them directly callable. Real
Zephyr headers (`<zephyr/kernel.h>`, `<zephyr/bluetooth/gatt.h>`,
`<zephyr/settings/settings.h>`, etc.) are swapped for minimal fakes in
`fakes/zephyr/...` via `-Ifakes`. Every fake header says exactly what it
fakes and why in its own header comment.

**One exception**: `test_adau1860_coeffs.c` does NOT include the real
`adau1860_control.c`. That file has a file-scope
`I2C_DT_SPEC_GET(DT_NODELABEL(adau1860))` devicetree macro that expands
against real generated devicetree headers from a Zephyr build -- faking
that convincingly felt like more risk of a *subtly wrong* fake than value,
for one function. Instead, `calc_band_coeffs()`'s formula is copied
verbatim into the test file, clearly marked, with a note that it must be
manually kept in sync if the real function ever changes. The tests
compensate by checking functional/algebraic properties of the RBJ formula
itself (does the resulting filter actually notch the right frequency; does
a known degenerate case reduce to an exact identity filter) rather than
hardcoded expected coefficients, so a transcription mistake is likely to
show up as a test failure rather than silently validating itself.

## What's covered vs. not

| File | What's tested | What's NOT tested |
|---|---|---|
| `protocol.c` | Full parse/clamp/reject logic, real production code, no stubs needed at all | n/a -- this file has no Zephyr/BLE dependency to begin with |
| `mock_audio_pipeline.c` | Real `recompute_filter`/`process_buffer`/`generate_test_tone`/`on_volume_changed`, functional DSP correctness (passband/rejection, gain scaling, memory reset) | The `k_work` scheduling/timing itself (fake no-op) |
| `adau1860_control.c` | `calc_band_coeffs()`'s formula via a documented verbatim copy (see above) | The real file's I2C bus code, init sequence, or anything past coefficient math -- all still placeholder TODOs anyway |
| `gatt_audio_service.c` | Real `write_volume`/`write_freq_range`/`read_volume`/`read_freq_range`/`apply_volume`/`apply_freq_range`, including the accept-vs-reject-vs-clamp distinction and the trusted-path-skips-validation contract | Real BLE transport, ATT bearer, connection handling, or notification delivery (fakes are link-satisfying stubs, not a working GATT server) |
| `settings_store.c` | Real `haven_settings_set()` key dispatch, length validation, read-failure propagation, subtree-vs-leaf name matching (using a REAL reimplementation of `settings_name_steq`'s semantics, not a no-op) | An actual flash/NVS save-then-restore round trip -- there is no native_sim or other Zephyr board target buildable in this sandbox to host a real settings backend against simulated flash. Only `haven_settings_set()`'s own dispatch logic is exercised, with isolated test-double `gatt_audio_set_volume`/`gatt_audio_set_freq_range` standing in for the real consumer. |

## Running

```
./run_tests.sh
```

Builds and runs all five suites with plain gcc, no Zephyr toolchain
required. Exits nonzero if anything fails.
