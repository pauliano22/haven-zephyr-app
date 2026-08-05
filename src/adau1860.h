/* ADAU1860 DSP control driver — SCAFFOLD.
 *
 * The nRF52 is control-port master to the ADAU1860:
 *   - I2C  : register access / parameter RAM safeload (default path)
 *   - SPI  : bulk program download (self-boot image, coefficient blocks)
 *
 * Everything here is a placeholder: the bus transactions are stubbed until
 * the production PCB arrives and the SigmaStudio+ program layout (parameter
 * RAM addresses for each biquad stage) is exported.
 */
#ifndef HAVEN_ADAU1860_H_
#define HAVEN_ADAU1860_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* One biquad stage in the form the DSP program consumes.
 * NOTE: coefficient number format (8.24 fixed vs 32-bit float) depends on the
 * ADAU1860 core config exported from SigmaStudio+ — TODO confirm and convert
 * in adau1860_apply_filters().
 */
struct adau1860_biquad {
	float b0, b1, b2, a1, a2;
};

/* Bring-up: verify device ID over I2C, release from reset, load DSP program.
 * Currently logs and returns 0 without touching hardware.
 */
int adau1860_init(void);

/* Compute biquad coefficients for each band and push them into the DSP's
 * parameter RAM (safeload). Bands with atten_db < PROTOCOL_ATTEN_MAX_DB get a
 * peaking-cut EQ of that depth; at the max they collapse to a pure notch.
 */
int adau1860_apply_filters(const struct filter_band *bands, size_t count);

/* True = audio passes through unprocessed. */
int adau1860_set_bypass(bool enabled);

#endif /* HAVEN_ADAU1860_H_ */
