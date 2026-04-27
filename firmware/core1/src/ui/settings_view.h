/* settings_view.h — LVGL panel for Core 1 settings UI (Stage 2).
 *
 * Lets the user browse and edit a curated subset of Meshtastic config keys
 * (settings_keys.c table) via the physical keypad. UP/DOWN moves the
 * cursor in the current group; LEFT/RIGHT cycles between groups; OK
 * enters edit mode on the highlighted key (or fires Apply on the trailing
 * "Apply" row); BACK leaves edit / clears pending.
 *
 * Talks to Core 0 through settings_client — sends GET on activation /
 * group switch, sends SET on Apply confirmation, sends COMMIT_CONFIG or
 * COMMIT_REBOOT depending on whether any edited key has needs_reboot=1
 * in the local settings_keys table.
 *
 * Stage 2 limitations:
 *   - SK_KIND_STR keys (owner names) show value but cannot be edited;
 *     edit overlay shows a "Stage 3 — IME wiring" placeholder.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "key_event.h"

void settings_view_init(lv_obj_t *panel);
void settings_view_apply(const key_event_t *ev);
void settings_view_refresh(void);
