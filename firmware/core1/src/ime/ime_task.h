/* ime_task.h -- FreeRTOS task wrapping mie::ImeLogic.
 *
 * Owns two TrieSearcher instances (ZH + optional EN), one ImeLogic
 * instance, and an IImeListener adapter that maintains a local text
 * buffer with a cursor — matching the semantics of the PC REPL
 * (firmware/mie/tools/mie_repl.cpp). The task drains the KeyEvent
 * queue (single consumer per §4.4) and calls ImeLogic::tick() on
 * each pop / timeout so multi-tap auto-commit and SYM1 long-press
 * fire even when no keys are pressed.
 *
 * Text + cursor semantics (REPL-faithful):
 *   - on_commit(utf8)    → insert at cursor; cursor advances
 *   - on_cursor_move(d)  → move cursor left/right in g_text
 *   - on_delete_before() → delete one UTF-8 codepoint before cursor
 *   - sync_text_context  → feed the 2 codepoints before cursor to
 *                          ImeLogic so SmartEn leading-space / auto-
 *                          capitalize reflect the local buffer state
 *
 * Call order from main_core1_bridge.c:
 *   1. key_event_init()
 *   2. mie_dict_load_to_psram(&dict)
 *   3. ime_task_start(&dict, priority)
 *
 * LVGL view readers (Task 5) acquire ime_view_lock() before touching
 * any of the snapshot accessors below.
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

/* Create and start the IME task. Returns false on any failure:
 * searcher load, ImeLogic construction, mutex allocation, or
 * xTaskCreate. On failure internal state is rolled back so the
 * caller may panic or retry. */
bool ime_task_start(const mie_dict_pointers_t *dict, UBaseType_t priority);

/* ── LVGL read-side API ───────────────────────────────────────────── */

/* Dirty counter: LVGL polls this in its refresh tick; re-renders
 * when the value differs from the last observed one. Bumped by
 * any listener callback that mutates renderable state. Safe to
 * read without the mutex. */
extern volatile uint32_t g_ime_dirty_counter;

/* Acquire / release the shared snapshot mutex. Short critical
 * section; the LVGL refresh path copies out to local buffers then
 * releases before touching any LVGL widget. */
bool ime_view_lock(TickType_t timeout_ticks);
void ime_view_unlock(void);

/* Pending composition (current in-flight input — "ㄋㄧˇ" while the
 * user is typing). Returns the raw buffer, its byte length, the
 * matched-prefix byte offset for PrefixBold rendering, and a style
 * hint (0 = None, 1 = Inverted, 2 = PrefixBold). */
const char *ime_view_pending(int *byte_len, int *matched_prefix, int *style);

/* Committed text buffer + cursor byte offset. The caller may copy
 * the whole string in one shot; buffer is a plain UTF-8 C string.
 * *cursor_bytes is the byte offset of the insertion point. */
const char *ime_view_text(int *byte_len, int *cursor_bytes);

/* One-char current mode tag ("中" / "EN" / "ABC"). */
const char *ime_view_mode_indicator(void);

/* Engine pagination (kPageSize = 5) — TAB cycles between pages. */
int         ime_view_page_candidate_count(void);
const char *ime_view_page_candidate(int idx);
int         ime_view_page_selected(void);
int         ime_view_page(void);
int         ime_view_page_count(void);

/* Full candidate pool (up to kMaxCandidates = 50). DPAD LEFT/RIGHT
 * moves `selected` one at a time across the full pool. */
int         ime_view_candidate_count(void);
const char *ime_view_candidate(int idx);
int         ime_view_selected(void);

/* Override the engine's selected index. Used by the LVGL view to implement
 * row-based Up/Down navigation that follows the visual layout instead of
 * the engine's fixed kPageSize=5 jump. Callable from any task; takes the
 * snapshot mutex internally. */
void        ime_view_set_selected(int idx);

#ifdef __cplusplus
} /* extern "C" */
#endif
