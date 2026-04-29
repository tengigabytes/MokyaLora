/* template_number.h — leaf template B (Phase 4 spec §50).
 *
 * Used by settings_app_view for SK_KIND_U8 / SK_KIND_I8 / SK_KIND_U32
 * keys (TX power, hop limit, broadcast intervals — ~25 keys). The
 * key's `min`/`max` bound the editable range.
 *
 * D-pad mapping per spec:
 *   UP   = +1   (small step)
 *   DOWN = -1
 *   RIGHT= +10  (large step)
 *   LEFT = -10
 *   OK   = apply, BACK = cancel.
 *
 * Caller passes the current i32 value; on apply the new i32 is
 * available via template_number_value(). settings_app_view packs it
 * back to the right wire-width (1, 4 bytes) before sending IPC_CFG_SET.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "key_event.h"
#include "settings_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

void   template_number_open(lv_obj_t *parent,
                            const settings_key_def_t *key,
                            int32_t current);
void   template_number_close(void);
bool   template_number_apply_key(const key_event_t *ev);
bool   template_number_done(void);
bool   template_number_committed(void);
int32_t template_number_value(void);

#ifdef __cplusplus
}
#endif
