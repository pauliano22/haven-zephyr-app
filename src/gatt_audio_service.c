#include "gatt_audio_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>

#include "settings_store.h"

LOG_MODULE_REGISTER(gatt_audio_service, LOG_LEVEL_INF);

/* Haven Audio Control Service — vendor-specific 128-bit UUIDs, not
 * associated with any Bluetooth SIG-assigned service. Generated once for
 * this project; keep stable so paired/bonded phones don't need to
 * rediscover on every firmware update.
 *
 * Service:      7a1e0001-4b5c-4e8a-9c1a-2f6b8d3c9a10
 * Volume Char:  7a1e0002-4b5c-4e8a-9c1a-2f6b8d3c9a10
 * FreqRange Char: 7a1e0003-4b5c-4e8a-9c1a-2f6b8d3c9a10
 */
#define BT_UUID_HAVEN_AUDIO_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x7a1e0001, 0x4b5c, 0x4e8a, 0x9c1a, 0x2f6b8d3c9a10)
#define BT_UUID_HAVEN_AUDIO_VOLUME_VAL \
	BT_UUID_128_ENCODE(0x7a1e0002, 0x4b5c, 0x4e8a, 0x9c1a, 0x2f6b8d3c9a10)
#define BT_UUID_HAVEN_AUDIO_FREQ_RANGE_VAL \
	BT_UUID_128_ENCODE(0x7a1e0003, 0x4b5c, 0x4e8a, 0x9c1a, 0x2f6b8d3c9a10)

#define BT_UUID_HAVEN_AUDIO_SERVICE    BT_UUID_DECLARE_128(BT_UUID_HAVEN_AUDIO_SERVICE_VAL)
#define BT_UUID_HAVEN_AUDIO_VOLUME     BT_UUID_DECLARE_128(BT_UUID_HAVEN_AUDIO_VOLUME_VAL)
#define BT_UUID_HAVEN_AUDIO_FREQ_RANGE BT_UUID_DECLARE_128(BT_UUID_HAVEN_AUDIO_FREQ_RANGE_VAL)

/* Forward declaration: the write callbacks below notify through this
 * service's attribute table, but BT_GATT_SERVICE_DEFINE (which defines it)
 * comes after them so it can reference those same callbacks.
 */
extern const struct bt_gatt_service_static haven_audio_svc;

/* Bench defaults: unity volume, full guardrail range (no filtering). */
static uint8_t current_volume_pct = 100;
static struct audio_freq_range current_freq_range = {
	.lower_hz = AUDIO_FREQ_MIN_HZ,
	.upper_hz = AUDIO_FREQ_MAX_HZ,
};

static bool volume_notify_enabled;
static bool freq_range_notify_enabled;

static audio_volume_changed_cb_t volume_changed_cb;
static audio_freq_range_changed_cb_t freq_range_changed_cb;

void gatt_audio_service_set_callbacks(audio_volume_changed_cb_t on_volume,
				       audio_freq_range_changed_cb_t on_freq_range)
{
	volume_changed_cb = on_volume;
	freq_range_changed_cb = on_freq_range;
}

/* ── Volume characteristic ───────────────────────────────────────────────*/

static ssize_t read_volume(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &current_volume_pct,
				 sizeof(current_volume_pct));
}

/* Shared by write_volume() (post-validation) and gatt_audio_set_volume()
 * (trusted callers, e.g. settings restore) -- everything except the ATT
 * validation and the flash write.
 */
static void apply_volume(uint8_t value)
{
	current_volume_pct = value;
	LOG_INF("Volume set: %u%%", current_volume_pct);

	if (volume_notify_enabled) {
		/* attrs[2] = this characteristic's value attribute — see the
		 * BT_GATT_SERVICE_DEFINE layout below (decl, value, CCC).
		 */
		bt_gatt_notify(NULL, &haven_audio_svc.attrs[2], &current_volume_pct,
			      sizeof(current_volume_pct));
	}
	if (volume_changed_cb) {
		volume_changed_cb(current_volume_pct);
	}
}

void gatt_audio_set_volume(uint8_t volume_pct)
{
	apply_volume(volume_pct);
}

