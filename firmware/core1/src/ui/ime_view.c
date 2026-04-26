/* ime_view.c — see ime_view.h.
 *
 * Layout ported from firmware/mie/tools/mie_repl.cpp (§render_text_line
 * / §render_cand_line). The widget set differs — we use lv_textarea for
 * the text row so LVGL draws a blinking cursor for us, and lv_label +
 * flex-wrap for the candidate row. Semantics match the REPL:
 *
 *   [commit-left] [pending in brackets] cursor [commit-right]    ← row 1
 *   [mode]  cand  cand  cand ...                   [N/M  sel/tot] ← row 2
 *
 * Pending is wrapped with the ASCII `[...]` because lv_textarea has no
 * inline-styled-run support and lv_spangroup's per-span font fallback
 * drops the MIEF glyph set. Brackets echo the REPL's reverse-video run
 * closely enough for visual disambiguation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ime_view.h"

#include <stdio.h>
#include <string.h>

#include "mie/keycode.h"

#include "ime_task.h"
#include "mie_font.h"
#include "mokya_trace.h"

/* ── SWD diagnostic — see docs/design-notes/firmware-architecture.md §9.3.
 *
 * Parked in the tail of the 24 KB shared IPC pad (0x2007FE00 is clear of
 * the bridge_task breadcrumbs at 0x2007FFC4..CC). The layout is designed
 * so that `mem32 0x2007FE00 N` from a J-Link Commander script prints
 * enough to diagnose "why is an empty cell appearing?":
 *   - magic (32-bit) confirms the struct is alive
 *   - refresh_count bumps every render, exposes live-ness
 *   - cand_count / selected mirror the last snapshot
 *   - words[0..7] capture the first 8 candidate snapshot strings as-is
 *     (NUL-terminated inside their slot) so an empty cell shows up as
 *     a zero-byte slot.
 */
#define IME_VIEW_DEBUG_ADDR   0x2007FE00u
/* v0003 layout — offsets fixed so external tooling (scripts/inject_keys.py)
 * can map fields by absolute address without re-reading the ELF. Total
 * size = 432 B; lives inside the 448 B free window below the SWD tail
 * pad at 0x2007FFC0. */
/* Size must fit in [0x2007FE00, 0x2007FFC0) = 448 B — the final 64 B at
 * 0x2007FFC0+ are reserved for IPC counters (rx total, tx total, USB
 * state, LVGL assert stamp; see firmware-architecture.md §9.3). An
 * earlier v0003 layout used 512 B and its pending_buf overlapped those
 * counters, giving garbage reads from SWD tooling. */
typedef struct {
    uint32_t magic;            /* 0x000  IME_VIEW_DEBUG_MAGIC */
    /* Seq-lock (Lamport pattern). Start-of-write bumps to an odd value;
     * end-of-write bumps to the next even value. A SWD reader that halts
     * the CPU mid-write sees an odd seq (or sees seq change between two
     * reads of the same struct) and retries. */
    uint32_t seq;              /* 0x004 */
    uint32_t refresh_count;    /* 0x008 */
    int32_t  cand_count;       /* 0x00C */
    int32_t  selected;         /* 0x010 */
    char     words[8][24];     /* 0x014..0x0D3  8 × 24 = 192 B            */
    uint8_t  raw0[16];         /* 0x0D4..0x0E3  raw of candidates_[0]     */
    uint8_t  raw1[16];         /* 0x0E4..0x0F3  raw of candidates_[1]     */
    uint32_t commit_count;     /* 0x0F4  bumps when text_len changes      */
    int32_t  text_len;         /* 0x0F8 */
    int32_t  cursor_pos;       /* 0x0FC */
    char     text_buf[96];     /* 0x100..0x15F  committed text, NUL-term  */
    int32_t  pending_len;      /* 0x160 */
    char     pending_buf[32];  /* 0x164..0x183  pending key composition   */
    uint8_t  mode;             /* 0x184  0=SmartZh, 1=SmartEn, 2=Direct   */
    uint8_t  reserved[11];     /* 0x185..0x18F  pad to 0x190 (= 400 B)    */
} ime_view_debug_t;
#define IME_VIEW_DEBUG_MAGIC  0xEEED0003u
static volatile ime_view_debug_t *const g_dbg =
    (volatile ime_view_debug_t *)IME_VIEW_DEBUG_ADDR;

