/* BLE peripheral transport: advertises as "Haven", exposes the
 * Nordic UART Service, reassembles newline-terminated lines from incoming
 * writes, and hands complete lines to the registered callback.
 */
#ifndef HAVEN_BLE_TRANSPORT_H_
#define HAVEN_BLE_TRANSPORT_H_

#include <stddef.h>

/* Called from the system workqueue with a NUL-terminated line ('\n' removed). */
typedef void (*ble_line_cb_t)(const char *line);

/* Fired on BLE connect/disconnect, from the Bluetooth connection callback
 * context (not the system workqueue) — keep handlers short and non-blocking.
 */
typedef void (*ble_conn_event_cb_t)(void);

/* Optional: register link lifecycle callbacks. Call before ble_transport_init(). */
void ble_transport_set_conn_callbacks(ble_conn_event_cb_t on_connected,
				       ble_conn_event_cb_t on_disconnected);

int ble_transport_init(ble_line_cb_t line_cb);

/* Optional device -> app notification (status/acks) over NUS TX. */
int ble_transport_send(const char *data, size_t len);

/* ── Power management ─────────────────────────────────────────────────────
 * After BLE_IDLE_ADV_TIMEOUT_MS (see ble_transport.c) of no connection,
 * advertising drops from a fast/responsive interval to a much slower one
 * to save power -- still connectable the whole time, just polled less
 * often. Call this to force an immediate return to fast advertising (e.g.
 * from a button press) without waiting for a reconnect attempt; also
 * resets the idle timer, same as if a fresh disconnect just happened.
 * No-op if already on fast advertising or currently connected.
 */
void ble_transport_wake_fast_advertising(void);

#endif /* HAVEN_BLE_TRANSPORT_H_ */