static ssize_t write_volume(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != sizeof(current_volume_pct)) {
		LOG_WRN("Volume write: bad length %u (want %u)", len,
			(unsigned int)sizeof(current_volume_pct));
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	uint8_t requested = *(const uint8_t *)buf;

	if (requested > AUDIO_VOLUME_MAX_PCT) {
		LOG_WRN("Volume write rejected: %u%% > max %u%%", requested,
			AUDIO_VOLUME_MAX_PCT);
		return BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE);
	}

	apply_volume(requested);

	int rc = settings_save_one(SETTINGS_STORE_VOLUME_KEY, &current_volume_pct,
				   sizeof(current_volume_pct));
	if (rc) {
		LOG_WRN("Failed to persist volume (err %d) -- applied, not saved", rc);
	}

	return len;
}

static void volume_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	volume_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* ── Frequency-range characteristic ──────────────────────────────────────*/

static ssize_t read_freq_range(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	uint8_t wire[4];

	sys_put_le16(current_freq_range.lower_hz, &wire[0]);
	sys_put_le16(current_freq_range.upper_hz, &wire[2]);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, sizeof(wire));
}

/* Shared by write_freq_range() (post-validation) and
 * gatt_audio_set_freq_range() (trusted callers, e.g. settings restore).
 */
static void apply_freq_range(const struct audio_freq_range *range)
{
	current_freq_range = *range;
	LOG_INF("FreqRange set: [%u, %u] Hz", range->lower_hz, range->upper_hz);

	if (freq_range_notify_enabled) {
		uint8_t wire[4];

		sys_put_le16(range->lower_hz, &wire[0]);
		sys_put_le16(range->upper_hz, &wire[2]);
		/* attrs[5] = this characteristic's value attribute. */
		bt_gatt_notify(NULL, &haven_audio_svc.attrs[5], wire, sizeof(wire));
	}
	if (freq_range_changed_cb) {
		freq_range_changed_cb(&current_freq_range);
	}
}

void gatt_audio_set_freq_range(const struct audio_freq_range *range)
{
	apply_freq_range(range);
}

static ssize_t write_freq_range(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);

	if (offset != 0) {
		LOG_WRN("FreqRange write: nonzero offset %u", offset);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 4) {
		LOG_WRN("FreqRange write: bad length %u (want 4)", len);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	const uint8_t *wire = buf;
	uint16_t lower_hz = sys_get_le16(&wire[0]);
	uint16_t upper_hz = sys_get_le16(&wire[2]);

	if (lower_hz < AUDIO_FREQ_MIN_HZ || upper_hz > AUDIO_FREQ_MAX_HZ ||
	    lower_hz >= upper_hz) {
		LOG_WRN("FreqRange write rejected: [%u, %u] Hz (bounds [%u, %u], "
			"lower < upper required)", lower_hz, upper_hz,
			AUDIO_FREQ_MIN_HZ, AUDIO_FREQ_MAX_HZ);
		return BT_GATT_ERR(BT_ATT_ERR_OUT_OF_RANGE);
	}

	struct audio_freq_range range = { .lower_hz = lower_hz, .upper_hz = upper_hz };

	apply_freq_range(&range);

	int rc = settings_save_one(SETTINGS_STORE_FREQ_KEY, &current_freq_range,
				   sizeof(current_freq_range));
	if (rc) {
		LOG_WRN("Failed to persist freq range (err %d) -- applied, not saved", rc);
	}

	return len;
}

static void freq_range_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	freq_range_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* ── Service declaration ─────────────────────────────────────────────────
 * Attribute indices (relied on above for bt_gatt_notify targets):
 *   0: primary service
 *   1: volume characteristic declaration      2: volume value
 *   3: volume CCC
 *   4: freq-range characteristic declaration  5: freq-range value
 *   6: freq-range CCC
 */
BT_GATT_SERVICE_DEFINE(haven_audio_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HAVEN_AUDIO_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_HAVEN_AUDIO_VOLUME,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_volume, write_volume, NULL),
	BT_GATT_CCC(volume_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_HAVEN_AUDIO_FREQ_RANGE,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_freq_range, write_freq_range, NULL),
	BT_GATT_CCC(freq_range_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int gatt_audio_service_init(void)
{
	LOG_INF("Haven Audio Control Service registered (volume=%u%%, freq=[%u,%u] Hz)",
		current_volume_pct, current_freq_range.lower_hz,
		current_freq_range.upper_hz);
	return 0;
}
