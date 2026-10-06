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
#include "tone_gen.h"

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

/* Hardware output ceiling -- see tac5301_control_set_output_ceiling_db()'s
 * doc comment in the header for the real architectural difference from
 * the ADAU1860 driver's equivalent (a genuinely separate, BLE-unreachable
 * register there; a software-enforced minimum against the single shared
 * DVOL register here, since this chip has no second gain stage).
 */
#ifdef CONFIG_HAVEN_TAC5301_OUTPUT_CEILING_DB
#define TAC5301_OUTPUT_CEILING_DB_DEFAULT CONFIG_HAVEN_TAC5301_OUTPUT_CEILING_DB
#else
#define TAC5301_OUTPUT_CEILING_DB_DEFAULT 0
#endif
static int output_ceiling_db = TAC5301_OUTPUT_CEILING_DB_DEFAULT;

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

/* CORRECTED (see fix/tac5301-biquad-coefficient-encoding): the previous
 * version of this function assumed the unnegated textbook form with no
 * scaling, which was wrong on 3 of 5 coefficients. TI's own application
 * note (SLAAEH6, "TAC5x1x and TAC5x1x-Q1 Programmable Biquad Filters -
 * Configuration and Applications", section 3, Equation 3 and the worked
 * MATLAB-conversion procedure in section 3.2) gives this exactly:
 *
 *   H(z) = (N0 + 2*N1*z^-1 + N2*z^-2) / (2 - 2*D1*z^-1 + D2*z^-2)
 *
 * and the conversion from standard RBJ [b0,b1,b2,a1,a2] (a0 normalized to
 * 1, matching this driver's calc_band_coeffs output):
 *   N0 = b0
 *   N1 = b1 / 2   -- NOT b1 directly
 *   N2 = b2
 *   D1 = -a1 / 2  -- negated AND halved
 *   D2 = -a2      -- negated (not halved)
 * then each converted to Q1.31. Getting this wrong would not have errored
 * -- it would have produced a filter with roughly the right shape but a
 * silently wrong center frequency/Q/depth, straight into a device worn in
 * someone's ear. Caught by reading TI's own app note before any hardware
 * use, not by a bench measurement -- another reason the bench experiment
 * (TAC5301_BENCH_EXPERIMENT.md) still matters even after this fix.
 */
