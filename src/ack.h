/* Device -> app acks over NUS TX.
 *
 * ble_transport_send() has existed since the transport was written for
 * exactly this purpose ("Optional device -> app notification (status/acks)
 * over NUS TX"), but nothing ever called it -- the app never subscribed
 * either. Both were roadmap items (docs/roadmap.md, "Next -- app").
 *
 * Kept deliberately tiny: which command landed, or that the line was
 * rejected -- not a structured error taxonomy. The app only needs enough
 * to show a quiet "applied"/"error" state, not a JSON dump (that's exactly
 * what the old TX monitor did and was removed for being too
 * engineering-facing).
 */
#ifndef HAVEN_ACK_H_
#define HAVEN_ACK_H_

#include "protocol.h"

/* Sends {"type":"ACK","cmd":"<name>"}\n over NUS TX. No-op (silently) if
 * there's no active connection -- ble_transport_send() already handles
 * that, same as every other outbound message on this link.
 */
void ack_send(enum dsp_command_type type);

/* Sends {"type":"ERROR"}\n over NUS TX, for a line that failed to parse. */
void ack_send_error(void);

#endif /* HAVEN_ACK_H_ */
