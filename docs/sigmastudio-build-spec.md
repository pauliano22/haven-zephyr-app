# SigmaStudio+ build spec for the Haven ADAU1860 program

## Why this doc exists

The firmware's DSP control plane (`src/adau1860_control.c`) is fully wired up
to the app/BLE side — GATT characteristics, safety clamps, NVS persistence,
the generic SigmaDSP I2C register framing — but every function that would
actually *do* something to audio is a `TODO(hw-bringup)` stub. The reason:
ADI's SigmaDSP chips (the ADAU1860 included) aren't programmed by writing to
a documented register map. The actual signal-processing program is designed
in SigmaStudio+ (a Windows GUI tool) and exported as a data file; the
firmware's job is to load that file's byte sequence over I2C at boot and
then write into a few specific parameter-RAM addresses at runtime. Nobody
has built that SigmaStudio+ project yet — this doc specifies exactly what it
needs to contain so that step can happen once, cleanly, instead of by trial
and error.

**I don't have SigmaStudio+ and have never operated it** — everything below
is a signal-chain / requirements spec (what blocks, what parameters, what
needs to be exported), not click-by-click UI instructions. Menu names and
exact workflow steps can and do vary by SigmaStudio+ version; cross-check
those against ADI's own SigmaStudio user guide as you go. Correct the spec
below if the actual toolbox doesn't have the exact blocks named — the
*behavior* required is what matters, not the specific cell name.

## Target hardware assumptions (confirmed against the real schematic + firmware)

