/* hint_bar.h — G-2 conditional bottom hint strip per docs/ui/00-design-charter.md.
 *
 * 16 px tall overlay parented to the active screen at y=224. Shows
 * D-pad / OK / contextual hints for the current view. Hidden on Home,
 * Launcher, and throughout Settings.
 *
 * Each view sets its hints in its `create()` (or on focus change) via
 * `hint_bar_set()`. The router calls `hint_bar_clear()` between view
 * swaps so a stale hint never leaks across views.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_HINT_BAR_H
#define MOKYA_CORE1_HINT_BAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HINT_BAR_HEIGHT  16

void hint_bar_init(lv_obj_t *screen);

/* `left`, `ok`, `right` may be NULL or "" to skip a column.
 * Typical:  hint_bar_set("up/dn list", "OK enter", "BACK"); */
void hint_bar_set(const char *left, const char *ok, const char *right);
void hint_bar_clear(void);
void hint_bar_set_visible(bool visible);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  /* MOKYA_CORE1_HINT_BAR_H */
