/* rf_debug_view.h — LVGL view for Teseo-LIV3FL RF / signal diagnostics.
 *
 * Displays the driver-maintained teseo_rf_state_t + teseo_state_t
 * snapshots:
 *   - header: online / fix status
 *   - noise floor (GPS + GLONASS raw estimates)
 *   - CPU usage + clock
 *   - Adaptive Notch Filter status per path (freq, lock, mode, jammer)
 *   - per-satellite C/N0 table (top ~8 by C/N0)
 *
 * The underlying $PSTM* sentences are not enabled by default — call
 * teseo_enable_rf_debug_messages(true) once to flip them on in NVM.
 * If they're not enabled the view shows zero counters and a hint.
 *
 * Thread model (LV_USE_OS = LV_OS_NONE):
 *   Both rf_debug_view_init() and rf_debug_view_tick() MUST be called
 *   from the lvgl_task context.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"

#include "key_event.h"

/* Build all widgets under `parent` (usually a router-owned panel). Must
 * be called after lv_init() and the display driver is attached. */
void rf_debug_view_init(lv_obj_t *parent);

/* Refresh label text from the latest driver snapshot. Call periodically
 * from the router tick; non-blocking. */
void rf_debug_view_refresh(void);

/* Accept a key event — the diagnostic view has no interactive bindings
 * today, but the hook exists so future per-view gestures plug in cleanly. */
void rf_debug_view_apply(const key_event_t *ev);
