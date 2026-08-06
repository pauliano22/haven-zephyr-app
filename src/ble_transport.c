#include "ble_transport.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/nus.h>

LOG_MODULE_REGISTER(ble_transport, LOG_LEVEL_INF);

/* Matches MAX_BLE_BUF on the Teensy prototype; a 5-band MULTI_FILTER payload
 * is ~220 bytes, so 512 leaves generous headroom.
 */
#define LINE_BUF_SIZE 512

static ble_line_cb_t line_handler;
static ble_conn_event_cb_t connected_handler;
static ble_conn_event_cb_t disconnected_handler;
static struct bt_conn *current_conn;

static char line_buf[LINE_BUF_SIZE];
static size_t line_len;
static bool line_overflow;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/* ── Line assembly ──────────────────────────────────────────────────────────
 * NUS writes can arrive fragmented; accumulate until '\n'. On overflow the
 * rest of the line is dropped and the (truncated) buffer will simply fail
 * JSON parsing — same graceful-degradation contract as the Teensy firmware.
 */
static void feed_bytes(const uint8_t *data, uint16_t len)
{
	for (uint16_t i = 0; i < len; i++) {
		char c = (char)data[i];

		if (c == '\n') {
			if (!line_overflow && line_len > 0) {
				line_buf[line_len] = '\0';
				line_handler(line_buf);
			} else if (line_overflow) {
				LOG_WRN("Dropped oversized line (> %d bytes)",
					LINE_BUF_SIZE);
			}
			line_len = 0;
			line_overflow = false;
		} else if (line_len < LINE_BUF_SIZE - 1) {
			line_buf[line_len++] = c;
		} else {
			line_overflow = true;
		}
	}
}

static void nus_received(struct bt_conn *conn, const uint8_t *data,
			 uint16_t len)
{
	ARG_UNUSED(conn);
	feed_bytes(data, len);
}

static struct bt_nus_cb nus_callbacks = {
	.received = nus_received,
};

/* ── Connection lifecycle ──────────────────────────────────────────────────*/

static void start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd,
				  ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
	} else {
		LOG_INF("Advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	line_len = 0;
	line_overflow = false;
	LOG_INF("Phone connected");
	if (connected_handler) {
		connected_handler();
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Phone disconnected (reason %u)", reason);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	if (disconnected_handler) {
		disconnected_handler();
	}
	/* The app auto-reconnects; be discoverable again immediately. */
	start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ── Public API ─────────────────────────────────────────────────────────────*/

void ble_transport_set_conn_callbacks(ble_conn_event_cb_t on_connected,
				       ble_conn_event_cb_t on_disconnected)
{
	connected_handler = on_connected;
	disconnected_handler = on_disconnected;
}

int ble_transport_init(ble_line_cb_t line_cb)
{
	int err;

	line_handler = line_cb;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (err %d)", err);
		return err;
	}

	err = bt_nus_init(&nus_callbacks);
	if (err) {
		LOG_ERR("bt_nus_init failed (err %d)", err);
		return err;
	}

	start_advertising();
	return 0;
}

int ble_transport_send(const char *data, size_t len)
{
	if (current_conn == NULL) {
		return -ENOTCONN;
	}
	return bt_nus_send(current_conn, (const uint8_t *)data, (uint16_t)len);
}
