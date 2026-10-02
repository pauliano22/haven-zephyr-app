/* Host tests for tac5301_control.c -- same approach as
 * test_adau1860_coeffs.c: the real production file is included directly so
 * a formula change here is caught automatically, with a fake I2C layer
 * (tests/host/fakes_tac5301/zephyr/drivers/i2c.h -- note the different
 * include path from the ADAU1860 tests, since this part's I2C wire format
 * is genuinely different, not just a different address space) that records
 * every write.
 *
 * NOT a hardware verification -- these tests check that the driver computes
 * and sends the coefficients/registers it claims to, not that a real
 * TAC5301-Q1 produces the expected acoustic result. See
 * TAC5301_BENCH_EXPERIMENT.md (haven-dev-board-kicad) for that.
 */
#include "test_harness.h"

#include <math.h>
#include <string.h>

#include "../../src/tone_gen.c" /* tac5301_control.c's tone functions call into it */
#include "../../src/tac5301_control.c"

const struct device haven_fake_i2c_bus_dev = { .name = "fake-i2c-tac5301" };

static double q31_decode(uint32_t w)
{
	return (double)(int32_t)w / 2147483648.0;
}

/* |pole| of z^2 + A1 z + A2 (denominator 1 + A1 z^-1 + A2 z^-2) -- same
 * stability check as the ADAU1860 test, reused since it's pure math.
 */
static double pole_radius(double A1, double A2)
{
	double disc = A1 * A1 - 4.0 * A2;

	if (disc < 0.0) {
		return sqrt(A2);
	}
	double r1 = fabs((-A1 + sqrt(disc)) / 2.0);
	double r2 = fabs((-A1 - sqrt(disc)) / 2.0);

	return r1 > r2 ? r1 : r2;
}

