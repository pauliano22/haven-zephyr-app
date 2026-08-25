/* NVS-backed persistence (Zephyr Settings subsystem) for the Haven Audio
 * Control Service's Volume/FreqRange characteristics. gatt_audio_service.c
 * saves on every validated write; this module restores on boot.
 */
#ifndef HAVEN_SETTINGS_STORE_H_
#define HAVEN_SETTINGS_STORE_H_

#define SETTINGS_STORE_SUBTREE     "haven"
#define SETTINGS_STORE_VOLUME_KEY  SETTINGS_STORE_SUBTREE "/volume"
#define SETTINGS_STORE_FREQ_KEY    SETTINGS_STORE_SUBTREE "/freq"

/* Initializes the settings subsystem and restores any previously-saved
 * volume/freq range (via gatt_audio_set_volume()/gatt_audio_set_freq_range()).
 * Call once at boot, AFTER gatt_audio_service_init() and after registering
 * any gatt_audio_service_set_callbacks() consumers (e.g. the mock audio
 * pipeline) that should see the restored values applied, not just the
 * compiled-in defaults.
 */
int haven_settings_init(void);

#endif /* HAVEN_SETTINGS_STORE_H_ */
