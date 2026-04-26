/* messages_send.h — Push outbound text onto the c1_to_c0 CMD ring.
 *
 * Builds an IpcPayloadText and ipc_ring_pushes it as IPC_CMD_SEND_TEXT.
 * Core 0's mokya_handle_ipc_command() consumes it and hands it off to
 * MeshService::sendToMesh(). Returns true on success, false if the ring
 * is full (the caller can choose to retry on a later tick).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_MESSAGES_SEND_H
#define MOKYA_CORE1_MESSAGES_SEND_H

#include <stdbool.h>
#include <stdint.h>

#define MESSAGES_SEND_TEXT_MAX  200u  /* must be <= IPC_MSG_PAYLOAD_MAX - sizeof(IpcPayloadText hdr) */
#define MESSAGES_SEND_BROADCAST 0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

bool messages_send_text(uint32_t to_node_id,
                        uint8_t  channel_index,
                        bool     want_ack,
                        const uint8_t *text,
                        uint16_t text_len);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_MESSAGES_SEND_H */
