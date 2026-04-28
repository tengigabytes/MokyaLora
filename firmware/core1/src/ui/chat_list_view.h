/* chat_list_view.h — A-1 DM chat list.
 *
 * Lists peers from `dm_store` sorted by recency. OK enters A-2
 * (`conversation_view`) for the focused peer; BACK returns to caller
 * (typically L-0 home via launcher modal).
 *
 * Selected peer is stashed via `chat_list_set_active_peer` so
 * conversation_view can pick it up after navigation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_CHAT_LIST_VIEW_H
#define MOKYA_CORE1_CHAT_LIST_VIEW_H

#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *chat_list_view_descriptor(void);

/* Cross-view stash so conversation_view knows which peer to render.
 * Set by chat_list when OK fires; read once by conversation_view's
 * create() (ignored if 0). */
void     chat_list_set_active_peer(uint32_t peer_node_id);
uint32_t chat_list_get_active_peer(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_CHAT_LIST_VIEW_H */
