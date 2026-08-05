/* BLE peripheral transport: advertises as "AcousticShield", exposes the
 * Nordic UART Service, reassembles newline-terminated lines from incoming
 * writes, and hands complete lines to the registered callback.
 */
#ifndef ACOUSTICSHIELD_BLE_TRANSPORT_H_
#define ACOUSTICSHIELD_BLE_TRANSPORT_H_

#include <stddef.h>

/* Called from the system workqueue with a NUL-terminated line ('\n' removed). */
typedef void (*ble_line_cb_t)(const char *line);

int ble_transport_init(ble_line_cb_t line_cb);

/* Optional device -> app notification (status/acks) over NUS TX. */
int ble_transport_send(const char *data, size_t len);

#endif /* ACOUSTICSHIELD_BLE_TRANSPORT_H_ */
