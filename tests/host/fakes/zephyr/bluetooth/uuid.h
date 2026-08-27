/* Host-test fake for <zephyr/bluetooth/uuid.h>. gatt_audio_service.c only
 * uses these to build compile-time UUID constants for the service/
 * characteristic declarations -- the host tests never inspect the actual
 * UUID bytes, so a structurally-valid but simplified encoding is enough to
 * let the file compile and the service-definition macros produce *some*
 * const data.
 */
#ifndef FAKE_ZEPHYR_BLUETOOTH_UUID_H_
#define FAKE_ZEPHYR_BLUETOOTH_UUID_H_

#include <stdint.h>

struct bt_uuid {
	uint8_t type;
};

struct bt_uuid_128 {
	struct bt_uuid uuid;
	uint8_t val[16];
};

#define BT_UUID_TYPE_128 128

#define BT_UUID_128_ENCODE(w32, w1, w2, w3, w48) \
	{ w32, w1, w2, w3, w48 } /* not byte-accurate; identity/order is unused by the tests */

#define BT_UUID_DECLARE_128(...) ((const struct bt_uuid *)0)

#endif
