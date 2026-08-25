/* ADAU1860 DSP control API — SCAFFOLD.
 *
 * The nRF5340 application core is control-port master to the ADAU1860:
 *   - I2C1 : register access / parameter RAM safeload (control plane)
 *   - I2S0 : multichannel audio data (see boards/nrf5340dk_nrf5340_cpuapp.overlay)
 *
 * Everything here is a placeholder: the bus transactions are stubbed until
 * the production PCB arrives and the SigmaStudio+ program layout (parameter
 * RAM addresses for each biquad stage) is exported.
 */
#ifndef HAVEN_ADAU1860_CONTROL_H_
#define HAVEN_ADAU1860_CONTROL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* One biquad stage in the form the DSP program consumes.
 * NOTE: coefficient number format (8.24 fixed vs 32-bit float) depends on the
 * ADAU1860 core config exported from SigmaStudio+ — TODO confirm and convert
 * in adau1860_control_apply_filters().
 */
struct adau1860_biquad {
	float b0, b1, b2, a1, a2;
};

/* Bring-up: verify device ID over I2C, release from reset, load DSP program.
 * Currently checks the I2C1 bus is ready and logs; no register transactions
 * yet.
 */
int adau1860_control_init(void);

/* Compute biquad coefficients for each band and push them into the DSP's
 * parameter RAM (safeload). Bands with atten_db < PROTOCOL_ATTEN_MAX_DB get a
 * peaking-cut EQ of that depth; at the max they collapse to a pure notch.
 */
int adau1860_control_apply_filters(const struct filter_band *bands, size_t count);

/* True = audio passes through unprocessed. */
int adau1860_control_set_bypass(bool enabled);

/* ── LDL calibration tone ─────────────────────────────────────────────────
 * Safety-critical -- see tone_safety.c, which owns validation/clamping and
 * the auto-stop watchdog. These are the raw hardware actions only; nothing
 * here enforces the level ceiling itself (that already happened by the
 * time a caller reaches these).
 */
int adau1860_control_set_tone(float f0_hz, float level_db);
int adau1860_control_set_tone_level(float level_db);
int adau1860_control_stop_tone(void);

/* ── BLE communication callbacks ─────────────────────────────────────────
 * Hooks for behavior that should react to the link itself, not a specific
 * parsed command — e.g. muting or holding last-known-good coefficients on
 * disconnect, once real hardware exists. Wired to the BLE connection
 * lifecycle in main.c via ble_transport_set_conn_callbacks(); currently
 * just logs.
 */
void adau1860_control_on_ble_connected(void);
void adau1860_control_on_ble_disconnected(void);

#endif /* HAVEN_ADAU1860_CONTROL_H_ */
