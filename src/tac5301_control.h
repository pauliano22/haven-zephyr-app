/* TAC5301-Q1 codec control -- the alternate (not current) codec driver, for
 * the redesign/tac5301-codec-swap hardware track (haven-dev-board-kicad PR
 * #11). NOT wired into main.c / Kconfig yet, and not tested on real
 * hardware -- see TAC5301_BENCH_EXPERIMENT.md (haven-dev-board-kicad) for
 * the bench verification this is waiting on before it's trusted over the
 * ADAU1860. This file exists so that work isn't blocked on the decision:
 * research and build now, wire in and pick between the two codecs once the
 * bench experiment answers the open questions.
 *
 * Topology this driver assumes (the TAC5301-Q1 redesign board):
 *
 *   CMA-4544PF-W mic ──analog──▶ TAC5301 ADC: 3 biquads (ch1 slots A/B/C)
 *                                   │
 *                        ADC-to-DAC loopback mixer (MIXER_CFG0 bit4)
 *                                   │
 *                                   ▼
 *                       TAC5301 DAC: 3 biquads (ch1 slots A/B/C) ──▶ speaker
 *
 * Entirely inside the codec, same as the ADAU1860 -- the nRF5340 only
 * writes coefficients over I2C, same R1 requirement (hear-through latency
 * well under 1ms) the architecture memo established. Unlike the ADAU1860,
 * Haven's 5 bands don't fit in one DSP engine: they split 3 ADC-side +
 * 2 DAC-side (or similar) across two independently-clocked biquad chains.
 * See tac5301_control_apply_filters()'s doc comment for exactly how.
 *
 * No enable/reset GPIO exists on this part (unlike the ADAU1860's
 * DAC_ENABLE pin) -- confirmed from the pin table, datasheet section 4.
 * Only power rails + I2C are needed to bring it up.
 */
#ifndef HAVEN_TAC5301_CONTROL_H_
#define HAVEN_TAC5301_CONTROL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* Sample rate the biquad coefficients are designed for. The TAC5301 derives
 * every internal clock from BCLK/FSYNC via its own PLL (datasheet section
 * 6.3.2) -- no crystal, no MCLK pin -- so this must match whatever rate the
 * nRF5340's I2S peripheral actually drives BCLK/FSYNC at.
 */
#ifdef CONFIG_HAVEN_TAC5301_RATE_HZ
#define TAC5301_RATE_HZ ((float)CONFIG_HAVEN_TAC5301_RATE_HZ)
#else
#define TAC5301_RATE_HZ 48000.0f
#endif

/* RBJ biquad, same form and precision rationale as the ADAU1860 driver's
 * struct adau1860_biquad (double: near-unit-circle poles need more than
 * single precision's ~6e-8 before fixed-point quantisation) -- deliberately
 * the same shape so calc_band_coeffs() can be shared/ported verbatim rather
 * than re-derived.
 */
struct tac5301_biquad {
	double b0, b1, b2, a1, a2;
};

/* Which of the two biquad chains (and which of that chain's 3 channel-1
 * slots) a given Haven band is assigned to. Exposed so tests and callers
 * can reason about the split explicitly rather than it being an opaque
 * internal index.
 */
enum tac5301_bq_chain { TAC5301_CHAIN_ADC = 0, TAC5301_CHAIN_DAC = 1 };

/* Power up and configure the codec: ADC input source/coupling, ultra-low-
 * latency ADC+DAC filters, 3-biquads-per-channel on both sides, the ADC-to-
 * DAC loopback mixer, MICBIAS, and ADC/DAC power -- the sequence verified
 * against the real register map in TAC5301_BENCH_EXPERIMENT.md. Returns 0,
 * or a negative errno (-ENODEV if the I2C bus isn't ready, I2C errors
 * otherwise). Unlike the ADAU1860 there is no STATUS/power-up-complete
 * register to poll -- the device is expected ready after its supply
 * sequencing settles (datasheet section 6.4), so this does not block.
 */
int tac5301_control_init(void);

/* Compute RBJ notch / peaking-cut coefficients for each band at
 * TAC5301_RATE_HZ and write them into the 3 ADC-side + 3 DAC-side channel-1
 * biquad slots. Haven supports up to PROTOCOL_MAX_BANDS (5) bands; this
 * part has 3 usable slots per chain (6 total) -- bands fill the ADC chain
 * first (up to 3), then the DAC chain (up to 2 more, since Haven never
 * sends more than 5), with unused slots in both chains set to the
 * documented unity/pass-through coefficients so stale bands never linger.
 * The split order (ADC-first) is an implementation choice, not something
 * the datasheet mandates -- cascading biquads is commutative (the combined
 * transfer function is the product of each section's response regardless
 * of which physical chain or order they run in), confirmed in
 * TAC5301_EVALUATION.md's reasoning about the loopback path not passing
 * through any resampling between the two chains.
 */
