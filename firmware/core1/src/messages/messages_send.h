/* messages_send.h — Push outbound text via the cascade PhoneAPI client.
 *
 * Builds a ToRadio.packet (TEXT_MESSAGE_APP) directly through the cascade
 * encoder and pushes the framed bytes onto the c1→c0 SERIAL_BYTES ring.
 * Returns true on success, false if the TX queue is full (the caller can
 * choose to retry on a later tick).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_MESSAGES_SEND_H
#define MOKYA_CORE1_MESSAGES_SEND_H

#include <stdbool.h>
#include <stdint.h>

#define MESSAGES_SEND_TEXT_MAX  200u
#define MESSAGES_SEND_BROADCAST 0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true on a successful TX-ring push. When true and
 * `out_packet_id != NULL`, *out_packet_id receives the locally-assigned,
 * non-zero MeshPacket.id so the caller can match the eventual
 * Routing-app ACK or QueueStatus back to this send. */
bool messages_send_text(uint32_t to_node_id,
                        uint8_t  channel_index,
                        bool     want_ack,
                        const uint8_t *text,
                        uint16_t text_len,
                        uint32_t *out_packet_id);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_MESSAGES_SEND_H */
