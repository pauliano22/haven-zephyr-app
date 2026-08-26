/* Host-test fake for <zephyr/bluetooth/conn.h> -- gatt_audio_service.c only
 * ever passes `struct bt_conn *` around opaquely (ARG_UNUSED in every
 * callback here), never dereferences it, so an opaque incomplete type is
 * sufficient.
 */
#ifndef FAKE_ZEPHYR_BLUETOOTH_CONN_H_
#define FAKE_ZEPHYR_BLUETOOTH_CONN_H_

struct bt_conn;

#endif