int tac5301_control_apply_filters(const struct filter_band *bands, size_t count);

/* true = all 6 biquad slots unity (raw hear-through through the loopback
 * mixer with no shaping). Mirrors adau1860_control_set_bypass()'s contract:
 * main.c re-applies the current bands on the next MULTI_FILTER to end it.
 */
int tac5301_control_set_bypass(bool enabled);

/* DAC_CH1A_DVOL / DAC_CH1B_DVOL (datasheet 7.1.1.73/7.1.1.75, registers
 * 0x67/0x69 -- the part has independent A/B differential DAC sub-channels
 * for its one logical output channel, both written together here so they
 * stay matched): 0.5dB steps, 1d=-100dB .. 201d=0dB .. 255d=+27dB. Applied
 * on the DAC side only, mirroring the ADAU1860 driver's volume contract (a
 * single user-facing level, not independent per-side gains).
 * 0 % = -100dB (code 1, quietest non-muted step), 100 % = 0dB (code 201),
 * log-linear between -- same shape as adau1860_control_set_volume_pct's
 * mapping (quietest..0dB), not its exact table, which is codec-specific.
 */
int tac5301_control_set_volume_pct(uint8_t volume_pct);

/* Hard mute via the DVOL registers' own dedicated code 0 ("Digital Volume
 * is muted" -- a distinct encoding from the 1..255 dB range, not just the
 * bottom of it), so muting never disturbs the tracked volume_pct: unmuting
 * restores exactly the code that was active before. Does not touch the
 * filter coefficients.
 */
int tac5301_control_set_mute(bool muted);

/* ── Hardware output ceiling ──────────────────────────────────────────────
 * Mirrors adau1860_control_set_output_ceiling_db()'s role (a cap the app
 * protocol never reaches), but NOT its architecture, and that difference
 * is real, not cosmetic: the ADAU1860 has a genuinely separate gain stage
 * (DAC_VOL0) after its FastDSP volume slot, so the ceiling is enforced by
 * hardware the BLE-exposed volume control physically cannot write to. The
 * TAC5301-Q1 has only one DVOL register pair for its entire output gain --
 * datasheet section 7 doesn't give this part a second, independent stage.
 * This driver enforces the ceiling in software instead: every
 * tac5301_control_set_volume_pct() call clamps its own result against the
 * ceiling before writing the single shared register. That is a weaker
 * safety property than the ADAU1860's -- a bug in this driver's own
 * clamping code could in principle let a requested volume exceed the
 * ceiling, which is structurally impossible on the ADAU1860 (the volume-
 * setting code path cannot reach DAC_VOL0 at all). Range -100..+27 dB,
 * matching the DVOL register's own native range (datasheet
 * 7.1.1.73/.75) -- wider than the ADAU1860's 24..-60, a real difference
 * in what each chip's hardware can represent, not a copy-paste number.
 * Rejects out-of-range input (-EINVAL) rather than silently clamping it,
 * matching the ADAU1860 driver's own choice to treat that as a caller
 * bug worth surfacing, not something to paper over.
 */
int tac5301_control_set_output_ceiling_db(int ceiling_db);
int tac5301_control_get_output_ceiling_db(void);

/* ── LDL calibration tone ─────────────────────────────────────────────────
 * Safety-critical -- see tone_safety.c, which owns validation/clamping and
 * the auto-stop watchdog, same as for the ADAU1860 path. The tone itself
 * is synthesised on the nRF5340 (tone_gen.c, codec-agnostic, reused as-is)
 * and streamed over I2S0 into this chip's ASI; this driver's job is
 * routing that into the DAC (MIXER_CFG0: ASI mixer on, loopback mixer off,
 * so the tone plays in isolation rather than mixed with live ambient
 * sound -- see tac5301_control.c's tone_route_engage() for exactly why
 * that pairing matters, not just "ASI on") and back out again cleanly
 * when it stops. NOT the same mechanism as the ADAU1860 driver's DAC_ROUTE0
 * mux switch or its ASRC-lock wait -- both real architectural differences,
 * documented at the implementation, not just asserted equivalent here.
 */
int tac5301_control_set_tone(float f0_hz, float level_db);
int tac5301_control_set_tone_level(float level_db);
int tac5301_control_stop_tone(void);

/* The level mapping above, exposed for tests and the calibration tool,
 * same contract as adau1860_tone_gain_q15(). */
int32_t tac5301_tone_gain_q15(float level_db);

/* BLE link lifecycle hooks (main.c). Hearing protection must keep working
 * with the phone gone, so neither touches the filter state; they log (and
 * on disconnect, synchronously restore the tone route as a second-layer
 * safety net -- see tac5301_control.c for why this matters independently
 * of tone_gen's own feeder-thread callback).
 */
void tac5301_control_on_ble_connected(void);
void tac5301_control_on_ble_disconnected(void);

#endif /* HAVEN_TAC5301_CONTROL_H_ */
