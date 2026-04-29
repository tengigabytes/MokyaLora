/* canned_view.h — A-4 canned (preset) message picker (modal).
 *
 * Entered as a modal from a chat context (typically conversation_view
 * via the LEFT key). The caller sets a target peer with
 * `canned_view_set_target_peer(peer)` BEFORE `view_router_modal_enter`,
 * then OK on the picker fires `messages_send_text(peer, ...)` and
 * commits the modal. BACK cancels.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_UI_CANNED_VIEW_H
#define MOKYA_CORE1_UI_CANNED_VIEW_H

#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set the destination peer for the next OK commit. Pass 0 to suppress
 * sending (the picker still opens but OK becomes a no-op + cancel). */
void canned_view_set_target_peer(uint32_t peer_node_id);

const view_descriptor_t *canned_view_descriptor(void);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_UI_CANNED_VIEW_H */
