/* template_text.c — see template_text.h.
 *
 * Delegates all rendering to ime_request_text (IME Mode B fullscreen).
 * Stores the committed UTF-8 bytes locally in s_buf so the host can
 * read them out after the IME callback fires.
 *
 * Buffer sizing: the largest SK_KIND_STR key today is Owner Long
 * Name (39 bytes per Meshtastic spec). The PSK / Admin Key strings
 * are 32 hex bytes when entered as text. 64 bytes covers everything
 * with headroom; the IME truncates internally to req.max_bytes
 * regardless.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "template_text.h"

#include <stdint.h>
#include <string.h>

#include "ime_task.h"

/* ── State ──────────────────────────────────────────────────────────── */

#define TT_BUF_MAX  64u

/* PSRAM-resident — written once per modal commit on the lvgl_task,
 * read once by settings_app_view's exit_text_edit. Low-frequency
 * single-core access, no SWD inspection target — safe vs PSRAM
 * write-back cache (project_psram_swd_cache_coherence). Saves 65 B
 * of the tight Core 1 BSS budget that the four templates' widget
 * pointers + state already eat into. */
static char     s_buf[TT_BUF_MAX + 1] __attribute__((section(".psram_bss")));
static uint16_t s_len;
static bool     s_done;
static bool     s_committed;

/* IME callback — runs on the lvgl_task context after modal_finish.
 * Copies the freshly-committed UTF-8 into our buffer so the host
 * (settings_app_view) can poll and ship via IPC SET. */
static void on_ime_done(bool committed, const char *utf8,
                        uint16_t byte_len, void *ctx)
{
    (void)ctx;
    if (committed && utf8 != NULL) {
        uint16_t n = byte_len < TT_BUF_MAX ? byte_len : TT_BUF_MAX;
        memcpy(s_buf, utf8, n);
        s_buf[n] = '\0';
        s_len = n;
    } else {
        s_buf[0] = '\0';
        s_len = 0;
    }
    s_committed = committed;
    s_done      = true;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void template_text_open(const settings_key_def_t *key)
{
    s_buf[0]   = '\0';
    s_len      = 0;
    s_done     = false;
    s_committed = false;

    /* Cap edit length at the key's `max` (the wire-protocol upper
     * bound) but never exceed our local buffer. */
    uint16_t cap = TT_BUF_MAX;
    if (key && key->max > 0 && (uint16_t)key->max < cap) {
        cap = (uint16_t)key->max;
    }

    ime_text_request_t req = {
        .prompt    = key && key->label ? key->label : "Edit",
        .initial   = NULL,                 /* current value not pre-filled
                                            * yet — phoneapi cache lookup
                                            * lands when each kind gets
                                            * a uniform getter */
        .max_bytes = cap,
        .mode_hint = IME_TEXT_MODE_DEFAULT,
        .flags     = IME_TEXT_FLAG_NONE,
        /* Mode B: fullscreen panel, settings App goes inactive while
         * the IME owns the screen. Template D is settings → IME, not
         * an inline overlay. */
        .layout    = IME_TEXT_LAYOUT_FULLSCREEN,
        .draft_id  = key ? (uint32_t)key->ipc_key : 0u,
    };
    if (!ime_request_text(&req, on_ime_done, NULL)) {
        /* ime_request_text only fails if a modal is already in flight
         * — treat as cancel so the host exits cleanly. */
        s_done      = true;
        s_committed = false;
    }
}

void template_text_close(void) { /* IME modal cleans up its own widgets */ }

bool template_text_apply_key(const key_event_t *ev)
{
    /* IME modal owns the screen and consumes every key event. The
     * host should NOT also forward events here. Return false so the
     * host knows we didn't consume anything (defensive — apply_key
     * should not even be called while we're delegating to IME). */
    (void)ev;
    return false;
}

bool        template_text_done(void)      { return s_done; }
bool        template_text_committed(void) { return s_committed; }
const char *template_text_value(void)     { return s_buf; }
uint16_t    template_text_value_len(void) { return s_len; }
