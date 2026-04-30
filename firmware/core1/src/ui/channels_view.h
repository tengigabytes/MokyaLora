/* channels_view.h — B-1 頻道列表.
 *
 * 8 channels (idx 0..7) read out of phoneapi_cache_get_channel; each
 * row shows index / role / name / encryption / muted flag / position
 * precision.  UP/DOWN moves the cursor, OK hands the focused index
 * to B-2 (channel_edit_view) via channels_view_set_active_index().
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "view_router.h"

#ifdef __cplusplus
extern "C" {
#endif

const view_descriptor_t *channels_view_descriptor(void);

/* Cross-view stash for B-1 → B-2 hand-off. 0xFF = unset. */
void    channels_view_set_active_index(uint8_t idx);
uint8_t channels_view_get_active_index(void);

#ifdef __cplusplus
}
#endif
