/* status_bar.h — G-1 persistent status strip per docs/ui/10-status-bar.md.
 *
 * 16 px tall overlay parented to the active screen at y=0. Owns 9 logical
 * cells: time, TX/RX lights, warning, neighbours, GPS, unread, battery,
 * mode. Driven by `status_bar_tick()` from the view router; per-element
 * setters are exposed so events (TX/RX edges, alert push) can fire
 * without waiting for the next tick.
 *
 * Implementation status (Phase 1 baseline):
 *   ✅ time, neighbours, GPS, unread, battery, mode
 *   ⏳ TX/RX pulses — stubbed; IPC_MSG_TX_DONE / RX_DONE not yet wired
 *   ⏳ alert overlay — stub API (single-line message, no SOS / low-batt
 *      colour band yet)
 *
 * Threading: must be called from the lvgl_task context (LV_OS_NONE).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_STATUS_BAR_H
#define MOKYA_CORE1_STATUS_BAR_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STATUS_BAR_HEIGHT  16

typedef enum {
    STATUS_BAR_MODE_OP   = 0,   /* Op (white)        */
    STATUS_BAR_MODE_ZH   = 1,   /* 注 (orange)       */
    STATUS_BAR_MODE_EN   = 2,   /* EN (orange)       */
    STATUS_BAR_MODE_AB   = 3,   /* Ab (orange)       */
} status_bar_mode_t;

void status_bar_init(lv_obj_t *screen);

/* Pulled once per view-router tick. Reads battery/GPS/cache and updates
 * cells. Cheap when nothing changed. */
void status_bar_tick(void);

/* Edge events. Pulses last ~250 ms then fade to dim. */
void status_bar_pulse_tx(void);
void status_bar_pulse_rx(void);

/* Mode label — view router calls when active view changes; IME view
 * calls when input method changes. */
void status_bar_set_mode(status_bar_mode_t mode);

/* One-shot alert (e.g. low battery, SOS). `level` 0 = info, 1 = warn,
 * 2 = critical. duration_ms = 0 → sticky until cleared. */
void status_bar_show_alert(uint8_t level, const char *text, uint32_t duration_ms);
void status_bar_clear_alert(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  /* MOKYA_CORE1_STATUS_BAR_H */
