/* TAC5301-Q1 codec driver.
 *
 * Register sequence, addresses, and bit fields are sourced directly from
 * the real datasheet (TI SLASFD9A Rev. A) -- see tac5301_regs.h's header
 * comment and TAC5301_BENCH_EXPERIMENT.md (haven-dev-board-kicad) for the
 * verification trail, including two real errors an earlier draft of that
 * doc had (missing biquad-count bits, wrong channel-1 filter numbers) that
 * are already corrected in tac5301_regs.h.
 *
 * The RBJ coefficient math (calc_band_coeffs) is the same formula as
 * adau1860_control.c's, since it's codec-agnostic cookbook math -- only the
 * encoding (Q1.31 inferred, not ADAU1860's Q5.27) and the register layout
 * differ.
 *
 * NOT YET RUN ON REAL HARDWARE. This exists so firmware work isn't blocked
 * on the ADAU1860-vs-TAC5301-Q1 decision -- see tac5301_control.h's header
 * comment.
 */
#include "tac5301_control.h"
#include "tac5301_regs.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(tac5301_control, LOG_LEVEL_INF);

#define TAC5301_NODE DT_NODELABEL(tac5301)
static const struct i2c_dt_spec bus = I2C_DT_SPEC_GET(TAC5301_NODE);

static bool initialised;
static int current_dvol_code = TAC5301_DVOL_UNITY_CODE;
static bool muted;

/* ── Control-port I/O ────────────────────────────────────────────────────
 * One byte of register address, then the payload, in a single I2C write
 * (datasheet 6.4.1) -- NOT the ADAU1860's 4-byte address scheme. The
 * register space is paged (register 0x00 on every page re-selects it);
 * track the last-selected page so repeated writes to the same page don't
 * re-send PAGE_CFG every time, matching how a real bring-up sequence would
 * minimise bus traffic.
 */
static int current_page = -1; /* unknown until the first select_page() */

static int select_page(uint8_t page)
{
	uint8_t buf[2];
	int err;

	if (current_page == page) {
		return 0;
	}
	buf[0] = TAC5301_REG_PAGE_CFG;
	buf[1] = page;
	err = i2c_write_dt(&bus, buf, sizeof(buf));
	if (err) {
		LOG_ERR("I2C page-select to %u failed: %d", page, err);
		return err;
	}
	current_page = page;
	return 0;
}

static int reg_write_page(uint8_t page, uint8_t reg, const uint8_t *data, size_t len)
{
	uint8_t buf[1 + 32];
	int err;

	if (len > sizeof(buf) - 1) {
		return -EINVAL;
	}
	err = select_page(page);
	if (err) {
		return err;
	}
	buf[0] = reg;
	memcpy(&buf[1], data, len);
	err = i2c_write_dt(&bus, buf, 1 + len);
	if (err) {
		LOG_ERR("I2C write page %u reg 0x%02x (%u B) failed: %d", page, reg,
			(unsigned int)len, err);
	}
	return err;
}

static int reg_write8_page(uint8_t page, uint8_t reg, uint8_t val)
{
	return reg_write_page(page, reg, &val, 1);
}

/* Page 0 is the common case -- every bring-up register this driver touches
 * outside the biquad coefficient pages lives there.
 */
static int reg_write8(uint8_t reg, uint8_t val)
{
	return reg_write8_page(0, reg, val);
}

/* ── Coefficient math ──────────────────────────────────────────────────────
 * Identical RBJ cookbook formula to adau1860_control.c's calc_band_coeffs
 * (validated there against the Teensy prototype and tinnitus_dsp/
 * test_filter.py) -- codec-agnostic math, only the target struct type and
 * sample rate constant differ.
 */
