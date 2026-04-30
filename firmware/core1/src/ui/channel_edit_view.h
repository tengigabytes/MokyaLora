/* channel_edit_view.h — B-2 頻道編輯 (subset).
 *
 * Edits the channel index stashed by channels_view_set_active_index().
 * Three writable rows in v1:
 *   - Name           (IME modal, IPC_CFG_CHANNEL_NAME)
 *   - Position prec. (UP/DOWN ±1 with 0..32 clamp,
 *                     IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION)
 *   - Muted          (toggle, IPC_CFG_CHANNEL_MODULE_IS_MUTED)
 *
 * Each writable change fires settings_client SET + COMMIT_CONFIG (no
 * reboot) immediately — Core 0 reloadConfig(SEGMENT_CHANNELS) re-pushes
 * NodeInfo / ChannelComplete and the cascade replay updates
 * phoneapi_cache, so the row's own re-render picks up confirmation.
 *
 * Read-only rows (Role, PSK, channel ID) are surfaced for context but
 * not editable in v1 — role + PSK editing needs IPC keys we don't
 * have yet (admin packet flow).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *channel_edit_view_descriptor(void);

#ifdef __cplusplus
}
#endif
