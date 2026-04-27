/* messages_view.h — incoming text-message viewer + reply target.
 *
 * UP/DOWN navigates the inbox; OK sends the IME-committed text as a
 * DM reply to the currently-displayed sender. Footer shows TX status
 * for the most recent send (sending / delivered / failed).
 *
 * Scroll position + sticky-to-newest are preserved across destroy/
 * recreate via the view-owned state struct in messages_view.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

const view_descriptor_t *messages_view_descriptor(void);
