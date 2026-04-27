/* ime_view.h — LVGL panel rendering the MIE IME state.
 *
 * Read-only view over ime_task's snapshot API (ime_view_lock /
 * ime_view_pending / ime_view_page_* / ime_view_commit_text, see
 * ime_task.h). Refresh is gated on the atomic g_ime_dirty_counter —
 * only repaint when the IME task signals a mutation, so the LVGL task
 * does no work while the user is idle.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "view_router.h"

const view_descriptor_t *ime_view_descriptor(void);
