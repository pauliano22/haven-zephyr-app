#include "adau1860_control.h"

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>

LOG_MODULE_REGISTER(adau1860_control, LOG_LEVEL_INF);

#define ADAU1860_SAMPLE_RATE_HZ 48000.0f

/* ── Bus binding ─────────────────────────────────────────────────────────
 * Resolved from the "adau1860" child node declared under &i2c1 in
 * boards/nrf5340dk_nrf5340_cpuapp.overlay. I2C_DT_SPEC_GET works without a
 * dedicated Zephyr driver binding for the ADAU1860 itself — it only needs a
 * `reg` property and a bus parent with a real driver (nordic,nrf-twim).
 */
static const struct i2c_dt_spec adau1860_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(adau1860));

/* TODO(hw-bringup): implement the safeload handshake for glitch-free
 * coefficient swaps once the register map is known. This currently performs
 * a real I2C write of whatever bytes are handed to it, with no framing.
 */
static int adau1860_i2c_write(const uint8_t *data, size_t len)
{
	int err = i2c_write_dt(&adau1860_i2c, data, len);

	if (err) {
		LOG_DBG("I2C write failed (err %d), %u bytes", err, (unsigned int)len);
	}
	return err;
}

/* TODO(hw-bringup): the audio data path (I2S0, see the overlay) still needs
 * a driver-level init once the ADAU1860's required PCM format (sample width,
 * frame clock polarity, channel count) is confirmed from the datasheet /
 * SigmaStudio+ export. Not wired yet — control plane comes first.
 */

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

int adau1860_control_init(void)
{
	if (!device_is_ready(adau1860_i2c.bus)) {
		LOG_ERR("I2C1 bus not ready");
		return -ENODEV;
	}

	/* TODO(hw-bringup): read CHIP_ID register, verify, release HIBERNATE,
	 * SPI-download or I2C-stream the SigmaStudio+ program image, start
	 * the DSP core.
	 */
	LOG_INF("ADAU1860 control init: I2C1 bus ready, addr 0x%02x [no register "
		"transactions yet]", adau1860_i2c.addr);
	return 0;
}

int adau1860_control_apply_filters(const struct filter_band *bands, size_t count)
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
		 * and safeload into the parameter RAM slot for stage i, e.g.:
		 *   uint8_t frame[PARAM_FRAME_LEN];
		 *   encode_param_ram_write(PARAM_RAM_STAGE(i), &c, frame);
		 *   adau1860_i2c_write(frame, sizeof(frame));
		 */
		ARG_UNUSED(c);
	}

	/* TODO(hw-bringup): zero the coefficients of the (MAX_BANDS - count)
	 * unused cascade stages so stale filters don't linger.
	 */
	(void)adau1860_i2c_write;
	return 0;
}

int adau1860_control_set_bypass(bool enabled)
{
	/* TODO(hw-bringup): flip the DSP program's bypass mux register. */
	LOG_INF("Bypass [placeholder]: %s", enabled ? "ENABLED" : "disabled");
	return 0;
}

void adau1860_control_on_ble_connected(void)
{
	LOG_INF("BLE connected [placeholder] — no DSP state change yet");
}

void adau1860_control_on_ble_disconnected(void)
{
	LOG_INF("BLE disconnected [placeholder] — no DSP state change yet");
}
