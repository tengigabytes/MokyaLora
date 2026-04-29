/* global_alert.h — G-4 cross-view notification orchestration.
 *
 * Drives the existing 16 px status_bar alert overlay (status_bar_show_alert)
 * with two event sources:
 *
 *   1. Inbound DM toast — when dm_store sees a new inbound message AND the
 *      user is not already viewing that peer's MESSAGES_CHAT, briefly
 *      surface "Msg <name>: <preview>" (level=info, 4 s).
 *   2. Low-battery — when bq25622 vbat_mv crosses below LOW_THRESHOLD_MV
 *      and stays there, latch a sticky critical alert; clear once vbat
 *      crosses above CLEAR_THRESHOLD_MV (hysteresis).
 *
 * The third part of G-4 (compose-draft recovery) is already implemented
 * via firmware/core1/src/ime/draft_store.c + the draft_id field passed
 * through ime_request_text — see ime_task.cpp:551 (save on cancel) /
 * :601 (load on enter).
 *
 * Hook: view_router_tick() calls global_alert_tick() once per LVGL frame
 * after status_bar_tick(). Internal throttling keeps the bq25622 read
 * to ~5 Hz and the dm_store change scan to ~5 Hz.
 *
 * Threading: same context as status_bar (LVGL task / LV_OS_NONE).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_GLOBAL_ALERT_H
#define MOKYA_CORE1_GLOBAL_ALERT_H

#ifdef __cplusplus
extern "C" {
#endif

void global_alert_init(void);
void global_alert_tick(void);

#ifdef __cplusplus
}
#endif

#endif  /* MOKYA_CORE1_GLOBAL_ALERT_H */
