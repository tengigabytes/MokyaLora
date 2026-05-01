/* messages_tx_status.c — see messages_tx_status.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "messages_tx_status.h"
#include "dm_store.h"
#include "notification.h"

static messages_tx_status_t s_status;

void messages_tx_status_publish(uint8_t  result,
                                uint8_t  error_reason,
                                uint32_t packet_id)
{
    s_status.result       = result;
    s_status.error_reason = error_reason;
    s_status.packet_id    = packet_id;

    uint32_t next = s_status.change_seq + 1u;
    __atomic_store_n(&s_status.change_seq, next, __ATOMIC_RELEASE);

    /* Phase 3: mirror into dm_store so conversation_view bubbles can
     * paint sending → delivered / failed without a second observer. */
    dm_ack_state_t ack_state = DM_ACK_NONE;
    switch (result) {
        case MESSAGES_TX_RESULT_SENDING:   ack_state = DM_ACK_SENDING;   break;
        case MESSAGES_TX_RESULT_DELIVERED: ack_state = DM_ACK_DELIVERED; break;
        case MESSAGES_TX_RESULT_FAILED:    ack_state = DM_ACK_FAILED;    break;
    }
    if (ack_state != DM_ACK_NONE) {
        dm_store_update_ack(packet_id, ack_state);
    }
    /* Phase 8 — notify on terminal ACK transitions only (not the
     * SENDING phase, which is just our local "queued" trace). */
    if (result == MESSAGES_TX_RESULT_DELIVERED ||
        result == MESSAGES_TX_RESULT_FAILED) {
        notification_event(NOTIF_EVENT_ACK, 0xFFu, 0u);
    }
}

void messages_tx_status_get(messages_tx_status_t *out)
{
    if (!out) return;
    /* Read change_seq first with acquire so the body fields we read
     * afterwards are at least as fresh as that seq. */
    uint32_t cur = __atomic_load_n(&s_status.change_seq, __ATOMIC_ACQUIRE);
    *out = s_status;
    out->change_seq = cur;
}
