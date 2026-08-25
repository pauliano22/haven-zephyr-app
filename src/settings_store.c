#include "settings_store.h"

#include <errno.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "gatt_audio_service.h"

LOG_MODULE_REGISTER(settings_store, LOG_LEVEL_INF);

static int haven_settings_set(const char *name, size_t len,
			      settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "volume", &next) && !next) {
		uint8_t volume;

		if (len != sizeof(volume)) {
			return -EINVAL;
		}
		if (read_cb(cb_arg, &volume, sizeof(volume)) < 0) {
			return -EIO;
		}
		gatt_audio_set_volume(volume);
		LOG_INF("Restored volume from flash: %u%%", volume);
		return 0;
	}

	if (settings_name_steq(name, "freq", &next) && !next) {
		struct audio_freq_range range;

		if (len != sizeof(range)) {
			return -EINVAL;
		}
		if (read_cb(cb_arg, &range, sizeof(range)) < 0) {
			return -EIO;
		}
		gatt_audio_set_freq_range(&range);
		LOG_INF("Restored freq range from flash: [%u, %u] Hz",
			range.lower_hz, range.upper_hz);
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(haven, SETTINGS_STORE_SUBTREE, NULL,
			       haven_settings_set, NULL, NULL);

int haven_settings_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings_subsys_init failed (err %d)", err);
		return err;
	}

	/* No-op if nothing was ever saved (first boot / after a flash
	 * erase) -- gatt_audio_service's compiled-in defaults stand.
	 */
	err = settings_load_subtree(SETTINGS_STORE_SUBTREE);
	if (err) {
		LOG_WRN("settings_load_subtree failed (err %d) -- using defaults", err);
	}

	return 0;
}
