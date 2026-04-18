/* view_router.h — Core 1 view switcher.
 *
 * Owns the KeyEvent queue consumer and dispatches events to exactly one
 * active view at a time. FUNC key (MOKYA_KEY_FUNC, press edge) cycles
 * between registered views; all other events are forwarded to the
 * active view's _apply hook. Each view's _refresh is called every tick
 * regardless of visibility — cheap and keeps hidden snapshots current
 * so there's no flicker on switch.
 *
 * Views today:
 *   0. keypad_view (M3.3 Phase C — physical keypad grid)
 *   1. rf_debug_view (M3.4.5d Part C — Teseo-LIV3FL telemetry)
 *
 * Adding a new view: create `<name>_view_init(panel)`, `<name>_view_apply(ev)`,
 * `<name>_view_refresh(void)` matching the signatures below, then add a
 * case to view_router's internal dispatch table.
 *
 * Thread model (LV_USE_OS = LV_OS_NONE): all entry points MUST be called
 * from the lvgl_task context.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"

/* Create one hidden panel per view under `screen` and initialise every
 * view. After this call, view 0 is visible and receives events. */
void view_router_init(lv_obj_t *screen);

/* Drain any queued KeyEvents, handle FUNC-press as a view toggle,
 * forward other events to the active view, then call every view's
 * refresh hook. Call once per lvgl_task iteration after
 * lv_timer_handler(). */
void view_router_tick(void);