/* Full candidate mirror — exposes all 100 engine candidates so an
 * SWD-driven test harness can find a target char beyond the top-8
 * window shown in ime_view_debug_t. Placed in regular .bss so its
 * address is resolvable via ELF nm — not stuck in the cramped
 * shared_ipc region. Seq-lock pattern matches the small debug struct. */
#define IME_CAND_FULL_MAGIC   0xECA11100u
#define IME_CAND_FULL_MAX     100
#define IME_CAND_FULL_WLEN    24
typedef struct {
    uint32_t magic;                                     /* 0 */
    uint32_t seq;                                       /* 4 */
    int32_t  count;                                     /* 8 */
    int32_t  selected;                                  /* 12 */
    char     words[IME_CAND_FULL_MAX][IME_CAND_FULL_WLEN]; /* 16..2416 */
} ime_cand_full_t;
volatile ime_cand_full_t g_ime_cand_full __attribute__((used));

/* ── Screen geometry (landscape 320×240) ─────────────────────────────── */
#define SCREEN_W            320
#define SCREEN_H            240

#define TEXT_Y                0
#define TEXT_H              180
#define DIVIDER_Y           181

#define CAND_Y              184
#define CAND_H               56

#define MODE_W               36
#define PAGE_W               56

/* ── Palette ─────────────────────────────────────────────────────────── */
static const lv_color_t COL_BG       = LV_COLOR_MAKE(0x0B, 0x0F, 0x14);
static const lv_color_t COL_DIVIDER  = LV_COLOR_MAKE(0x1E, 0x29, 0x3B);
static const lv_color_t COL_TEXT     = LV_COLOR_MAKE(0xE5, 0xE7, 0xEB);
static const lv_color_t COL_TEXT_DIM = LV_COLOR_MAKE(0x94, 0xA3, 0xB8);
static const lv_color_t COL_CAND_BG  = LV_COLOR_MAKE(0x1F, 0x29, 0x37);
static const lv_color_t COL_SEL_BG   = LV_COLOR_MAKE(0x22, 0xD3, 0xEE);
static const lv_color_t COL_SEL_TEXT = LV_COLOR_MAKE(0x0B, 0x0F, 0x14);
static const lv_color_t COL_MODE_BG  = LV_COLOR_MAKE(0x2D, 0x3A, 0x4E);
static const lv_color_t COL_CURSOR   = LV_COLOR_MAKE(0x22, 0xD3, 0xEE);

/* ── Widget handles ──────────────────────────────────────────────────── */
static lv_obj_t *s_ta;              /* lv_textarea (read-only display)  */
static lv_obj_t *s_mode_lbl;
static lv_obj_t *s_page_lbl;
static lv_obj_t *s_cand_box;

/* ── Dirty tracking ──────────────────────────────────────────────────── */
static uint32_t s_last_counter = (uint32_t)-1;

/* ── Snapshot buffers ────────────────────────────────────────────────── */
#define TEXT_BUF_SZ        2048
#define PENDING_BUF_SZ      128
#define CAND_BUF_SZ          48
/* Engine ImeLogic::kMaxCandidates is 100, but pre-allocating 100 cells
 * exhausts LVGL's heap. Each cell is ~440 B (lv_obj + lv_label + style).
 * With LV_MEM_SIZE bumped to 56 KB, 40 cells uses ~17.6 KB and leaves
 * room for the rest of the view tree. Rank 41-100 is still reached via
 * the engine's stored candidates, but the visible UI only paginates
 * top 40 — future paging UX can window into the remainder. */
