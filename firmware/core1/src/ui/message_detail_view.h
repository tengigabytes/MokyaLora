/* message_detail_view.h — A3 modal showing radio metadata for one DM.
 *
 * Reached via FUNC long-press in conversation_view; the caller stashes
 * the target peer + message via message_detail_view_set_target() and
 * calls view_router_modal_enter_overlay(VIEW_ID_MESSAGE_DETAIL).
 * BACK closes the modal.
 *
 * v1 always targets the most-recent message in the active chat (per
 * user decision — no per-message focus cursor in conversation_view).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_UI_MESSAGE_DETAIL_VIEW_H
#define MOKYA_CORE1_UI_MESSAGE_DETAIL_VIEW_H

#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *message_detail_view_descriptor(void);

/* Set the target message before opening the modal. `peer_node_id` keys
 * dm_store; `msg_offset` is 0..count-1 with 0 = oldest (matches the
 * existing dm_store_get_msg ordering). The render reads the message
 * fresh on every refresh so ack updates land while the modal is open. */
void message_detail_view_set_target(uint32_t peer_node_id, uint8_t msg_offset);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_UI_MESSAGE_DETAIL_VIEW_H */