static void calc_band_coeffs(const struct filter_band *band, struct tac5301_biquad *c)
{
	double fs = (double)TAC5301_RATE_HZ;
	double w0 = 2.0 * M_PI * ((double)band->f0_hz / fs);
	double alpha = sin(w0) / (2.0 * (double)band->q);
	double cosw0 = cos(w0);

	if (band->atten_db >= PROTOCOL_ATTEN_MAX_DB) {
		double a0 = 1.0 + alpha;

		c->b0 = 1.0 / a0;
		c->b1 = -2.0 * cosw0 / a0;
		c->b2 = 1.0 / a0;
		c->a1 = -2.0 * cosw0 / a0;
		c->a2 = (1.0 - alpha) / a0;
	} else {
		double A = pow(10.0, -(double)band->atten_db / 40.0);
		double a0 = 1.0 + alpha / A;

		c->b0 = (1.0 + alpha * A) / a0;
		c->b1 = -2.0 * cosw0 / a0;
		c->b2 = (1.0 - alpha * A) / a0;
		c->a1 = -2.0 * cosw0 / a0;
		c->a2 = (1.0 - alpha / A) / a0;
	}
}

/* Two's-complement Q1.31, saturating -- see tac5301_regs.h's header comment
 * for why this format is inferred (from the reset value 0x7FFFFFFF), not
 * confirmed by an explicit datasheet statement.
 */
static uint32_t q31_encode(double v)
{
	double scaled = v * 2147483648.0; /* 2^31 */

	if (scaled >= 2147483647.0) {
		return 0x7FFFFFFFu;
	}
	if (scaled <= -2147483648.0) {
		return 0x80000000u;
	}
	int32_t i = (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);

	return (uint32_t)i;
}

static const uint32_t unity_biquad_q31[TAC5301_BIQUAD_COEFF_COUNT] = {
	0x7FFFFFFFu, 0, 0, 0, 0, /* N0=~1.0, N1=N2=D1=D2=0 -- matches the chip's own reset values. */
};

/* H(z) = (N0 + N1 z^-1 + N2 z^-2) / (1 + D1 z^-1 + D2 z^-2) -- the
 * unnegated textbook form (D1 = a1, D2 = a2 directly); see tac5301_regs.h
 * for why this sign convention is inferred rather than confirmed.
 */
static void biquad_to_words(const struct tac5301_biquad *c, uint32_t out[TAC5301_BIQUAD_COEFF_COUNT])
{
	out[0] = q31_encode(c->b0);
	out[1] = q31_encode(c->b1);
	out[2] = q31_encode(c->b2);
	out[3] = q31_encode(c->a1);
	out[4] = q31_encode(c->a2);
}

/* Write one biquad's 5 coefficients (20 bytes, big-endian per word, per the
 * register map's own BYT1..BYT4 = bits[31:24]..[7:0] layout) starting at
 * (page, first_reg).
 */
static int write_biquad(uint8_t page, uint8_t first_reg, const uint32_t words[TAC5301_BIQUAD_COEFF_COUNT])
{
	uint8_t buf[TAC5301_BIQUAD_REG_COUNT];

	for (int i = 0; i < TAC5301_BIQUAD_COEFF_COUNT; i++) {
		sys_put_be32(words[i], &buf[i * 4]);
	}
	/* One I2C transaction would need reg_write_page to accept a 20-byte
	 * payload; its buffer is sized for that (see its `buf[1+32]`), so a
	 * single call covers the whole biquad.
	 */
	return reg_write_page(page, first_reg, buf, sizeof(buf));
}

static int write_adc_slot(int slot, const uint32_t words[TAC5301_BIQUAD_COEFF_COUNT])
{
	switch (slot) {
	case 0:
		return write_biquad(TAC5301_ADC_CH1_BQ_A_PAGE, TAC5301_ADC_CH1_BQ_A_REG, words);
	case 1:
		return write_biquad(TAC5301_ADC_CH1_BQ_B_PAGE, TAC5301_ADC_CH1_BQ_B_REG, words);
	case 2:
		return write_biquad(TAC5301_ADC_CH1_BQ_C_PAGE, TAC5301_ADC_CH1_BQ_C_REG, words);
	default:
		return -EINVAL;
	}
}

