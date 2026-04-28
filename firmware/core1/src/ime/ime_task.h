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

/* Discard the current committed text buffer (and reset cursor to 0).
 * Used by the messages-send flow after a successful out-bound push so
 * the user gets a clean slate for the next message. Bumps the dirty
 * counter so the IME view repaints. Acquires the shared snapshot
 * mutex internally; safe to call from any task that doesn't already
 * hold ime_view_lock. */
void ime_view_clear_text(void);

/* One-char current mode tag ("中" / "EN" / "ABC"). */
const char *ime_view_mode_indicator(void);

/* Numeric form of the active InputMode for SWD-side observability:
 *   0 = SmartZh, 1 = SmartEn, 2 = Direct, 0xFF = engine not initialised. */
uint8_t     ime_view_mode_byte(void);

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

/* ── SYM1 long-press symbol picker (Phase 1.4 Task B) ────────────────── */
bool        ime_view_picker_active(void);
int         ime_view_picker_cell_count(void);
int         ime_view_picker_cols(void);
const char *ime_view_picker_cell(int idx);
int         ime_view_picker_selected(void);

/* ── Generic text-input request (post-Stage 3, MIE reuse pattern) ────── *
 *
 * Lets any view ask the IME for a UTF-8 string without having to wire
 * up its own modal borrow + callback + truncation. Internally:
 *
 *   1. Validates that no other request is in flight (one at a time).
 *   2. Pre-fills g_text with `req->initial` (NULL = empty).
 *   3. Stores the user callback + ctx + max_bytes in file-static state.
 *   4. Calls view_router_modal_enter(IME view, trampoline) so FUNC press
 *      finishes the borrow.
 *   5. On modal exit, the trampoline reads g_text, UTF-8-safe-truncates
 *      to max_bytes, invokes the user callback, clears the request
 *      state, and clears g_text for the next request.
 *
 * `prompt`, `mode_hint`, and `flags` are accepted now but not yet
 * honoured — they reserve the API surface for follow-up work
 * (header label, initial mode, ASCII/NUMERIC/HEX restrictions). The
 * truncation done before the callback fires is the only piece that's
 * fully wired in this revision.
 *
 * Returns false if a request is already active or the IME isn't ready.
 */
typedef enum {
    IME_TEXT_FLAG_NONE        = 0,
    IME_TEXT_FLAG_ALLOW_EMPTY = (1u << 0),  /* commit empty string is OK */
    IME_TEXT_FLAG_ASCII_ONLY  = (1u << 1),  /* hint only — not yet enforced */
    IME_TEXT_FLAG_NUMERIC_ONLY= (1u << 2),  /* hint only */
    IME_TEXT_FLAG_HEX_ONLY    = (1u << 3),  /* hint only */
} ime_text_flags_t;

typedef enum {
    IME_TEXT_MODE_DEFAULT  = 0,             /* keep whatever mode IME is in */
    IME_TEXT_MODE_SMART_ZH = 1,
    IME_TEXT_MODE_SMART_EN = 2,
    IME_TEXT_MODE_DIRECT   = 3,
} ime_text_mode_hint_t;

/* Visual layout for the IME submode. Mode A = inline 24 px strip parented
 * to the bottom of the caller's view (used by A-2 conversation compose).
 * Mode B = fullscreen 208 px panel (used by Settings text edit, message
 * compose). Phase 2 records the layout end-to-end; the ime_view side
 * still renders fullscreen for both, with Mode A's true inline geometry
 * arriving when conversation_view in Phase 3 needs it. */
typedef enum {
    IME_TEXT_LAYOUT_FULLSCREEN = 0,         /* Mode B (default) */
    IME_TEXT_LAYOUT_INLINE     = 1,         /* Mode A           */
} ime_text_layout_t;

typedef struct {
    const char *prompt;       /* TODO: header label, ignored for now */
    const char *initial;      /* pre-fill text (NULL = empty)        */
    uint16_t    max_bytes;    /* UTF-8-safe truncate before callback */
    uint8_t     mode_hint;    /* TODO: ime_text_mode_hint_t, ignored */
    uint8_t     flags;        /* ime_text_flags_t bitmask            */
    /* Phase 2 additions (zero-init defaults preserve old behaviour) */
    uint8_t     layout;       /* ime_text_layout_t — A inline / B full */
    uint8_t     reserved[3];
    uint32_t    draft_id;     /* 0 = no flash-backed draft persistence */
} ime_text_request_t;

typedef void (*ime_text_done_fn)(bool        committed,
                                 const char *utf8,
                                 uint16_t    byte_len,
                                 void       *ctx);

bool ime_request_text(const ime_text_request_t *req,
                      ime_text_done_fn          done,
                      void                     *ctx);

/* True if a request is currently in flight (between ime_request_text and
 * its callback). Diagnostic; settings_view's modal flow reads this to
 * detect re-entry before pushing a new request. */
bool ime_request_text_active(void);

/* Layout flag of the in-flight request (IME_TEXT_LAYOUT_*). Read by
 * ime_view to choose its render geometry. Returns
 * IME_TEXT_LAYOUT_FULLSCREEN when no request is active. */
uint8_t ime_request_text_layout(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