#define CAND_MAX             40
#define COMBINED_BUF_SZ   (TEXT_BUF_SZ + PENDING_BUF_SZ + 8)

static char s_text_buf[TEXT_BUF_SZ + 1];
static int  s_text_len;
static int  s_cursor_bytes;

static char s_pending_buf[PENDING_BUF_SZ + 1];

static char s_mode_buf[16];

static char s_cand_buf[CAND_MAX][CAND_BUF_SZ];
static int  s_cand_count;
static int  s_cand_total;
static int  s_selected;

static char s_combined[COMBINED_BUF_SZ];

/* Cell layout coordinates — captured after lv_obj_update_layout() finishes
 * the flex-wrap pass so apply() can map DPAD Up/Down onto a visual row
 * above/below the currently-selected cell. */
static int16_t s_cell_x[CAND_MAX];
static int16_t s_cell_y[CAND_MAX];
static int16_t s_cell_valid;   /* number of entries populated in the arrays */

/* Pre-allocated candidate cells. Created once in ime_view_init and reused
 * every refresh — render_candidates only updates label text + bg color and
 * toggles HIDDEN. Killing the create/destroy cycle dropped per-keystroke
 * render from ~120 ms to single-digit ms (RTT trace 2026-04-22). */
static lv_obj_t *s_cand_cells[CAND_MAX];
static lv_obj_t *s_cand_lbls[CAND_MAX];

/* Cached per-cell text so text writes can short-circuit (LVGL text set
 * is the expensive part — glyph layout reruns). Selection bg/color is
 * always written every frame (see render_candidates comment). */