static int write_dac_slot(int slot, const uint32_t words[TAC5301_BIQUAD_COEFF_COUNT])
{
	switch (slot) {
	case 0:
		return write_biquad(TAC5301_DAC_CH1_BQ_A_PAGE, TAC5301_DAC_CH1_BQ_A_REG, words);
	case 1:
		return write_biquad(TAC5301_DAC_CH1_BQ_B_PAGE, TAC5301_DAC_CH1_BQ_B_REG, words);
	case 2:
		return write_biquad(TAC5301_DAC_CH1_BQ_C_PAGE, TAC5301_DAC_CH1_BQ_C_REG, words);
	default:
		return -EINVAL;
	}
}

/* ── Volume/mute ──────────────────────────────────────────────────────── */

static int write_dac_dvol(uint8_t code)
{
	int err = reg_write8(TAC5301_REG_DAC_CH1A_DVOL, code);

	if (err) {
		return err;
	}
	return reg_write8(TAC5301_REG_DAC_CH1B_DVOL, code);
}

/* ── Public API ──────────────────────────────────────────────────────── */

int tac5301_control_init(void)
{
	int err;

	if (!device_is_ready(bus.bus)) {
		LOG_ERR("I2C bus for the TAC5301-Q1 not ready");
		return -ENODEV;
	}

	/* ADC_CH1_CFG0: single-ended, AC-coupled analog input -- matches the
	 * CMA-4544PF-W mic wiring on the redesign board. */
	err = reg_write8(TAC5301_REG_ADC_CH1_CFG0,
			 TAC5301_ADC_INSRC_SINGLE_ENDED | TAC5301_ADC_COUPLING_AC);
	if (err) {
		return err;
	}

	/* DSP_CFG0 (ADC side): ultra-low-latency decimation filter, keep the
	 * reset-default 1Hz HPF, 3 biquads per channel (NOT the reset
	 * default of 2 -- see tac5301_regs.h's header comment on the bug
	 * this corrects). */
	err = reg_write8(TAC5301_REG_DSP_CFG0,
			 TAC5301_DSP_DECIM_FILT_ULTRA_LOW_LATENCY | TAC5301_DSP_HPF_SEL_1HZ |
				 TAC5301_DSP_BQ_CFG_3_PER_CHANNEL);
	if (err) {
		return err;
	}

	/* DSP_CFG1 (DAC side): same corrections, interpolation filter. */
	err = reg_write8(TAC5301_REG_DSP_CFG1,
			 TAC5301_DSP_INTERP_FILT_ULTRA_LOW_LATENCY | TAC5301_DSP_HPF_SEL_1HZ |
				 TAC5301_DSP_BQ_CFG_3_PER_CHANNEL);
	if (err) {
		return err;
	}

	/* MIXER_CFG0: enable the ADC-to-DAC loopback mixer only -- leaves
	 * EN_DAC_ASI_MIXER at its reset-default 0, so the DAC hears purely
	 * the loopback path, nothing from the (unused) I2S data stream. */
	err = reg_write8(TAC5301_REG_MIXER_CFG0, TAC5301_MIXER_EN_LOOPBACK_MIXER);
	if (err) {
		return err;
	}

	/* CH_EN is already 0xCC at reset (all 4 relevant bits on) -- no
	 * write needed, confirmed from the register map's own reset value. */

	/* PWR_CFG: power up ADC, DAC, and MICBIAS together. */
	err = reg_write8(TAC5301_REG_PWR_CFG,
			 TAC5301_PWR_ADC_PDZ | TAC5301_PWR_DAC_PDZ | TAC5301_PWR_MICBIAS_PDZ);
	if (err) {
		return err;
	}

	/* MICBIAS_CFG (page 1) is already 0xA0 (7.5V) at reset -- left at
	 * default rather than rewritten, since no different bias voltage is
	 * needed yet. */

	/* Start from the DVOL unity code (0dB), not muted, consistent with
	 * boot flat/full-volume -- main.c calls set_mute()/set_volume_pct()
	 * to reach whatever state it actually wants before unmuting the
	 * user-audible path. */
	err = write_dac_dvol((uint8_t)current_dvol_code);
	if (err) {
		return err;
	}

	initialised = true;
	LOG_INF("TAC5301-Q1 up: loopback mixer active, 3 biquads/channel both sides, fs %u Hz",
		(unsigned int)TAC5301_RATE_HZ);
	return 0;
}