static void test_notch_dc_gain_is_unity(void)
{
	struct filter_band band = { .f0_hz = 1000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct tac5301_biquad c;

	calc_band_coeffs(&band, &c);
	/* DC gain H(1) = (b0+b1+b2)/(1+a1+a2); a pure notch must pass DC. */
	double dc = (c.b0 + c.b1 + c.b2) / (1.0 + c.a1 + c.a2);

	CHECK(fabs(dc - 1.0) < 1e-9);
}

static void test_notch_is_stable(void)
{
	struct filter_band band = { .f0_hz = 4500.0f, .q = 20.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB };
	struct tac5301_biquad c;

	calc_band_coeffs(&band, &c);
	CHECK(pole_radius(c.a1, c.a2) < 1.0);
}

static void test_peaking_cut_stable_across_range(void)
{
	for (float f0 = 200.0f; f0 <= 8000.0f; f0 += 400.0f) {
		for (float q = 1.0f; q <= 20.0f; q += 3.0f) {
			struct filter_band band = { .f0_hz = f0, .q = q, .atten_db = 10.0f };
			struct tac5301_biquad c;

			calc_band_coeffs(&band, &c);
			CHECK(pole_radius(c.a1, c.a2) < 1.0);
		}
	}
}

static void test_q31_encode_roundtrip(void)
{
	double vals[] = { 0.0, 1.0, -1.0, 0.5, -0.5, 0.999999, -0.999999 };

	for (size_t i = 0; i < ARRAY_SIZE(vals); i++) {
		uint32_t w = q31_encode(vals[i]);
		double back = q31_decode(w);

		CHECK(fabs(back - vals[i]) < 1e-8 || (vals[i] == 1.0 && back > 0.9999999));
	}
}

static void test_q31_encode_saturates(void)
{
	CHECK(q31_encode(2.0) == 0x7FFFFFFFu);
	CHECK(q31_encode(-2.0) == 0x80000000u);
}

static void test_unity_biquad_matches_reset_value(void)
{
	/* The chip's own reset value for N0 is 0x7FFFFFFF (datasheet register
	 * map) -- this is what tac5301_regs.h's header comment infers as
	 * Q1.31's closest representable +1.0. Confirm the driver's own
	 * "unity" constant matches that exactly, not just "close to 1.0" --
	 * if a bench readback ever shows the real reset byte pattern differs,
	 * this is the test that should fail first. */
	CHECK(unity_biquad_q31[0] == 0x7FFFFFFFu);
	for (int i = 1; i < TAC5301_BIQUAD_COEFF_COUNT; i++) {
		CHECK(unity_biquad_q31[i] == 0);
	}
}

/* Cross-check biquad_to_words() against TI's own documented conversion
 * (SLAAEH6 section 3.2: N0=b0, N1=b1/2, N2=b2, D1=-a1/2, D2=-a2, then
 * Q1.31), using round numbers chosen so every result is exactly
 * representable -- no rounding ambiguity in the assertion. This is the
 * test that would have caught the original bug (which used N0=b0, N1=b1,
 * N2=b2, D1=a1, D2=a2 -- unnegated and unscaled on N1/D1).
 */
static void test_biquad_to_words_matches_ti_app_note_conversion(void)
{
	struct tac5301_biquad c = { .b0 = 0.5, .b1 = 0.5, .b2 = 0.125, .a1 = -0.5, .a2 = 0.25 };
	uint32_t words[TAC5301_BIQUAD_COEFF_COUNT];

	biquad_to_words(&c, words);
	/* N0 = b0 = 0.5 -> 0.5 * 2^31 = 0x40000000 */
	CHECK(words[0] == 0x40000000u);
	/* N1 = b1/2 = 0.25 -> 0x20000000 (NOT b1=0.5 -> 0x40000000, the bug) */
	CHECK(words[1] == 0x20000000u);
	/* N2 = b2 = 0.125 -> 0x10000000 (unscaled, unlike N1) */
	CHECK(words[2] == 0x10000000u);
	/* D1 = -a1/2 = -(-0.5)/2 = 0.25 -> 0x20000000 (negated AND halved) */
	CHECK(words[3] == 0x20000000u);
	/* D2 = -a2 = -0.25 -> two's complement of 0.25*2^31 = 0xE0000000
	 * (negated, NOT halved -- distinguishes this from D1's treatment) */
	CHECK(words[4] == 0xE0000000u);
}

/* Pins the DAC biquad page numbers against literal values transcribed
 * directly from the datasheet's own page-7.2.5/7.2.6/7.2.7 section
 * headers ("consists of the programmable coefficients for the DAC
 * biquad 1 to biquad 6 filters" = page 15; "...7 to biquad 12..." = page
 * 16; page 17 is a DIFFERENT set of registers entirely -- ASI DIN mixer /
 * loopback mixer / DAC first-order IIR filter, not biquads at all).
 * Checked against the literal numbers, not the macros that define them --
 * a self-referential check against TAC5301_PAGE_DAC_BQ_1_6 would still
 * pass if that constant were wrong, which is exactly the bug this test
 * exists to catch (the original value was 16/17, one page off, which
 * would have written biquad-9 coefficients into live mixer/HPF
 * configuration registers instead of a biquad).
 */
static void test_dac_biquad_pages_match_datasheet_literally(void)
{
	CHECK(TAC5301_PAGE_DAC_BQ_1_6 == 15);
	CHECK(TAC5301_PAGE_DAC_BQ_7_12 == 16);
	CHECK(TAC5301_DAC_CH1_BQ_A_PAGE == 15);
	CHECK(TAC5301_DAC_CH1_BQ_B_PAGE == 15);
	CHECK(TAC5301_DAC_CH1_BQ_C_PAGE == 16);
	/* Page 17 must never be touched by biquad writes -- confirm no slot
	 * page constant equals 17. */
	CHECK(TAC5301_DAC_CH1_BQ_A_PAGE != 17);
	CHECK(TAC5301_DAC_CH1_BQ_B_PAGE != 17);
	CHECK(TAC5301_DAC_CH1_BQ_C_PAGE != 17);
}

static void test_init_sets_3_biquads_per_channel(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	int err = tac5301_control_init();

	CHECK(err == 0);

	const struct haven_fake_i2c_xfer *dsp0 = haven_fake_i2c_last_write_page(0, TAC5301_REG_DSP_CFG0);
	const struct haven_fake_i2c_xfer *dsp1 = haven_fake_i2c_last_write_page(0, TAC5301_REG_DSP_CFG1);

	CHECK(dsp0 != NULL && dsp1 != NULL);
	/* bits[3:2] = 11b in both -- the bug an earlier doc draft had. */
	CHECK((dsp0->data[0] & 0x0C) == TAC5301_DSP_BQ_CFG_3_PER_CHANNEL);
	CHECK((dsp1->data[0] & 0x0C) == TAC5301_DSP_BQ_CFG_3_PER_CHANNEL);
	/* bits[7:6] = 10b (ultra-low-latency) in both. */
	CHECK((dsp0->data[0] & 0xC0) == TAC5301_DSP_DECIM_FILT_ULTRA_LOW_LATENCY);
	CHECK((dsp1->data[0] & 0xC0) == TAC5301_DSP_INTERP_FILT_ULTRA_LOW_LATENCY);
}

static void test_init_enables_only_loopback_mixer(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();

	const struct haven_fake_i2c_xfer *mix = haven_fake_i2c_last_write_page(0, TAC5301_REG_MIXER_CFG0);

	CHECK(mix != NULL);
	CHECK(mix->data[0] == TAC5301_MIXER_EN_LOOPBACK_MIXER);
}

static void test_init_powers_up_adc_dac_micbias_together(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();

	const struct haven_fake_i2c_xfer *pwr = haven_fake_i2c_last_write_page(0, TAC5301_REG_PWR_CFG);

	CHECK(pwr != NULL);
	CHECK(pwr->data[0] == (TAC5301_PWR_ADC_PDZ | TAC5301_PWR_DAC_PDZ | TAC5301_PWR_MICBIAS_PDZ));
}

static void test_apply_filters_splits_adc_then_dac(void)
{
	struct filter_band bands[5] = {
		{ .f0_hz = 1000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB },
		{ .f0_hz = 2000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB },
		{ .f0_hz = 3000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB },
		{ .f0_hz = 4000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB },
		{ .f0_hz = 5000.0f, .q = 5.0f, .atten_db = PROTOCOL_ATTEN_MAX_DB },
	};

	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	haven_fake_i2c_reset(); /* isolate from init's own writes */

	int err = tac5301_control_apply_filters(bands, 5);

	CHECK(err == 0);

	/* Bands 0,1,2 -> ADC channel-1 slots A/B/C; bands 3,4 -> DAC slots A/B;
	 * DAC slot C gets the unity default (only 5 bands, 6 slots total). */
	const struct haven_fake_i2c_xfer *adc_a =
		haven_fake_i2c_last_write_page(TAC5301_ADC_CH1_BQ_A_PAGE, TAC5301_ADC_CH1_BQ_A_REG);
	const struct haven_fake_i2c_xfer *dac_c =
		haven_fake_i2c_last_write_page(TAC5301_DAC_CH1_BQ_C_PAGE, TAC5301_DAC_CH1_BQ_C_REG);

	CHECK(adc_a != NULL && adc_a->len == TAC5301_BIQUAD_REG_COUNT);
	CHECK(dac_c != NULL && dac_c->len == TAC5301_BIQUAD_REG_COUNT);

	uint32_t n0 = sys_get_be32(dac_c->data);

	CHECK(n0 == 0x7FFFFFFFu);
}

static void test_apply_filters_zero_bands_is_all_unity(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	haven_fake_i2c_reset();

	int err = tac5301_control_apply_filters(NULL, 0);

	CHECK(err == 0);

	const struct haven_fake_i2c_xfer *adc_a =
		haven_fake_i2c_last_write_page(TAC5301_ADC_CH1_BQ_A_PAGE, TAC5301_ADC_CH1_BQ_A_REG);

	CHECK(adc_a != NULL);
	uint32_t n0 = sys_get_be32(adc_a->data);

	CHECK(n0 == 0x7FFFFFFFu);
}

static void test_set_bypass_true_calls_apply_filters_empty(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	haven_fake_i2c_reset();

	int err = tac5301_control_set_bypass(true);

	CHECK(err == 0);
	CHECK(haven_fake_i2c_count_writes_page(TAC5301_ADC_CH1_BQ_A_PAGE, TAC5301_ADC_CH1_BQ_A_REG) == 1);
}

static void test_volume_pct_0_is_min_code(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	muted = false;
	haven_fake_i2c_reset();

	tac5301_control_set_volume_pct(0);
	const struct haven_fake_i2c_xfer *a = haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1A_DVOL);
	const struct haven_fake_i2c_xfer *b = haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1B_DVOL);

	CHECK(a != NULL && a->data[0] == TAC5301_DVOL_MIN_CODE);
	CHECK(b != NULL && b->data[0] == TAC5301_DVOL_MIN_CODE);
}