static void biquad_to_words(const struct tac5301_biquad *c, uint32_t out[TAC5301_BIQUAD_COEFF_COUNT])
{
	out[0] = q31_encode(c->b0);
	out[1] = q31_encode(c->b1 / 2.0);
	out[2] = q31_encode(c->b2);
	out[3] = q31_encode(-c->a1 / 2.0);
	out[4] = q31_encode(-c->a2);
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

/* The DVOL registers above are this chip's ONLY mute mechanism (unlike the
 * ADAU1860's separate DAC_CTRL2 mute bit, independent of its volume
 * register) -- so a tone-route switch's own "mute for the switch, unmute
 * after" step and a genuine user mute share one register. Writing this
 * instead of unconditionally un-muting means a user-muted device stays
 * silent through a tone start/stop rather than the route switch itself
 * briefly revealing the tone. */
static int write_dac_dvol_effective(void)
{
	return write_dac_dvol(muted ? TAC5301_DVOL_MUTE_CODE : (uint8_t)current_dvol_code);
}

/* ── LDL calibration tone ─────────────────────────────────────────────────
 * Mirrors adau1860_control.c's tone-path role (the tone is synthesised on
 * the nRF5340 by tone_gen.c -- codec-agnostic, reused as-is -- and arrives
 * over I2S0 into this chip's ASI; this file's job is only getting that
 * signal to the DAC and back out again cleanly) but not its exact
 * mechanism, for two real reasons specific to this chip:
 *
 * 1. Routing: the ADAU1860 has one DAC input mux (DAC_ROUTE0) that picks
 *    exactly one source. The TAC5301-Q1 has independent mixer ENABLE bits
 *    (MIXER_CFG0) instead -- EN_LOOPBACK_MIXER and EN_DAC_ASI_MIXER are
 *    separate bits that would ADD together if both were left on, not
 *    switch exclusively like a mux. Engaging the tone route here
 *    explicitly turns the loopback bit OFF while turning the ASI bit ON,
 *    and disengage does the reverse -- getting this backwards (e.g. only
 *    setting the ASI bit without clearing loopback) would mix the tone
 *    with live ambient sound, defeating the point of a calibration tone
 *    at a known, uncontaminated level.
 * 2. No ASRC-lock wait: the ADAU1860 driver polls STATUS2 for input-ASRC
 *    lock before unmuting into the tone, because its PLL/clock domain is
 *    something the driver explicitly brings up and can observe locking.
 *    This driver's init already assumes BCLK/FSYNC are running
 *    continuously before any I2C traffic happens at all (per
 *    TAC5301_BENCH_EXPERIMENT.md's bring-up procedure), and no datasheet
 *    register surfaced an equivalent "ASI data valid" status bit to poll
 *    -- so there's no wait here. This is an assumption carried over from
 *    the bench procedure's own assumption, not a verified absence; if a
 *    real device shows an audible glitch at tone start, this is the first
 *    place to look, not something already ruled out.
 */
static K_MUTEX_DEFINE(tone_route_lock);
static bool tone_route_engaged;

#ifdef CONFIG_HAVEN_TONE_FULL_SCALE_DB
#define TAC5301_TONE_FULL_SCALE_DB ((float)CONFIG_HAVEN_TONE_FULL_SCALE_DB)
#else
#define TAC5301_TONE_FULL_SCALE_DB 85.0f
#endif

/* Same mapping as adau1860_tone_gain_q15() -- codec-agnostic math, the
 * nominal full-scale constant is a board/calibration property, not a
 * per-codec one, so it's deliberately the same Kconfig symbol. */
int32_t tac5301_tone_gain_q15(float level_db)
{
	float rel_db = level_db - TAC5301_TONE_FULL_SCALE_DB;

	if (rel_db >= 0.0f) {
		return TONE_GEN_GAIN_ONE;
	}
	if (rel_db < -96.0f) {
		return 0;
	}
	return (int32_t)lrint(pow(10.0, (double)rel_db / 20.0) * (double)TONE_GEN_GAIN_ONE);
}

static int tone_route_engage(void)
{
	int err = 0;

	if (!initialised) {
		return 0;
	}
	k_mutex_lock(&tone_route_lock, K_FOREVER);
	if (!tone_route_engaged) {
		err = write_dac_dvol(TAC5301_DVOL_MUTE_CODE);
		if (!err) {
			err = reg_write8(TAC5301_REG_MIXER_CFG0, TAC5301_MIXER_EN_DAC_ASI_MIXER);
		}
		if (!err) {
			err = write_dac_dvol_effective();
		}
		tone_route_engaged = true;
	}
	k_mutex_unlock(&tone_route_lock);
	return err;
}

static int tone_route_disengage(void)
{
	int err = 0;

	if (!initialised) {
		return 0;
	}
	k_mutex_lock(&tone_route_lock, K_FOREVER);
	if (tone_route_engaged) {
		err = write_dac_dvol(TAC5301_DVOL_MUTE_CODE);
		if (!err) {
			err = reg_write8(TAC5301_REG_MIXER_CFG0, TAC5301_MIXER_EN_LOOPBACK_MIXER);
		}
		if (!err) {
			err = write_dac_dvol_effective();
		}
		tone_route_engaged = false;
	}
	k_mutex_unlock(&tone_route_lock);
	return err;
}

/* Runs on the tone_gen feeder thread once the I2S link has actually gone
 * quiet (ramp-down played out, peripheral stopped) -- registered in
 * tac5301_control_init(), same hook adau1860_control.c uses. */
static void on_tone_stopped(void)
{
	int err = tone_route_disengage();

	if (err) {
		LOG_ERR("Restoring hear-through route after tone failed: %d", err);
	} else {
		LOG_INF("Tone finished -- hear-through route restored");
	}
}

/* ── Public API ──────────────────────────────────────────────────────── */

int tac5301_control_init(void)
{
	int err;

	tone_gen_set_stopped_callback(on_tone_stopped);

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

/* Datasheet SLASFD9A §6.3.7.1.5 (ADC) and §6.3.7.2 (DAC): "the host device
 * must write these coefficient values before powering up any ADC channels
 * for recording or DAC channels for playback. In two channel use case, the
 * TAC5301-Q1 also supports on the fly programmable filters" (two banks +
 * a switch bit). Haven runs the mono, single-bank configuration, so a live
 * rewrite is outside the documented behaviour -- and even where the silicon
 * tolerates it, six biquads x five 32-bit words land one register at a
 * time, so the filter passes through dozens of half-updated coefficient
 * sets (unstable ones included) while audio is flowing. There is no
 * safeload here (that is the ADAU1860's feature). The documented-correct
 * sequence is therefore: mute the DAC, power down ADC+DAC (MICBIAS stays
 * up so the electret bias does not have to re-settle), write every slot,
 * power ADC+DAC back up, restore the volume. This costs an audible dropout
 * per update; that is a property of the part, not of this driver, and is
 * one of the reasons the research track is a research track.
 */
static int coeff_update_begin(void)
{
	int err = write_dac_dvol(TAC5301_DVOL_MUTE_CODE);

	if (err) {
		return err;
	}
	return reg_write8(TAC5301_REG_PWR_CFG, TAC5301_PWR_MICBIAS_PDZ);
}

static int coeff_update_end(void)
{
	int err = reg_write8(TAC5301_REG_PWR_CFG,
			     TAC5301_PWR_ADC_PDZ | TAC5301_PWR_DAC_PDZ | TAC5301_PWR_MICBIAS_PDZ);

	if (err) {
		return err;
	}
	return write_dac_dvol_effective();
}

int tac5301_control_apply_filters(const struct filter_band *bands, size_t count)
{
	int err = 0;

	if (count > PROTOCOL_MAX_BANDS) {
		count = PROTOCOL_MAX_BANDS;
	}

	if (initialised) {
		err = coeff_update_begin();
		if (err) {
			return err;
		}
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

	if (initialised) {
		/* Always bring the chains back up, even after a failed slot write:
		 * a silent codec is a worse failure than a stale band. */
		int e = coeff_update_end();

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

/* dB (DVOL's native range, -100..+27) -> DVOL code (1..255), 0.5dB/step,
 * per datasheet 7.1.1.73/.75's field description. Shared by
 * set_volume_pct and the output-ceiling functions so both convert the
 * same way -- rounding/clamping once, not two slightly-different
 * formulas that could disagree at an edge value.
 */
static int dvol_code_from_db(double db)
{
	double code_f = (db + 100.0) * 2.0 + (double)TAC5301_DVOL_MIN_CODE;
	int code = (int)lround(code_f);

	if (code < TAC5301_DVOL_MIN_CODE) {
		code = TAC5301_DVOL_MIN_CODE;
	}
	if (code > TAC5301_DVOL_MAX_CODE) {
		code = TAC5301_DVOL_MAX_CODE;
	}
	return code;
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
	int code = dvol_code_from_db(db);

	/* Enforce the ceiling here, at the single point this driver has for
	 * it -- see the header comment on why this chip needs a software
	 * clamp instead of a genuinely separate hardware stage. */
	int ceiling_code = dvol_code_from_db((double)output_ceiling_db);

	if (code > ceiling_code) {
		code = ceiling_code;
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

int tac5301_control_set_output_ceiling_db(int ceiling_db)
{
	if (ceiling_db > 27 || ceiling_db < -100) {
		return -EINVAL;
	}
	output_ceiling_db = ceiling_db;

	int ceiling_code = dvol_code_from_db((double)ceiling_db);

	/* Re-clamp whatever's currently active, same as the ADAU1860 driver's
	 * equivalent taking effect immediately rather than waiting for the
	 * next explicit set_volume_pct call -- except there, lowering the
	 * ceiling can't be bypassed by anything else touching the volume,
	 * because it's a separate register; here, current_dvol_code IS the
	 * single value both paths share, so clamping it here is what makes
	 * the two behave the same way from the caller's point of view. */
	if (current_dvol_code > ceiling_code) {
		current_dvol_code = ceiling_code;
	}
	if (muted || !initialised) {
		return 0;
	}
	return write_dac_dvol((uint8_t)current_dvol_code);
}

int tac5301_control_get_output_ceiling_db(void)
{
	return output_ceiling_db;
}

int tac5301_control_set_tone(float f0_hz, float level_db)
{
	/* Caller (tone_safety.c) has already clamped level_db to
	 * [PROTOCOL_TONE_LEVEL_MIN_DB, PROTOCOL_TONE_LEVEL_MAX_DB]. */
	int32_t gain = tac5301_tone_gain_q15(level_db);

	LOG_INF("Tone: f0=%.1f Hz level=%.1f dB -> gain %ld/32768 (DAC ASI mixer, loopback paused)",
		(double)f0_hz, (double)level_db, (long)gain);

	/* I2S first, same ordering rationale as the ADAU1860 driver (give the
	 * codec a clock/data stream before switching the mixer onto it) even
	 * though this chip's own ASI doesn't need an explicit lock wait. */
	int err = tone_gen_start(f0_hz, gain);

	if (err) {
		LOG_ERR("Tone generator start failed: %d", err);
		return err;
	}
	return tone_route_engage();
}

int tac5301_control_set_tone_level(float level_db)
{
	int32_t gain = tac5301_tone_gain_q15(level_db);

	LOG_INF("Tone level: %.1f dB -> gain %ld/32768", (double)level_db, (long)gain);
	return tone_gen_set_gain(gain);
}

int tac5301_control_stop_tone(void)
{
	LOG_INF("Tone stop");
	int err = tone_gen_stop();

	if (err) {
		/* No generator (bench without I2S, or init failed): nothing is
		 * playing, but make sure the codec isn't left routed to the
		 * ASI mixer. */
		return tone_route_disengage();
	}
	/* Route restore follows from the feeder thread (on_tone_stopped) once
	 * the ramp-down has actually played. */
	return 0;
}

void tac5301_control_on_ble_connected(void)
{
	LOG_INF("BLE connected -- filters unchanged");
}

void tac5301_control_on_ble_disconnected(void)
{
	/* Filters keep running -- hearing protection must not depend on the
	 * phone. The tone must not either: main.c's tone_safety_stop() already
	 * stops the generator; this is the second layer, restoring the codec
	 * route synchronously (under soft mute) even if the feeder thread's
	 * callback is late or never comes. Idempotent. */
	int err = tone_route_disengage();

	if (err) {
		LOG_ERR("Route restore on BLE disconnect failed: %d", err);
	}
	LOG_INF("BLE disconnected -- filters kept running, tone route restored");
}