int tac5301_control_apply_filters(const struct filter_band *bands, size_t count)
{
	int err = 0;

	if (count > PROTOCOL_MAX_BANDS) {
		count = PROTOCOL_MAX_BANDS;
	}

	/* ADC chain gets the first up-to-3 bands, DAC chain gets the rest
	 * (up to 2 more, since PROTOCOL_MAX_BANDS is 5) -- cascading biquads
	 * is commutative regardless of which physical chain or order they
	 * run in (see tac5301_control_apply_filters()'s doc comment), so
	 * this split order is an implementation choice, not a correctness
	 * requirement.
	 */
	for (int slot = 0; slot < 3; slot++) {
		uint32_t words[TAC5301_BIQUAD_COEFF_COUNT];
		size_t band_idx = (size_t)slot;

		if (band_idx < count) {
			struct tac5301_biquad c;

			calc_band_coeffs(&bands[band_idx], &c);
			biquad_to_words(&c, words);
			LOG_INF("ADC slot %d: band f0=%.1f Hz Q=%.1f atten=%.1f dB", slot,
				(double)bands[band_idx].f0_hz, (double)bands[band_idx].q,
				(double)bands[band_idx].atten_db);
		} else {
			memcpy(words, unity_biquad_q31, sizeof(words));
		}
		if (!initialised) {
			continue; /* bench without a codec: math only */
		}
		int e = write_adc_slot(slot, words);

		if (e && !err) {
			err = e;
		}
	}

	for (int slot = 0; slot < 3; slot++) {
		uint32_t words[TAC5301_BIQUAD_COEFF_COUNT];
		size_t band_idx = (size_t)(slot + 3);

		if (band_idx < count) {
			struct tac5301_biquad c;

			calc_band_coeffs(&bands[band_idx], &c);
			biquad_to_words(&c, words);
			LOG_INF("DAC slot %d: band f0=%.1f Hz Q=%.1f atten=%.1f dB", slot,
				(double)bands[band_idx].f0_hz, (double)bands[band_idx].q,
				(double)bands[band_idx].atten_db);
		} else {
			memcpy(words, unity_biquad_q31, sizeof(words));
		}
		if (!initialised) {
			continue;
		}
		int e = write_dac_slot(slot, words);

		if (e && !err) {
			err = e;
		}
	}
	return err;
}

int tac5301_control_set_bypass(bool enabled)
{
	if (!enabled) {
		return 0; /* main.c re-applies current bands to end bypass. */
	}
	return tac5301_control_apply_filters(NULL, 0);
}

int tac5301_control_set_volume_pct(uint8_t volume_pct)
{
	if (volume_pct > 100) {
		volume_pct = 100;
	}
	/* 0% -> TAC5301_DVOL_MIN_CODE (-100dB), 100% -> TAC5301_DVOL_UNITY_CODE
	 * (0dB), log-linear between -- same shape as the ADAU1860 driver's
	 * volume_pct mapping, codec-specific numbers. */
	double frac = (double)volume_pct / 100.0;
	double db = -100.0 + frac * 100.0; /* linear in dB, 0% = -100dB .. 100% = 0dB */
	double code_f = (db + 100.0) * 2.0 + (double)TAC5301_DVOL_MIN_CODE; /* 0.5dB/step */

	int code = (int)lround(code_f);

	if (code < TAC5301_DVOL_MIN_CODE) {
		code = TAC5301_DVOL_MIN_CODE;
	}
	if (code > TAC5301_DVOL_UNITY_CODE) {
		code = TAC5301_DVOL_UNITY_CODE;
	}
	current_dvol_code = code;
	if (muted) {
		return 0; /* applied on unmute. */
	}
	return write_dac_dvol((uint8_t)code);
}

int tac5301_control_set_mute(bool enable_mute)
{
	muted = enable_mute;
	return write_dac_dvol(muted ? TAC5301_DVOL_MUTE_CODE : (uint8_t)current_dvol_code);
}
