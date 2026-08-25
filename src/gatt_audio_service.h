/* Haven Audio Control Service — custom BLE GATT service for the nRF5340 DK
 * prototyping bench (bring-up milestone, ahead of the real DSP board).
 *
 * Two characteristics, both read/write/notify:
 *   Volume       — 1 byte,  uint8 percent, 0-100
 *   FreqRange    — 4 bytes, two little-endian uint16 (lower_hz, upper_hz)
 *
 * Writes are validated and rejected outright (BT_ATT_ERR_*) rather than
 * clamped — unlike the JSON MULTI_FILTER path in protocol.c, which silently
 * clamps. That's deliberate here: a GATT client should find out immediately
 * that its value didn't stick, rather than read back something it didn't
 * send.
 */
#ifndef HAVEN_GATT_AUDIO_SERVICE_H_
#define HAVEN_GATT_AUDIO_SERVICE_H_

#include <stdint.h>

#define AUDIO_VOLUME_MIN_PCT 0
#define AUDIO_VOLUME_MAX_PCT 100

/* Same cutoff-frequency guardrails as the JSON MULTI_FILTER protocol
 * (protocol.h's PROTOCOL_F0_MIN_HZ/MAX_HZ) so both control paths agree on
 * what's a physically sane band edge for this hardware.
 */
#define AUDIO_FREQ_MIN_HZ 200
#define AUDIO_FREQ_MAX_HZ 8000

struct audio_freq_range {
	uint16_t lower_hz;
	uint16_t upper_hz;
} __packed;

/* Fired after a write is validated and applied; NULL is a valid "not
 * interested" argument to either callback.
 */
typedef void (*audio_volume_changed_cb_t)(uint8_t volume_pct);
typedef void (*audio_freq_range_changed_cb_t)(const struct audio_freq_range *range);

/* Register consumers of validated parameter changes (e.g. the mock audio
 * pipeline). Call any time before or after gatt_audio_service_init() — it
 * only matters relative to the first BLE write.
 */
void gatt_audio_service_set_callbacks(audio_volume_changed_cb_t on_volume,
				       audio_freq_range_changed_cb_t on_freq_range);

/* Registers the GATT service. Call once at boot. */
int gatt_audio_service_init(void);

#endif /* HAVEN_GATT_AUDIO_SERVICE_H_ */
