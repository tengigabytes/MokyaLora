/* template_toggle.h — leaf template C (Phase 4 spec §50).
 *
 * Used by settings_app_view in EDIT_TOGGLE mode. Caller passes:
 *   - the key being edited (for label / IPC code)
 *   - the current bool value (false = left, true = right)
 *   - LVGL parent panel (settings_app's own panel area)
 *
 * This module owns the rendered widgets and the in-progress edit
 * value. settings_app_view forwards key events to apply_key() and
 * polls is_done() / committed_value() / committed() to act on
 * exit. Push-style callback removed — the host view already owns
 * the FreeRTOS event flow.
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

/** Build the toggle widgets onto `parent`. `current` is the value as
 *  read from cache / IPC. `key` provides label + IPC code for the
 *  on-apply IPC SET. */
void template_toggle_open(lv_obj_t *parent,
                          const settings_key_def_t *key,
                          bool current);

/** Tear down the widgets created by template_toggle_open. */
void template_toggle_close(void);

/** Forward a key event. Returns true if the template consumed the
 *  event and the host should NOT process it further. */
bool template_toggle_apply_key(const key_event_t *ev);

/** True after OK or BACK has been pressed; host should call
 *  template_toggle_close() and re-enter BROWSE mode. */
bool template_toggle_done(void);

/** True iff exit was via OK (commit) — false iff BACK (cancel). */
bool template_toggle_committed(void);

/** Edited value at exit (only meaningful when committed() == true). */
bool template_toggle_value(void);

#ifdef __cplusplus
}
#endif