static void test_volume_pct_100_is_unity_code(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	muted = false;
	haven_fake_i2c_reset();

	tac5301_control_set_volume_pct(100);
	const struct haven_fake_i2c_xfer *a = haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1A_DVOL);

	CHECK(a != NULL && a->data[0] == TAC5301_DVOL_UNITY_CODE);
}

static void test_output_ceiling_clamps_volume_pct(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	muted = false;
	output_ceiling_db = 0;

	int err = tac5301_control_set_output_ceiling_db(-20);

	CHECK(err == 0);
	haven_fake_i2c_reset();

	/* Ask for 100% (0dB) -- should be clamped to the -20dB ceiling, not
	 * the chip's own 0dB unity code. This is the single-shared-register
	 * software clamp the header comment describes, not a separate stage. */
	tac5301_control_set_volume_pct(100);
	const struct haven_fake_i2c_xfer *a = haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1A_DVOL);
	int expected_code = (int)lround((-20.0 + 100.0) * 2.0 + (double)TAC5301_DVOL_MIN_CODE);

	CHECK(a != NULL && a->data[0] == (uint8_t)expected_code);
	CHECK(a != NULL && a->data[0] != TAC5301_DVOL_UNITY_CODE);

	output_ceiling_db = 0; /* don't leak into later tests */
}

