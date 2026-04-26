/* ime_view.h — LVGL panel rendering the MIE IME state.
 *
 * Read-only view over ime_task's snapshot API (ime_view_lock /
 * ime_view_pending / ime_view_page_* / ime_view_commit_text, see
 * ime_task.h). Refresh is gated on the atomic g_ime_dirty_counter —
 * only repaint when the IME task signals a mutation, so the LVGL task
 * does no work while the user is idle.
 *
 * Layout (landscape 320×240):
 *   y   0 .. 25   pending composition    (16 px MIEF, left-aligned)
 *   y  26 .. 27   divider
 *   y  28 .. 61   mode tag + candidate row (flex-wrap, page N/M right)
 *   y  62 .. 63   divider
 *   y  64 .. 239  commit buffer (LV_LABEL_LONG_WRAP, auto-scroll tail)
 *
 * Candidate pagination follows feedback_candidate_pagination: render
 * as many page_cand() entries as the engine exposes and let LVGL flex-
 * wrap them — no hard-coded per-page count in this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "key_event.h"

void ime_view_init(lv_obj_t *panel);
void ime_view_apply(const key_event_t *ev);
void ime_view_refresh(void);
