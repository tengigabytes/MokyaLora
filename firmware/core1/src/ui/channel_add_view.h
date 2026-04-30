/* channel_add_view.h — B-3 加入頻道 (manual create new).
 *
 * Triggered from channels_view when OK lands on an empty slot. v1 sets
 * name + role + auto-generated 32-byte random PSK in one
 * AdminMessage.set_channel encode. Custom-PSK input is v2.
 *
 * Target slot index is communicated via channels_view_set_active_index
 * (existing API; shared with channel_edit_view).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *channel_add_view_descriptor(void);

#ifdef __cplusplus
}
#endif
