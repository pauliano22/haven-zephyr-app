#include "adau1860_control.h"

#include <math.h>
#include <string.h>

/* picolibc's math.h only exposes M_PI under a feature-test macro that
 * -std=c17 doesn't define. */
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

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

/* ── Generic SigmaDSP register I/O ────────────────────────────────────────
 * The ADAU1860 is part of ADI's SigmaDSP family; its own datasheet doesn't
 * publish a general register map (confirmed against the datasheet, the
 * product FAQ, and EngineerZone community reports -- Analog Devices expects
 * register-level programming to come from a SigmaStudio+-exported firmware
 * blob, not hand-picked addresses). What *is* standardized across the whole
 * SigmaDSP family, and confirmed against both the Linux kernel's generic
 * `sigmadsp` codec driver and a real, widely-used Arduino reference
 * implementation (MCUdude/SigmaDSP): 16-bit register addresses, sent
 * big-endian (MSByte first) immediately before the data payload, on both
 * plain register writes and multi-byte "safeload" writes. This layer
 * implements exactly that framing and nothing chip-specific -- callers still
 * need real addresses, which come from the SigmaStudio+ export once it
 * exists (see adau1860_control_init()'s TODO below).
 */
int adau1860_i2c_write_reg(uint16_t addr, const uint8_t *data, size_t len)
{
	uint8_t addr_buf[2] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF) };
	/* Two messages, only the last carrying I2C_MSG_STOP, so the controller
	 * keeps the bus held between them -- this is one contiguous I2C
	 * transaction (address bytes then data, no repeated START in between),
	 * not two separate ones. Avoids copying arbitrarily large payloads
	 * (a whole program-memory WRITEXBYTES chunk, for the blob loader) into
	 * a fixed-size stack buffer first.
	 */
	struct i2c_msg msgs[2] = {
		{
			.buf = addr_buf,
			.len = sizeof(addr_buf),
			.flags = I2C_MSG_WRITE,
		},
		{
			.buf = (uint8_t *)data,
			.len = len,
			.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
		},
	};
	int err = i2c_transfer_dt(&adau1860_i2c, msgs, ARRAY_SIZE(msgs));

	if (err) {
		LOG_DBG("I2C write failed (err %d), reg 0x%04x, %u bytes", err, addr,
			(unsigned int)len);
	}
	return err;
}

int adau1860_i2c_read_reg(uint16_t addr, uint8_t *data, size_t len)
{
	uint8_t addr_buf[2] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF) };
	int err = i2c_write_read_dt(&adau1860_i2c, addr_buf, sizeof(addr_buf), data, len);

	if (err) {
		LOG_DBG("I2C read failed (err %d), reg 0x%04x, %u bytes", err, addr,
			(unsigned int)len);
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

	/* TODO(hw-bringup): once a SigmaStudio+ project for this board exists
	 * and is exported, this is where its firmware blob gets loaded --
	 * release HIBERNATE, stream the program over I2C (or SPI, depending
	 * on how the export is configured), start the DSP core. There is no
	 * public ADAU1860 register map to hand-write this against (checked:
	 * the datasheet's own I2C section doesn't include one, consistent
	 * with EngineerZone reports for this chip) -- adau1860_i2c_write_reg()
	 * / _read_reg() above implement the generic SigmaDSP-family wire
	 * framing (confirmed against the Linux kernel's `sigmadsp` codec
	 * driver and a real Arduino SigmaDSP library), ready for whatever
	 * real register addresses the export actually contains.
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
		 *   encode_param_ram_write(&c, frame);
		 *   adau1860_i2c_write_reg(PARAM_RAM_STAGE_ADDR(i), frame, sizeof(frame));
		 * PARAM_RAM_STAGE_ADDR() doesn't exist yet -- it's a real address
		 * from the SigmaStudio+ export, not something to guess at.
		 */
		ARG_UNUSED(c);
	}

	/* TODO(hw-bringup): zero the coefficients of the (MAX_BANDS - count)
	 * unused cascade stages so stale filters don't linger.
	 */
	return 0;
}

int adau1860_control_set_bypass(bool enabled)
{
	/* TODO(hw-bringup): flip the DSP program's bypass mux register. */
	LOG_INF("Bypass [placeholder]: %s", enabled ? "ENABLED" : "disabled");
	return 0;
}

int adau1860_control_set_tone(float f0_hz, float level_db)
{
	/* TODO(hw-bringup): program the DSP's tone generator (frequency +
	 * output gain register) once the SigmaStudio+ program layout for it
	 * is known. Caller (tone_safety.c) has already clamped level_db to
	 * PROTOCOL_TONE_LEVEL_MAX_DB.
	 */
	LOG_INF("Tone [placeholder]: f0=%.1f Hz level=%.1f dB", (double)f0_hz,
		(double)level_db);
	return 0;
}

int adau1860_control_set_tone_level(float level_db)
{
	/* TODO(hw-bringup): update just the tone generator's output gain
	 * register, leaving frequency untouched.
	 */
	LOG_INF("Tone level [placeholder]: %.1f dB", (double)level_db);
	return 0;
}

int adau1860_control_stop_tone(void)
{
	/* TODO(hw-bringup): mute the tone generator specifically, without
	 * otherwise disturbing the main filter/bypass signal path.
	 */
	LOG_INF("Tone stop [placeholder]");
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