static int  s_last_cand_count = -1;
static char s_last_cell_text[CAND_MAX][CAND_BUF_SZ];

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void make_divider(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, SCREEN_W, 1);
    lv_obj_set_pos(line, 0, y);
    lv_obj_set_style_bg_color(line, COL_DIVIDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

/* ── Init ────────────────────────────────────────────────────────────── */

void ime_view_init(lv_obj_t *panel)
{
    const lv_font_t *f16 = mie_font_unifont_sm_16();

    lv_obj_set_style_bg_color(panel, COL_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(panel, COL_TEXT, 0);

    /* Text area — lv_textarea. Read-only (no focus / no keyboard input);
     * set_text rewrites the whole line each frame. Cursor is drawn by
     * LVGL at the byte position set via lv_textarea_set_cursor_pos. */
    s_ta = lv_textarea_create(panel);
    lv_obj_set_pos(s_ta, 0, TEXT_Y);
    lv_obj_set_size(s_ta, SCREEN_W, TEXT_H);
    lv_obj_set_style_bg_color(s_ta, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ta, 0, 0);
    lv_obj_set_style_pad_all(s_ta, 4, 0);
    lv_obj_set_style_text_color(s_ta, COL_TEXT, 0);
    if (f16) lv_obj_set_style_text_font(s_ta, f16, 0);
    lv_obj_set_style_text_line_space(s_ta, 4, 0);
    lv_obj_set_style_bg_color(s_ta, COL_CURSOR, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_text_color(s_ta, COL_SEL_TEXT, LV_PART_CURSOR);
    /* Keep the cursor always visible (no blink) — the blink rate in
     * LVGL is global, and we'd rather the user always see where the
     * insertion point is. */
    lv_obj_set_style_anim_duration(s_ta, 0, LV_PART_CURSOR);
    lv_textarea_set_cursor_click_pos(s_ta, false);
    lv_textarea_set_one_line(s_ta, false);
    lv_textarea_set_text(s_ta, "");
    /* Disable actual edit — we're using this as a styled display. */
    lv_obj_add_flag(s_ta, LV_OBJ_FLAG_CLICKABLE);   /* still scrollable */
    lv_obj_clear_state(s_ta, LV_STATE_FOCUSED);

    make_divider(panel, DIVIDER_Y);

    /* Candidate row — mode pill | flex-wrap candidates | page pill. */
    s_mode_lbl = lv_label_create(panel);
    lv_obj_set_pos(s_mode_lbl, 4, CAND_Y + 8);
    lv_obj_set_size(s_mode_lbl, MODE_W - 4, 20);
    if (f16) lv_obj_set_style_text_font(s_mode_lbl, f16, 0);
    lv_obj_set_style_text_color(s_mode_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_align(s_mode_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_mode_lbl, COL_MODE_BG, 0);
    lv_obj_set_style_bg_opa(s_mode_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_mode_lbl, 4, 0);
    lv_obj_set_style_pad_ver(s_mode_lbl, 2, 0);
    lv_obj_set_style_radius(s_mode_lbl, 4, 0);
    lv_label_set_text(s_mode_lbl, "");

    s_cand_box = lv_obj_create(panel);
    lv_obj_set_pos(s_cand_box, MODE_W + 4, CAND_Y + 2);
    lv_obj_set_size(s_cand_box, SCREEN_W - MODE_W - PAGE_W - 8, CAND_H - 4);
    lv_obj_set_style_bg_opa(s_cand_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cand_box, 0, 0);
    lv_obj_set_style_pad_all(s_cand_box, 0, 0);
    lv_obj_set_style_pad_row(s_cand_box, 2, 0);
    lv_obj_set_style_pad_column(s_cand_box, 6, 0);
    lv_obj_set_flex_flow(s_cand_box, LV_FLEX_FLOW_ROW_WRAP);
    /* Enable vertical scroll so rows that don't fit stay reachable;
     * the refresh path calls lv_obj_scroll_to_view on the selected
     * cell to keep the highlight visible as the user navigates. */
    lv_obj_set_scroll_dir(s_cand_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cand_box, LV_SCROLLBAR_MODE_AUTO);

    /* Pre-allocate every candidate cell up-front. All start hidden +
     * styled with the unselected colour. render_candidates() will only
     * update text / bg / HIDDEN per refresh, which is single-digit-ms
     * vs the ~120 ms we used to spend recreating them all. */
    for (int i = 0; i < CAND_MAX; ++i) {
        lv_obj_t *cell = lv_obj_create(s_cand_box);
        lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(cell, 4, 0);
        lv_obj_set_style_pad_ver(cell, 1, 0);
        lv_obj_set_style_radius(cell, 3, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_bg_color(cell, COL_CAND_BG, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, "");
        if (f16) lv_obj_set_style_text_font(lbl, f16, 0);
        lv_obj_set_style_text_color(lbl, COL_TEXT, 0);

        s_cand_cells[i] = cell;
        s_cand_lbls[i]  = lbl;
        s_last_cell_text[i][0] = '\0';
    }

    s_page_lbl = lv_label_create(panel);
    lv_obj_set_pos(s_page_lbl, SCREEN_W - PAGE_W, CAND_Y + 8);
    lv_obj_set_size(s_page_lbl, PAGE_W - 4, 20);
    if (f16) lv_obj_set_style_text_font(s_page_lbl, f16, 0);
    lv_obj_set_style_text_color(s_page_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_page_lbl, "");
}

/* Map a DPAD Up/Down press onto a row step in the flex-wrapped candidate
 * layout. Engine-level Up/Down jumps by kPageSize=5, which doesn't line up
 * with the UI's visual rows — we override by finding the cell above/below
 * the current selection at the closest X column and setting the engine's
 * selected index to that. Layout arrays are populated on the last render,
 * so this reflects exactly what the user is seeing. */
static int find_row_neighbour(int dir /* -1 = up, +1 = down */)
{
    if (s_cell_valid <= 1) return -1;
    int cur = s_selected;
    if (cur < 0 || cur >= s_cell_valid) return -1;

    int cur_y = s_cell_y[cur];
    int cur_x = s_cell_x[cur];

    /* Find the Y of the nearest row in the requested direction. */
    int target_y = -1;
    for (int i = 0; i < s_cell_valid; ++i) {
        int y = s_cell_y[i];
        if (dir < 0 && y < cur_y) {
            if (target_y < 0 || y > target_y) target_y = y;
        } else if (dir > 0 && y > cur_y) {
            if (target_y < 0 || y < target_y) target_y = y;
        }
    }
    if (target_y < 0) return -1;

    /* On that row, pick the cell closest to the current X. */
    int best_idx  = -1;
    int best_dist = 0;
    for (int i = 0; i < s_cell_valid; ++i) {
        if (s_cell_y[i] != target_y) continue;
        int d = s_cell_x[i] - cur_x;
        if (d < 0) d = -d;
        if (best_idx < 0 || d < best_dist) { best_idx = i; best_dist = d; }
    }
    return best_idx;
}

void ime_view_apply(const key_event_t *ev)
{
    if (!ev || !ev->pressed) return;
    /* SYM1 picker has its own engine-side DPAD routing (steps ±cols on
     * UP/DOWN, ±1 on LEFT/RIGHT, mod cell_count). Skip the view's
     * row-neighbour remap so we don't fight the engine and flash a
     * stale selection between snapshots. */
    if (ime_view_picker_active()) return;
    if (s_cand_total <= 0) return;         /* DPAD with no candidates falls
                                            * through to engine's cursor move */

    /* Up/Down is engine-suppressed (see ImeLogic::handle_dpad); the view
     * owns these keys exclusively. Primary path = jump to the closest cell
     * on the row above/below. Fallback when only one visible row exists:
     * behave like Left/Right so the user still gets a useful 1-step move.
     */
    int target = -1;
    int dir    = 0;
    if (ev->keycode == MOKYA_KEY_UP)   { target = find_row_neighbour(-1); dir = -1; }
    if (ev->keycode == MOKYA_KEY_DOWN) { target = find_row_neighbour(+1); dir = +1; }
    if (dir == 0) return;

    if (target < 0 && s_cand_total > 0) {
        /* Single-row fallback: cycle the selection within the visible row. */
        int sel = s_selected;
        if (sel < 0) sel = 0;
        if (dir > 0) sel = (sel + 1) % s_cand_total;
        else         sel = (sel - 1 + s_cand_total) % s_cand_total;
        target = sel;
    }
    if (target >= 0) {
        ime_view_set_selected(target);
    }
}

/* ── Snapshot + render ───────────────────────────────────────────────── */

static bool snapshot(void)
{
    if (!ime_view_lock(pdMS_TO_TICKS(5))) return false;

    /* Text + cursor. */
    int tlen = 0, cpos = 0;
    const char *t = ime_view_text(&tlen, &cpos);
    if (tlen < 0) tlen = 0;
    if (tlen > TEXT_BUF_SZ) tlen = TEXT_BUF_SZ;
    if (t && tlen > 0) memcpy(s_text_buf, t, (size_t)tlen);
    s_text_buf[tlen] = '\0';
    s_text_len     = tlen;
    s_cursor_bytes = cpos;

    /* Pending composition. */
    int plen = 0;
    const char *p = ime_view_pending(&plen, NULL, NULL);
    if (plen < 0) plen = 0;
    if (plen > PENDING_BUF_SZ) plen = PENDING_BUF_SZ;
    if (p && plen > 0) memcpy(s_pending_buf, p, (size_t)plen);
    s_pending_buf[plen] = '\0';

    /* Mode + candidates. */
    const char *mi = ime_view_mode_indicator();
    if (!mi) mi = "";
    strncpy(s_mode_buf, mi, sizeof(s_mode_buf) - 1);
    s_mode_buf[sizeof(s_mode_buf) - 1] = '\0';

    if (ime_view_picker_active()) {
        /* SYM1 long-press picker overlay. The picker's grid replaces the
         * candidate row contents — same flex-wrap renderer, just sourcing
         * its strings + selection from picker_* APIs. The user sees a
         * highlighted grid of common Traditional-Chinese punctuation. */
        int total = ime_view_picker_cell_count();
        if (total < 0) total = 0;
        int n = total;
        if (n > CAND_MAX) n = CAND_MAX;
        s_cand_count = n;
        s_cand_total = total;
        for (int i = 0; i < n; ++i) {
            const char *c = ime_view_picker_cell(i);
            if (!c) c = "";
            strncpy(s_cand_buf[i], c, CAND_BUF_SZ - 1);
            s_cand_buf[i][CAND_BUF_SZ - 1] = '\0';
        }
        s_selected = ime_view_picker_selected();
    } else {
        int total = ime_view_candidate_count();
        if (total < 0) total = 0;
        int n = total;
        if (n > CAND_MAX) n = CAND_MAX;
        s_cand_count = n;
        s_cand_total = total;
        for (int i = 0; i < n; ++i) {
            const char *w = ime_view_candidate(i);
            if (!w) w = "";
            strncpy(s_cand_buf[i], w, CAND_BUF_SZ - 1);
            s_cand_buf[i][CAND_BUF_SZ - 1] = '\0';
        }
        s_selected = ime_view_selected();
    }

    ime_view_unlock();
    return true;
}

static void render_text(void)
{
    /* Build commit_left + pending(in brackets) + commit_right and set
     * the textarea cursor to end-of-pending so LVGL draws it there. */
    int cpos = s_cursor_bytes;
    if (cpos < 0) cpos = 0;
    if (cpos > s_text_len) cpos = s_text_len;

    size_t off = 0;
    if (cpos > 0) {
        size_t n = (size_t)cpos;
        if (n > sizeof(s_combined) - 1) n = sizeof(s_combined) - 1;
        memcpy(s_combined, s_text_buf, n);
        off = n;
    }
    int pending_chars = 0;
    if (s_pending_buf[0] != '\0' && off + 2 < sizeof(s_combined)) {
        int add = snprintf(s_combined + off, sizeof(s_combined) - off,
                           "[%s]", s_pending_buf);
        if (add > 0) {
            off += (size_t)add;
            pending_chars = add;     /* byte count for cursor positioning */
        }
    }
    if (cpos < s_text_len) {
        size_t tail = (size_t)(s_text_len - cpos);
        if (off + tail > sizeof(s_combined) - 1) tail = sizeof(s_combined) - 1 - off;
        memcpy(s_combined + off, s_text_buf + cpos, tail);
        off += tail;
    }
    if (off >= sizeof(s_combined)) off = sizeof(s_combined) - 1;
    s_combined[off] = '\0';

    lv_textarea_set_text(s_ta, s_combined);

    /* Cursor byte position = cpos + pending chars; lv_textarea expects a
     * codepoint index, so convert bytes → codepoints by walking UTF-8. */
    int cursor_target_bytes = cpos + pending_chars;
    int cp_index = 0;
    for (int i = 0; i < cursor_target_bytes && i < (int)off;) {
        unsigned char b = (unsigned char)s_combined[i];
        int step = 1;
        if      ((b & 0x80u) == 0x00u) step = 1;
        else if ((b & 0xE0u) == 0xC0u) step = 2;
        else if ((b & 0xF0u) == 0xE0u) step = 3;
        else                           step = 4;
        i += step;
        ++cp_index;
    }
    lv_textarea_set_cursor_pos(s_ta, cp_index);
}

static void render_candidates(void)
{
    /* Update the SWD diagnostic first — it reflects the post-snapshot
     * view state regardless of any rendering skips, so an empty-word
     * slot shows up verbatim in mem32 0x2007FE00. */
    /* Seq-lock: bump to odd = "write in progress" so an SWD reader that
     * halts the CPU mid-update sees odd seq and retries. */
    g_dbg->magic         = IME_VIEW_DEBUG_MAGIC;
    g_dbg->seq           = g_dbg->seq + 1u;   /* odd */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_dbg->refresh_count = g_dbg->refresh_count + 1u;
    g_dbg->cand_count    = s_cand_count;
    g_dbg->selected      = s_selected;
    for (int i = 0; i < 8; ++i) {
        const char *src = (i < s_cand_count) ? s_cand_buf[i] : "";
        int j = 0;
        for (; j < (int)sizeof g_dbg->words[0] - 1 && src[j]; ++j) {
            g_dbg->words[i][j] = src[j];
        }
        g_dbg->words[i][j] = '\0';
    }
    /* Capture raw bytes of s_cand_buf[0] and [1] including trailing zeros
     * so the mis-parse signature is visible via mem8 (the NUL-stopping
     * words[] array masks them). */
    for (int k = 0; k < 16; ++k) {
        g_dbg->raw0[k] = (s_cand_count > 0) ? (uint8_t)s_cand_buf[0][k] : 0;
        g_dbg->raw1[k] = (s_cand_count > 1) ? (uint8_t)s_cand_buf[1][k] : 0;
    }

    /* v0003: committed-text + pending + mode mirror so an SWD-driven test
     * harness can read the engine's externally-visible state without
     * needing UART. text_buf is freshly snapped (s_text_buf), so each
     * read shows the current commit ledger. commit_count bumps whenever
     * text_len changes from the previous snapshot — a poor-man's edge
     * detector that lets the host distinguish "OK absorbed → text grew"
     * from "OK rejected (no candidate) → text unchanged". */
    static int s_prev_text_len = -1;
    if (s_text_len != s_prev_text_len) {
        g_dbg->commit_count = g_dbg->commit_count + 1u;
        s_prev_text_len = s_text_len;
    }
    g_dbg->text_len   = s_text_len;
    g_dbg->cursor_pos = s_cursor_bytes;
    {
        int n = s_text_len;
        if (n < 0) n = 0;
        if (n > (int)sizeof g_dbg->text_buf - 1)
            n = (int)sizeof g_dbg->text_buf - 1;
        /* Clamp on a UTF-8 boundary so truncation doesn't produce an
         * incomplete multi-byte sequence. */
        while (n > 0 && ((unsigned char)s_text_buf[n] & 0xC0) == 0x80) --n;
        for (int j = 0; j < n; ++j) g_dbg->text_buf[j] = s_text_buf[j];
        g_dbg->text_buf[n] = '\0';
    }
    {
        int plen = (int)strlen(s_pending_buf);
        if (plen > (int)sizeof g_dbg->pending_buf - 1)
            plen = (int)sizeof g_dbg->pending_buf - 1;
        while (plen > 0 && ((unsigned char)s_pending_buf[plen] & 0xC0) == 0x80) --plen;
        g_dbg->pending_len = plen;
        for (int j = 0; j < plen; ++j) g_dbg->pending_buf[j] = s_pending_buf[j];
        g_dbg->pending_buf[plen] = '\0';
    }
    g_dbg->mode = ime_view_mode_byte();

    /* End-of-write: bump to even = "stable snapshot complete". */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_dbg->seq = g_dbg->seq + 1u;   /* even */

    /* Fill the full-candidate mirror so SWD tooling can find a target
     * beyond the 8-candidate window. Copies UP TO 100 candidates by
     * direct engine query. Same seq-lock discipline. */
    g_ime_cand_full.magic = IME_CAND_FULL_MAGIC;
    g_ime_cand_full.seq   = g_ime_cand_full.seq + 1u;   /* odd */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    {
        int total = ime_view_candidate_count();
        if (total < 0) total = 0;
        if (total > IME_CAND_FULL_MAX) total = IME_CAND_FULL_MAX;
        g_ime_cand_full.count    = total;
        g_ime_cand_full.selected = s_selected;
        for (int i = 0; i < total; ++i) {
            const char *w = ime_view_candidate(i);
            if (!w) w = "";
            int j = 0;
            for (; j < IME_CAND_FULL_WLEN - 1 && w[j]; ++j)
                g_ime_cand_full.words[i][j] = w[j];
            g_ime_cand_full.words[i][j] = '\0';
        }
        for (int i = total; i < IME_CAND_FULL_MAX; ++i)
            g_ime_cand_full.words[i][0] = '\0';
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ime_cand_full.seq = g_ime_cand_full.seq + 1u;   /* even */

    /* Update each pre-allocated cell in place. Skip lv_label_set_text /
     * bg_color writes when nothing changed for that cell — those are the
     * expensive operations and they often no-op between adjacent keystrokes
     * (e.g. typing more pinyin keeps most cells the same). */
    bool any_layout_change = (s_cand_count != s_last_cand_count);
    for (int i = 0; i < CAND_MAX; ++i) {
        lv_obj_t *cell = s_cand_cells[i];
        lv_obj_t *lbl  = s_cand_lbls[i];
        const bool show = (i < s_cand_count) && (s_cand_buf[i][0] != '\0');

        if (!show) {
            if (!lv_obj_has_flag(cell, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
                any_layout_change = true;
            }
            continue;
        }

        if (lv_obj_has_flag(cell, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
            any_layout_change = true;
        }

        if (strncmp(s_last_cell_text[i], s_cand_buf[i], CAND_BUF_SZ) != 0) {
            lv_label_set_text(lbl, s_cand_buf[i]);
            strncpy(s_last_cell_text[i], s_cand_buf[i], CAND_BUF_SZ - 1);
            s_last_cell_text[i][CAND_BUF_SZ - 1] = '\0';
            any_layout_change = true;
        }

        /* Always write bg+text color for visible cells. The previous
         * short-circuit on (sel != was_sel) left stale SEL_BG on cells
         * that were hidden then unhidden with new content, causing
         * "two cells highlighted" after a commit cleared the list and
         * a new search repopulated it (2026-04-22). LVGL style writes
         * are cheap enough to do every frame. */
        const bool sel = (i == s_selected);
        lv_obj_set_style_bg_color(cell, sel ? COL_SEL_BG : COL_CAND_BG, 0);
        lv_obj_set_style_text_color(lbl, sel ? COL_SEL_TEXT : COL_TEXT, 0);
    }

    s_last_cand_count = s_cand_count;

    /* Only re-run flex-wrap layout if cell visibility / size actually
     * changed. Pure selection-only updates (e.g. DPAD navigation across
     * an unchanged candidate set) skip this entirely. */
    if (any_layout_change) {
        lv_obj_update_layout(s_cand_box);
    }

    /* Capture the laid-out positions so apply()'s Up/Down handler can
     * walk the visual grid. Positions are container-local. */
    s_cell_valid = s_cand_count;
    for (int i = 0; i < s_cand_count; ++i) {
        if (i < s_cand_count && s_cand_buf[i][0] != '\0') {
            s_cell_x[i] = (int16_t)lv_obj_get_x(s_cand_cells[i]);
            s_cell_y[i] = (int16_t)lv_obj_get_y(s_cand_cells[i]);
        } else {
            s_cell_x[i] = 0;
            s_cell_y[i] = INT16_MAX;
        }
    }

    if (s_selected >= 0 && s_selected < s_cand_count
        && s_cand_buf[s_selected][0] != '\0') {
        lv_obj_scroll_to_view(s_cand_cells[s_selected], LV_ANIM_OFF);
    }
}

void ime_view_refresh(void)
{
    uint32_t cur = __atomic_load_n(&g_ime_dirty_counter, __ATOMIC_ACQUIRE);
    if (cur == s_last_counter) return;

    TRACE("lvgl", "render_start", "cnt=%lu", (unsigned long)cur);

    if (!snapshot()) return;
    s_last_counter = cur;

    render_text();
    lv_label_set_text(s_mode_lbl, s_mode_buf);

    char page_buf[24];
    if (s_cand_total > 0) {
        snprintf(page_buf, sizeof page_buf, "%d/%d",
                 s_selected + 1, s_cand_total);
    } else {
        page_buf[0] = '\0';
    }
    lv_label_set_text(s_page_lbl, page_buf);

    render_candidates();

    TRACE("lvgl", "render_end", "n_cand=%d", s_cand_count);
}
