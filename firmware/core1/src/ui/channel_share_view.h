/* channel_share_view.h — B-4 分享頻道 URL display.
 *
 * Triggered from channel_edit_view by SET key. Shows:
 *   - header "B-4 分享 ch%u  %s"
 *   - URL text (wrapped) — `https://meshtastic.org/e/#<base64>`
 *   - hint "BACK 返回"
 *
 * v1: text only. v2 (Phase 5b) adds qrcodegen QR rendering above the
 * URL text.
 *
 * Target channel index is communicated via channels_view's existing
 * `set_active_index` API (shared with channel_edit_view + add_view).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *channel_share_view_descriptor(void);

#ifdef __cplusplus
}
#endif