static void test_output_ceiling_rejects_out_of_range(void)
{
	output_ceiling_db = 0;
	int err_high = tac5301_control_set_output_ceiling_db(28); /* max is +27 */
	int err_low = tac5301_control_set_output_ceiling_db(-101); /* min is -100 */

	CHECK(err_high == -EINVAL);
	CHECK(err_low == -EINVAL);
	CHECK(output_ceiling_db == 0); /* rejected calls must not change state */
}

static void test_lowering_ceiling_immediately_reclamps_current_volume(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	muted = false;
	output_ceiling_db = 0;

	tac5301_control_set_volume_pct(100); /* 0dB, well above the ceiling about to be set */
	haven_fake_i2c_reset();

	/* Lowering the ceiling alone (no new set_volume_pct call) must write
	 * the new, lower value immediately -- mirroring the ADAU1860 driver's
	 * "takes effect immediately" behavior, which matters because the app
	 * protocol can change the ceiling independently of the user touching
	 * volume at all. */
	tac5301_control_set_output_ceiling_db(-50);
	const struct haven_fake_i2c_xfer *a = haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1A_DVOL);
	int expected_code = (int)lround((-50.0 + 100.0) * 2.0 + (double)TAC5301_DVOL_MIN_CODE);

	CHECK(a != NULL && a->data[0] == (uint8_t)expected_code);

	output_ceiling_db = 0;
}

static void test_ceiling_does_not_apply_while_muted(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	muted = false;
	output_ceiling_db = 0;
	tac5301_control_set_mute(true);
	haven_fake_i2c_reset();

	/* Lowering the ceiling while muted must not un-mute the device by
	 * writing a non-zero code -- the dedicated mute code stays in place. */
	int err = tac5301_control_set_output_ceiling_db(-30);

	CHECK(err == 0);
	CHECK(haven_fake_i2c_count_writes_page(0, TAC5301_REG_DAC_CH1A_DVOL) == 0);

	output_ceiling_db = 0;
}

/* ── Tone-path routing ────────────────────────────────────────────────── */

static void reset_tone_state(void)
{
	haven_fake_i2c_reset();
	haven_fake_i2s_reset();
	initialised = true;
	i2s_ready = true;
	tone_route_engaged = false;
	muted = false;
	current_dvol_code = TAC5301_DVOL_UNITY_CODE;
	atomic_set(&state, TONE_IDLE);
	run_sem.count = 0;
}

