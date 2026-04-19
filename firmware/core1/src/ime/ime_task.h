/* ime_task.h -- FreeRTOS task wrapping mie::ImeLogic.
 *
 * Owns two TrieSearcher instances (ZH + optional EN), one ImeLogic
 * instance, and an IImeListener adapter that captures the composition
 * / candidate / commit state for the LVGL view to render. The task
 * drains the KeyEvent queue (single consumer per §4.4) and also calls
 * ImeLogic::tick() on each pop / timeout so multi-tap auto-commit and
 * SYM1 long-press fire even when no keys are pressed.
 *
 * Call order from main_core1_bridge.c:
 *   1. key_event_init()        (queue is created)
 *   2. mie_dict_load_to_psram(&dict)
 *   3. ime_task_start(&dict, priority)
 *
 * LVGL view readers (Task 5) use the getter API below to produce a
 * snapshot suitable for rendering. The snapshot pointers are only
 * valid while the caller holds the mutex obtained via
 * ime_view_lock(); callers MUST release via ime_view_unlock()
 * before returning to the LVGL task loop.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"

#include "mie_dict_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create and start the IME task. Returns false if any step fails:
 * searcher load, ImeLogic construction, mutex allocation, or
 * xTaskCreate. On failure internal state is rolled back so the caller
 * may panic or retry. */
bool ime_task_start(const mie_dict_pointers_t *dict, UBaseType_t priority);

/* ── LVGL read-side API (Task 5) ──────────────────────────────────── */

/* Dirty counter: LVGL polls this in its timer; re-renders when the
 * value differs from the last observed one. Bumped by any listener
 * callback that mutates renderable state (composition / candidate /
 * commit buffer). Safe to read without the mutex. */
extern volatile uint32_t g_ime_dirty_counter;

/* Acquire / release the shared snapshot mutex. LVGL render callback
 * wraps its state reads in this pair. timeout_ticks may be
 * portMAX_DELAY; returns false on timeout. */
bool ime_view_lock(TickType_t timeout_ticks);
void ime_view_unlock(void);

/* Snapshot accessors. Only valid between ime_view_lock() / unlock(). */
const char *ime_view_pending(int *byte_len, int *matched_prefix, int *style);
const char *ime_view_mode_indicator(void);
int         ime_view_page_candidate_count(void);
const char *ime_view_page_candidate(int idx);
int         ime_view_page_selected(void);
int         ime_view_page(void);
int         ime_view_page_count(void);

/* Commit buffer accessors. The buffer accumulates every on_commit()
 * UTF-8 fragment (newest at the tail); bounded at ~1 KB, drops the
 * oldest half when full. */
const char *ime_view_commit_text(int *byte_len);
void        ime_view_clear_commit(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
