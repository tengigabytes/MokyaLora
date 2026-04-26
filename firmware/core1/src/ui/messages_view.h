/* messages_view.h — Show the most recent IPC_MSG_RX_TEXT (M5 Phase 1).
 *
 * Two labels: a header line with the sender node ID + channel index, and
 * a wrapped body label with the message text. Refresh polls the
 * messages_inbox singleton; when its sequence number bumps, the labels
 * are updated.
 *
 * Thread model: call all entry points from lvgl_task only.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"
#include "key_event.h"

void messages_view_init(lv_obj_t *panel);
void messages_view_apply(const key_event_t *ev);
void messages_view_refresh(void);