static void test_set_tone_starts_i2s_then_engages_asi_mixer_disables_loopback(void)
{
	reset_tone_state();

	CHECK(tac5301_control_set_tone(4000.0f, 30.0f) == 0);

	/* nRF side armed... */
	CHECK(atomic_get(&state) == TONE_RUN);
	CHECK(atomic_get(&target_gain) == tac5301_tone_gain_q15(30.0f));

	/* ...codec side: mute -> MIXER_CFG0 = ASI only (loopback explicitly
	 * off, not just "ASI also on") -> unmute, in that order. Getting the
	 * mixer write wrong (e.g. ASI | LOOPBACK instead of ASI alone) would
	 * mix the tone with live ambient sound -- this is the specific thing
	 * tone_route_engage()'s header comment flags as the real risk, so the
	 * test checks the exact byte, not just "a write happened". */
	CHECK(tone_route_engaged);
	const struct haven_fake_i2c_xfer *mix = haven_fake_i2c_last_write_page(0, TAC5301_REG_MIXER_CFG0);

	CHECK(mix != NULL && mix->data[0] == TAC5301_MIXER_EN_DAC_ASI_MIXER);

	int seen_mute = -1, seen_route = -1, seen_unmute = -1;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		const struct haven_fake_i2c_xfer *x = &haven_fake_i2c_log[i];

		if (x->reg == TAC5301_REG_DAC_CH1A_DVOL && x->data[0] == TAC5301_DVOL_MUTE_CODE &&
		    seen_mute < 0) {
			seen_mute = (int)i;
		}
		if (x->reg == TAC5301_REG_MIXER_CFG0) {
			seen_route = (int)i;
		}
		if (x->reg == TAC5301_REG_DAC_CH1A_DVOL && x->data[0] == TAC5301_DVOL_UNITY_CODE) {
			seen_unmute = (int)i;
		}
	}
	CHECK(seen_mute >= 0 && seen_route > seen_mute && seen_unmute > seen_route);

	/* Filters untouched by the tone: no biquad coefficient writes. */
	CHECK(haven_fake_i2c_count_writes_page(TAC5301_ADC_CH1_BQ_A_PAGE, TAC5301_ADC_CH1_BQ_A_REG) == 0);

	/* Level update: nRF only, no codec I2C traffic. */
	size_t before = haven_fake_i2c_log_count;

	CHECK(tac5301_control_set_tone_level(50.0f) == 0);
	CHECK(atomic_get(&target_gain) == tac5301_tone_gain_q15(50.0f));
	CHECK(haven_fake_i2c_log_count == before);

	/* Stop: generator told to drain; codec route NOT yet restored (that
	 * waits for the link to actually go quiet). */
	CHECK(tac5301_control_stop_tone() == 0);
	CHECK(atomic_get(&state) == TONE_STOPPING);
	CHECK(tone_route_engaged);
	CHECK(haven_fake_i2c_log_count == before);

	/* ...then the feeder's completion callback restores it under mute,
	 * back to loopback-only (NOT loopback | asi -- the same specific
	 * mistake would matter in reverse here too). */
	haven_fake_i2c_reset();
	on_tone_stopped();
	CHECK(!tone_route_engaged);
	mix = haven_fake_i2c_last_write_page(0, TAC5301_REG_MIXER_CFG0);
	CHECK(mix != NULL && mix->data[0] == TAC5301_MIXER_EN_LOOPBACK_MIXER);
}

static void test_tone_engage_respects_existing_mute(void)
{
	reset_tone_state();
	muted = true;
	current_dvol_code = TAC5301_DVOL_UNITY_CODE;

	/* A device that's already user-muted must stay silent through a tone
	 * start -- the route switch's own "unmute after switching" step must
	 * write the mute code, not current_dvol_code, or the tone would
	 * briefly leak out despite the user having muted the device. */
	CHECK(tac5301_control_set_tone(4000.0f, 30.0f) == 0);
	CHECK(tone_route_engaged);

	const struct haven_fake_i2c_xfer *last_dvol =
		haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1A_DVOL);

	CHECK(last_dvol != NULL && last_dvol->data[0] == TAC5301_DVOL_MUTE_CODE);
}

static void test_ble_disconnect_restores_route_even_if_engaged(void)
{
	reset_tone_state();
	CHECK(tac5301_control_set_tone(4000.0f, 30.0f) == 0);
	CHECK(tone_route_engaged);
	haven_fake_i2c_reset();

	/* Second-layer safety net, independent of tone_gen's own feeder-
	 * thread callback -- a disconnect must restore the route
	 * synchronously even if that callback is late or never comes. */
	tac5301_control_on_ble_disconnected();

	CHECK(!tone_route_engaged);
	const struct haven_fake_i2c_xfer *mix = haven_fake_i2c_last_write_page(0, TAC5301_REG_MIXER_CFG0);

	CHECK(mix != NULL && mix->data[0] == TAC5301_MIXER_EN_LOOPBACK_MIXER);
}

