/* Host-test fake for <zephyr/bluetooth/gatt.h>.
 *
 * Goal: let gatt_audio_service.c compile and run UNMODIFIED under host gcc
 * so the tests exercise the real write_volume()/write_freq_range()
 * validation logic, not a reimplementation of it. This is NOT a faithful
 * reproduction of Zephyr's real GATT attribute table layout -- attribute
 * indices/content are only as real as needed for the file to compile and
 * for read_volume()/read_freq_range()/bt_gatt_notify() to not crash if
 * exercised. The BT_ATT_ERR_* numeric values below are placeholders for
 * type/return-code purposes only -- NOT sourced from Zephyr's actual
 * att.h, since this project has no local Zephyr headers checked in to
 * copy them from and guessing exact spec values would be fabricating a
 * "real" number that isn't. Tests assert against these same symbolic
 * constants, never against a hardcoded magic number, so this is safe
 * either way.
 */
#ifndef FAKE_ZEPHYR_BLUETOOTH_GATT_H_
#define FAKE_ZEPHYR_BLUETOOTH_GATT_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t on a POSIX host */

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

#define BT_ATT_ERR_INVALID_OFFSET        0x07
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 0x0d
#define BT_ATT_ERR_OUT_OF_RANGE          0xff

#define BT_GATT_ERR(_att_err) (-(_att_err))

#define BT_GATT_CHRC_READ   0x02
#define BT_GATT_CHRC_WRITE  0x08
#define BT_GATT_CHRC_NOTIFY 0x10

#define BT_GATT_PERM_READ  0x01
#define BT_GATT_PERM_WRITE 0x02

#define BT_GATT_CCC_NOTIFY 0x0001

struct bt_gatt_attr {
	const struct bt_uuid *uuid;
	uint16_t perm;
	void *user_data;
};

struct bt_gatt_service_static {
	const struct bt_gatt_attr *attrs;
	size_t attr_count;
};

typedef ssize_t (*bt_gatt_attr_read_func_t)(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					     void *buf, uint16_t len, uint16_t offset);
typedef ssize_t (*bt_gatt_attr_write_func_t)(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					      const void *buf, uint16_t len, uint16_t offset,
					      uint8_t flags);
typedef void (*bt_gatt_ccc_cfg_changed_func_t)(const struct bt_gatt_attr *attr, uint16_t value);

/* Real semantics (copy len-or-remaining bytes from value at offset) so
 * read_volume()/read_freq_range() behave sensibly if a test does exercise
 * them, even though they aren't this pass's primary target.
 */
static inline ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
					 void *buf, uint16_t len, uint16_t offset,
					 const void *value, uint16_t value_len)
{
	(void)conn;
	(void)attr;
	if (offset > value_len) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	uint16_t remaining = value_len - offset;
	uint16_t n = remaining < len ? remaining : len;

	if (n > 0) {
		const uint8_t *src = (const uint8_t *)value + offset;
		uint8_t *dst = (uint8_t *)buf;

		for (uint16_t i = 0; i < n; i++) {
			dst[i] = src[i];
		}
	}
	return n;
}

static inline int bt_gatt_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  const void *data, uint16_t len)
{
	(void)conn;
	(void)attr;
	(void)data;
	(void)len;
	return 0;
}

/* Each "macro" below produces one struct bt_gatt_attr compound literal;
 * BT_GATT_CHARACTERISTIC in real Zephyr produces two (decl + value) but
 * since this fake's bt_gatt_attr has no field for the read/write function
 * pointers to preserve indexing-by-type distinctions that don't matter
 * here, one representative attr per macro call is enough for the file to
 * compile and for haven_audio_svc.attrs[2]/[5] to be in-bounds accesses
 * (dead code in these tests -- only reached if volume_notify_enabled /
 * freq_range_notify_enabled is true, which no test sets).
 */
#define BT_GATT_PRIMARY_SERVICE(_uuid) \
	{ .uuid = (_uuid), .perm = 0, .user_data = NULL }

#define BT_GATT_CHARACTERISTIC(_uuid, _props, _perm, _read, _write, _value) \
	{ .uuid = (_uuid), .perm = (_perm), .user_data = NULL }, \
	{ .uuid = (_uuid), .perm = (_perm), .user_data = (_value) }

#define BT_GATT_CCC(_changed, _perm) \
	{ .uuid = NULL, .perm = (_perm), .user_data = NULL }

#define BT_GATT_SERVICE_DEFINE(_name, ...) \
	static const struct bt_gatt_attr _name##_attrs[] = { __VA_ARGS__ }; \
	const struct bt_gatt_service_static _name = { \
		.attrs = _name##_attrs, \
		.attr_count = sizeof(_name##_attrs) / sizeof(_name##_attrs[0]), \
	}

#endif /* FAKE_ZEPHYR_BLUETOOTH_GATT_H_ */
