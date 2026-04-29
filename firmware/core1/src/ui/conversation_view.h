/* conversation_view.h — A-2 DM thread + compose.
 *
 * Reads the active peer from `chat_list_get_active_peer()` on enter,
 * renders the recent N messages, and lets OK trigger an IME modal to
 * compose a reply (Mode B for now — Mode A inline geometry deferred).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_CONVERSATION_VIEW_H
#define MOKYA_CORE1_CONVERSATION_VIEW_H

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *conversation_view_descriptor(void);

/* A3: invoked by view_router when FUNC long-press fires while
 * VIEW_ID_MESSAGES_CHAT is active and IME is not in compose. Opens
 * the A-3 message detail modal targeting the most-recent message. */
void conversation_view_open_msg_detail(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_CONVERSATION_VIEW_H */
