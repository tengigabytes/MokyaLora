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
 *   2. font_test_view (M3.6 — MIEF Traditional Chinese smoke test)
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

#ifdef __cplusplus
extern "C" {
#endif

/* Create one hidden panel per view under `screen` and initialise every
 * view. After this call, view 0 is visible and receives events. */
void view_router_init(lv_obj_t *screen);

/* Drain any queued KeyEvents, handle FUNC-press as a view toggle,
 * forward other events to the active view, then call every view's
 * refresh hook. Call once per lvgl_task iteration after
 * lv_timer_handler(). */
void view_router_tick(void);

/* ── Modal view borrow (Stage 3 IME string edit) ─────────────────────── *
 *
 * Lets one view temporarily hand control to another (e.g.
 * settings_view → ime_view to type a string), with a callback that
 * fires when the user is done. While modal, FUNC press no longer
 * cycles views; instead it commits the modal and returns to the
 * caller. Other keys are forwarded to the borrowed view normally.
 *
 * Re-entry is rejected (only one modal at a time). The caller view's
 * panel becomes hidden while the borrowed view runs; refresh hooks
 * still fire on every view so state stays current.
 */
typedef void (*view_router_modal_done_t)(bool committed, void *ctx);

void view_router_modal_enter(int target_view,
                             view_router_modal_done_t on_done,
                             void *ctx);
bool view_router_in_modal(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
