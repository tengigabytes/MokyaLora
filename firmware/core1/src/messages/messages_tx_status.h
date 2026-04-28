/* messages_tx_status.h — Latest outbound-message ack status.
 *
 * Holds whatever the cascade ack handler last said about the most
 * recent send: which packet id it was for, what status (sending /
 * delivered / failed), and the underlying meshtastic_Routing_Error if
 * it failed. messages_view's footer renders this.
 *
 * Single producer (phoneapi_session ack handler / messages_send seed),
 * single consumer (lvgl_task via messages_view_refresh). Atomic seq
 * gate, no mutex.
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
    uint8_t  result;           ///< MESSAGES_TX_RESULT_*
    uint8_t  error_reason;     ///< meshtastic_Routing_Error (0 on success)
    uint32_t packet_id;        ///< Locally-assigned MeshPacket.id (non-zero)
} messages_tx_status_t;

void messages_tx_status_publish(uint8_t  result,
                                uint8_t  error_reason,
                                uint32_t packet_id);

/* Returns the latest snapshot. The change_seq field lets the caller
 * skip work when nothing has changed since its last sample. */
void messages_tx_status_get(messages_tx_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_MESSAGES_TX_STATUS_H */
