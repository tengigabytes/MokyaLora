/* keypad_view.h — LVGL diagnostic view for the keypad driver (M3.3 Phase C).
 *
 * Displays a 6×6 grid mirroring the physical matrix layout
 * (keymap_matrix.h). Cells are grey when released and green when pressed.
 * A header label shows the most recent key + press/release; a footer label
 * shows cumulative pushed / dropped counters (primary consumer-path
 * sanity-check: dropped must stay at 0 in steady state).
 *
 * Thread model (LV_USE_OS = LV_OS_NONE):
 *   Both keypad_view_init() and keypad_view_tick() MUST be called from the
 *   lvgl_task context. keypad_view_tick() is non-blocking — it drains the
 *   KeyEvent queue with a 0-tick timeout and updates widgets inline.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"

#include "key_event.h"

/* Create the grid + labels under `parent` (usually a router-owned panel).
 * Must be called after lv_init() and a display has been attached. */
void keypad_view_init(lv_obj_t *parent);

/* Feed a single KeyEvent into the view — highlight the cell, update
 * footer counters. Caller owns the queue; keypad_view no longer drains
 * it on its own. */
void keypad_view_apply(const key_event_t *ev);