static void test_tone_gain_q15_boundary_values(void)
{
	CHECK(tac5301_tone_gain_q15(85.0f) == TONE_GEN_GAIN_ONE); /* at full scale */
	CHECK(tac5301_tone_gain_q15(120.0f) == TONE_GEN_GAIN_ONE); /* clamped, not overflowed */
	CHECK(tac5301_tone_gain_q15(-20.0f) == 0); /* below the 16-bit floor */

	int32_t at_minus_6db = tac5301_tone_gain_q15(85.0f - 6.0206f);

	CHECK(at_minus_6db > TONE_GEN_GAIN_ONE / 2 - 50 && at_minus_6db < TONE_GEN_GAIN_ONE / 2 + 50);
}

static void test_mute_uses_dedicated_zero_code_and_restores(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	muted = false;
	tac5301_control_set_volume_pct(75);
	int code_before = current_dvol_code;

	haven_fake_i2c_reset();
	tac5301_control_set_mute(true);
	const struct haven_fake_i2c_xfer *muted_write = haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1A_DVOL);

	CHECK(muted_write != NULL && muted_write->data[0] == TAC5301_DVOL_MUTE_CODE);

	haven_fake_i2c_reset();
	tac5301_control_set_mute(false);
	const struct haven_fake_i2c_xfer *unmuted_write = haven_fake_i2c_last_write_page(0, TAC5301_REG_DAC_CH1A_DVOL);

	CHECK(unmuted_write != NULL && unmuted_write->data[0] == (uint8_t)code_before);
}

static void test_set_volume_while_muted_does_not_unmute(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	tac5301_control_init();
	muted = false;
	tac5301_control_set_mute(true);

	haven_fake_i2c_reset();
	int err = tac5301_control_set_volume_pct(50);

	CHECK(err == 0);
	CHECK(haven_fake_i2c_count_writes_page(0, TAC5301_REG_DAC_CH1A_DVOL) == 0);
}

static void test_page_select_only_sent_once_per_page(void)
{
	haven_fake_i2c_reset();
	initialised = false;
	current_page = -1;
	tac5301_control_init();

	/* init() does several page-0 writes in a row; PAGE_CFG(0) should be
	 * sent at most once if the device starts on page 0 already (it does,
	 * per the reset state) -- or at least not once per register write. */
	size_t page_writes = haven_fake_i2c_count_writes_page(0, TAC5301_REG_PAGE_CFG);
	size_t total_page0_regs = 0;

	for (size_t i = 0; i < haven_fake_i2c_log_count; i++) {
		if (haven_fake_i2c_log_page[i] == 0 && haven_fake_i2c_log[i].reg != TAC5301_REG_PAGE_CFG) {
			total_page0_regs++;
		}
	}
	CHECK(page_writes <= 1 && total_page0_regs >= 5);
}

int main(void)
{
	RUN(test_notch_dc_gain_is_unity);
	RUN(test_notch_is_stable);
	RUN(test_peaking_cut_stable_across_range);
	RUN(test_q31_encode_roundtrip);
	RUN(test_q31_encode_saturates);
	RUN(test_unity_biquad_matches_reset_value);
	RUN(test_dac_biquad_pages_match_datasheet_literally);
	RUN(test_biquad_to_words_matches_ti_app_note_conversion);
	RUN(test_init_sets_3_biquads_per_channel);
	RUN(test_init_enables_only_loopback_mixer);
	RUN(test_init_powers_up_adc_dac_micbias_together);
	RUN(test_apply_filters_splits_adc_then_dac);
	RUN(test_apply_filters_zero_bands_is_all_unity);
	RUN(test_set_bypass_true_calls_apply_filters_empty);
	RUN(test_volume_pct_0_is_min_code);
	RUN(test_volume_pct_100_is_unity_code);
	RUN(test_output_ceiling_clamps_volume_pct);
	RUN(test_output_ceiling_rejects_out_of_range);
	RUN(test_lowering_ceiling_immediately_reclamps_current_volume);
	RUN(test_ceiling_does_not_apply_while_muted);
	RUN(test_set_tone_starts_i2s_then_engages_asi_mixer_disables_loopback);
	RUN(test_tone_engage_respects_existing_mute);
	RUN(test_ble_disconnect_restores_route_even_if_engaged);
	RUN(test_tone_gain_q15_boundary_values);
	RUN(test_mute_uses_dedicated_zero_code_and_restores);
	RUN(test_set_volume_while_muted_does_not_unmute);
	RUN(test_page_select_only_sent_once_per_page);
	return haven_test_summary("test_tac5301_coeffs");
}
