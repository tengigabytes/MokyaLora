/* messages_tx_status.h — Last outbound-message status reported by Core 0.
 *
 * Holds whatever the most recent IPC_MSG_TX_ACK said: which IPC seq it
 * was for, what status (sending / delivered / failed), and the
 * underlying meshtastic_Routing_Error if it failed. messages_view's
 * footer renders this.
 *
 * Single producer (bridge_task IPC dispatcher), single consumer
 * (lvgl_task via messages_view_refresh). Atomic seq gate, no mutex —
 * same SPSC pattern as messages_inbox.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_MESSAGES_TX_STATUS_H
#define MOKYA_CORE1_MESSAGES_TX_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#define MESSAGES_TX_RESULT_SENDING    0u
#define MESSAGES_TX_RESULT_DELIVERED  1u
#define MESSAGES_TX_RESULT_FAILED     2u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t change_seq;       ///< Bumps every time anything below changes
    uint8_t  ipc_seq;          ///< IPC seq the original CMD_SEND_TEXT carried
    uint8_t  result;           ///< MESSAGES_TX_RESULT_*
    uint8_t  error_reason;     ///< meshtastic_Routing_Error (0 on success)
    uint32_t packet_id;        ///< Meshtastic packet id (32 bit), 0 if unknown
} messages_tx_status_t;

void messages_tx_status_publish(uint8_t  ipc_seq,
                                uint8_t  result,
                                uint8_t  error_reason,
                                uint32_t packet_id);

/* Returns the latest snapshot. The change_seq field lets the caller
 * skip work when nothing has changed since its last sample. */
void messages_tx_status_get(messages_tx_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_MESSAGES_TX_STATUS_H */
