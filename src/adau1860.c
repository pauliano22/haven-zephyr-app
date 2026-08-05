#include "adau1860.h"

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(adau1860, LOG_LEVEL_INF);

#define ADAU1860_SAMPLE_RATE_HZ 48000.0f

/* ── Bus placeholders ───────────────────────────────────────────────────────
 * TODO(hw-bringup): bind to the devicetree nodes (adi,adau1860 on i2c0 and
 * adi,adau1860-spi on spi1) and implement the real control-port transactions,
 * including the safeload handshake for glitch-free coefficient swaps.
 */

static int adau1860_i2c_write(uint16_t reg, const uint8_t *data, size_t len)
{
	LOG_DBG("I2C write [placeholder]: reg 0x%04x, %u bytes", reg,
		(unsigned int)len);
	return 0;
}

static int adau1860_spi_burst_write(uint32_t addr, const uint8_t *data,
				    size_t len)
{
	LOG_DBG("SPI burst [placeholder]: addr 0x%08x, %u bytes", addr,
		(unsigned int)len);
	return 0;
}

/* ── Coefficient math ───────────────────────────────────────────────────────
 * Same RBJ cookbook math validated on the Teensy prototype and in
 * tinnitus_dsp/test_filter.py, extended with a variable-depth peaking cut:
 * at PROTOCOL_ATTEN_MAX_DB the band degenerates to the classic notch.
 */
static void calc_band_coeffs(const struct filter_band *band,
			     struct adau1860_biquad *c)
{
	float w0 = 2.0f * (float)M_PI * (band->f0_hz / ADAU1860_SAMPLE_RATE_HZ);
	float alpha = sinf(w0) / (2.0f * band->q);
	float cosw0 = cosf(w0);

	if (band->atten_db >= PROTOCOL_ATTEN_MAX_DB) {
		/* Pure notch */
		float a0 = 1.0f + alpha;

		c->b0 = 1.0f / a0;
		c->b1 = -2.0f * cosw0 / a0;
		c->b2 = 1.0f / a0;
		c->a1 = -2.0f * cosw0 / a0;
		c->a2 = (1.0f - alpha) / a0;
	} else {
		/* Peaking EQ cut of atten_db decibels */
		float A = powf(10.0f, -band->atten_db / 40.0f);
		float a0 = 1.0f + alpha / A;

		c->b0 = (1.0f + alpha * A) / a0;
		c->b1 = -2.0f * cosw0 / a0;
		c->b2 = (1.0f - alpha * A) / a0;
		c->a1 = -2.0f * cosw0 / a0;
		c->a2 = (1.0f - alpha / A) / a0;
	}
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

int adau1860_init(void)
{
	/* TODO(hw-bringup): read CHIP_ID register, verify, release HIBERNATE,
	 * SPI-download the SigmaStudio+ program image, start the DSP core.
	 */
	LOG_INF("ADAU1860 init [placeholder] — no hardware transactions yet");
	return 0;
}

int adau1860_apply_filters(const struct filter_band *bands, size_t count)
{
	if (count > PROTOCOL_MAX_BANDS) {
		count = PROTOCOL_MAX_BANDS;
	}

	for (size_t i = 0; i < count; i++) {
		struct adau1860_biquad c;

		calc_band_coeffs(&bands[i], &c);

		LOG_INF("Band %u: f0=%.1f Hz Q=%.1f atten=%.1f dB", (unsigned int)i,
			(double)bands[i].f0_hz, (double)bands[i].q,
			(double)bands[i].atten_db);

		/* TODO(hw-bringup): convert c to the DSP's coefficient format
		 * and safeload into the parameter RAM slot for stage i:
		 *   adau1860_i2c_write(PARAM_RAM_STAGE(i), coeff_bytes, len);
		 */
		ARG_UNUSED(c);
	}

	/* TODO(hw-bringup): zero the coefficients of the (MAX_BANDS - count)
	 * unused cascade stages so stale filters don't linger.
	 */
	(void)adau1860_i2c_write;
	(void)adau1860_spi_burst_write;
	return 0;
}

int adau1860_set_bypass(bool enabled)
{
	/* TODO(hw-bringup): flip the DSP program's bypass mux register. */
	LOG_INF("Bypass [placeholder]: %s", enabled ? "ENABLED" : "disabled");
	return 0;
}
