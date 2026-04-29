/* template_enum.h — leaf template A (Phase 4 spec §50).
 *
 * Used by settings_app_view in EDIT_ENUM mode for SK_KIND_ENUM_U8
 * keys (Region / Modem Preset / Role / etc). The key's
 * `enum_values[]` provides the option strings; `enum_count` bounds
 * the selection.
 *
 * Same lifecycle pattern as template_toggle:
 *   open()        - build widgets onto parent panel
 *   apply_key()   - forward key event, returns true if consumed
 *   done()        - poll: set after OK or BACK
 *   committed()   - true iff exit was via OK
 *   value()       - selected enum index at exit (u8 0..enum_count-1)
 *   close()       - destroy widgets
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

void  template_enum_open(lv_obj_t *parent,
                         const settings_key_def_t *key,
                         uint8_t current);
void  template_enum_close(void);
bool  template_enum_apply_key(const key_event_t *ev);
bool  template_enum_done(void);
bool  template_enum_committed(void);
uint8_t template_enum_value(void);

#ifdef __cplusplus
}
#endif