- Chip: ADAU1860, I2C control port at address `0x64` (schematic ADDR0/ADDR1
  strapping — see `boards/nrf5340dk_nrf5340_cpuapp.overlay`'s comments).
- Sample rate: **48 kHz**, assumed by `adau1860_control.c`'s
  `ADAU1860_SAMPLE_RATE_HZ` in its own (currently host-side, not yet
  DSP-side) biquad coefficient math. If the actual project uses a different
  rate, that constant needs to change to match — don't silently pick 48 kHz
  in SigmaStudio+ without updating firmware, or the two will disagree about
  what a given `f0_hz` means.
- Clocking: no MCLK net exists between the nRF5340 module and the ADAU1860
  in the schematic; the ADAU1860 has its own dedicated 24.576 MHz crystal.
  This strongly suggests the ADAU1860 is the I2S clock master (it generates
  BCLK/LRCLK) and the nRF5340 is the I2S slave — the firmware's I2S pinctrl
  is already set up this way (`I2S_SCK_S`/`I2S_LRCK_S`, see the overlay).
  Confirm this against the ADAU1860 datasheet's clocking section before
  finalizing the SigmaStudio+ project's clock source setting — this was
  inferred from schematic evidence, not read directly off a register-level
  clocking diagram.
- Coefficient number format: **not yet confirmed** — the ADAU1860's DSP core
  could use 8.24 fixed-point (typical for older SigmaDSP parts) or a float
  core (some newer SigmaDSP chips do). SigmaStudio+ will show this in the
  cell's own coefficient display and/or the export; whichever it is, note
  it explicitly when you export, since `adau1860_control.c`'s
  `calc_band_coeffs()` currently computes plain `float` biquad coefficients
  that will need a conversion step if the core is fixed-point.

## Required signal chain

Five things need to exist in the project, matching what the firmware
already sends over BLE/NUS and expects to control:

### 1. Up to 5 independently-adjustable biquad (2nd-order IIR) filter stages, cascaded

Maps to `struct filter_band` (`src/protocol.h`): each band carries
`f0_hz` (center/corner frequency), `q` (Q factor), and `atten_db` (0 to 40
dB of cut at `f0_hz`; at exactly 40 dB the firmware treats it as a full
notch rather than a peaking cut — see `calc_band_coeffs()`'s two branches
in `adau1860_control.c` for the exact RBJ cookbook math already validated
host-side).

- Each stage needs runtime-writable coefficients (b0/b1/b2/a1/a2, or
  whatever parameterization SigmaStudio+'s "second-order notch/peaking
  filter" cell exposes) — the firmware computes coefficients on the host
  side per the existing math and writes them in; it does not expect
  SigmaStudio+ to compute them from a frequency/Q/gain UI at runtime.
  A "General Second-Order Filter" or "Biquad" cell in double-precision
  mode (if the toolbox offers a choice) is the right kind of block, not a
  fixed EQ curve.
- Unused stages (fewer than 5 bands active) need to be safely
  bypassable/flat — either a dedicated bypass per stage, or coefficients
  set to a unity pass-through (b0=1, everything else 0) work equally well
  from the firmware's side; whichever is simpler to implement reliably in
  the project.
- Record the parameter RAM address (or safeload target) for each stage's
  coefficient block — this is the actual number `adau1860_control.c` needs
  in place of today's `PARAM_RAM_STAGE(i)` placeholder.

### 2. A single bypass switch for the entire filter chain

Maps to `DSP_CMD_BYPASS` / `adau1860_control_set_bypass()`. A mux or
crossfade cell that can route the signal around all 5 stages when
disabled. Needs one register address + bit (or a full register write, if
that's how the cell exposes it) that firmware can flip.

### 3. A tone generator (sine) with independently controllable frequency and level

Maps to `DSP_CMD_TONE_START` / `TONE_LEVEL` / `TONE_STOP`
(`tone_safety.c` owns the safety clamping/watchdog on top of this;
`adau1860_control_set_tone()`/`set_tone_level()`/`stop_tone()` are the
functions that need real register addresses). Requirements:

- Frequency needs to be runtime-settable, not fixed at compile time in
  the SigmaStudio+ project — the LDL test in the app sweeps through
  several fixed test frequencies (see `LDL_TEST_FREQUENCIES_HZ` in the
  mobile app), so the DSP needs a tone generator whose frequency register
  the firmware can rewrite per `TONE_START`.
- Level needs a *separate* runtime-settable gain from frequency, since
  `TONE_LEVEL` updates loudness mid-tone without restarting/re-picking the
  frequency, and `tone_safety.c` already enforces `PROTOCOL_TONE_LEVEL_MAX_DB
  = 85.0` dB and a minimum of 0 dB firmware-side, independent of whatever
  cap the app itself applies — the DSP just needs to accept whatever gain
  value it's given in that range without its own clamping getting in the
  way (or if it does clamp, know what its own max is so firmware doesn't
  ask for more than the DSP can represent).
- The tone needs to be independently mutable (`TONE_STOP`) without
  disturbing the main filter/bypass signal path — i.e. summed in after the
  filter chain, or on a separate output mix, not injected pre-filter where
  stopping it would also require touching filter state.

### 4. Standard I/O

Whatever input/output routing matches the real board's analog/digital I/O
for this design (mic input(s), speaker/receiver output) — this doc doesn't
specify that since it depends on the final board's analog front-end, which
is a separate, still-in-progress hardware question.

## What to export, and what to send back

Once the project is built and verified in SigmaStudio+'s own simulation/
capture window:

1. **The firmware/self-boot export** — whatever SigmaStudio+ calls the
   full byte-sequence-for-microcontroller-boot output (this is the
   "capture window" byte stream `adau1860_control_init()`'s TODO refers
   to). This is what gets streamed over I2C at boot to bring the DSP up
   running this exact program.
2. **The parameter RAM address map** — specifically, the addresses for:
   each of the (up to 5) filter stages' coefficient blocks, the bypass
   register, the tone generator's frequency register, and the tone
   generator's level/gain register. SigmaStudio+'s "Parameter Capture" or
   cell inspector should show each cell's actual RAM address once
   compiled.
3. **The coefficient number format actually used** (see clocking/format
   note above) — needed to write the conversion from this project's
   `float` biquad math to whatever the DSP core expects.
4. **The confirmed sample rate**, if it ended up being anything other than
   48 kHz.

With those four things, the `TODO(hw-bringup)` stubs in
`adau1860_control.c` become real, testable code rather than placeholders —
this is genuinely the last missing piece before real audio processing can
happen on hardware.
