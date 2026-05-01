/* launcher_view.c — see launcher_view.h.
 *
 * 3 × 3 grid of app tiles. D-pad moves focus, OK launches. The "launch"
 * commit signals the modal back to the router with the chosen view id;
 * the router then `view_router_navigate()`s to it after modal_finish.
 *
 * Modal handshake: the caller (router) passes a `view_id_t *` ctx; this
 * view writes the chosen id into *ctx, sets `committed=true`, then the
 * caller observes the value and navigates. BACK leaves *ctx untouched
 * and signals committed=false — router stays on the calling view.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "launcher_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"

/* ── Grid model ──────────────────────────────────────────────────────── */

typedef struct {
    const char *label;
    view_id_t   target;        /* VIEW_ID_COUNT means placeholder slot */
} tile_t;

#define COLS         3
#define ROWS         3        /* viewport rows visible at once */
#define ROWS_TOTAL   4        /* total rows in the grid (scrollable) */

/* Slot order — matches docs/ui/01-page-architecture.md L-1 spec:
 *
 *    Msg(A)   Chan(B)   Nodes(C)
 *    Map(D)   Tele(F)   Tools(T)
 *    Set(S)   Me        Power
 *    HWDiag   —         —
 *
 * As each app lands, swap its target from VIEW_ID_COUNT to the new
 * view id. Placeholders:
 *   - Power → reserved for Z-1 SOS standby (waits on power button
 *     driver + low-batt state machine + IPC_CMD_SEND_SOS).
 *   - Row 4 col 2/3 → unassigned slots reserved for future apps.
 * Pressing OK on a placeholder shows a one-line "coming soon" toast
 * (handled by apply()) instead of silently exiting the launcher.
 *
 * Scroll model: only `ROWS` (3) rows are visible at any moment. The
 * viewport tops at `s_state.view_row` (0..ROWS_TOTAL-ROWS), and shifts
 * automatically when the focus hits the top/bottom of the visible area
 * and the user keeps moving in that direction. No scroll indicator
 * widget — the row swap is its own visual cue. */
typedef struct {
    const char *placeholder_msg;
} tile_meta_t;

/* const so it lives in .rodata (flash) — saves ~96 B SRAM .bss. */
static const tile_t s_tiles[ROWS_TOTAL * COLS] = {
    { "Msg",    VIEW_ID_MESSAGES }, { "Chan",  VIEW_ID_CHANNELS },  { "Nodes", VIEW_ID_NODES   },
    { "Map",    VIEW_ID_MAP      }, { "Tele",  VIEW_ID_TELEMETRY }, { "Tools", VIEW_ID_TOOLS   },
    { "Set",    VIEW_ID_SETTINGS }, { "Me",    VIEW_ID_MY_NODE },   { "Power", VIEW_ID_COUNT   },
    { "HWDiag", VIEW_ID_HW_DIAG  }, { "SysDiag", VIEW_ID_SYS_DIAG }, { "—",     VIEW_ID_COUNT   },
};

/* Per-tile placeholder reasons (only meaningful when target == VIEW_ID_COUNT). */
static const tile_meta_t s_tile_meta[ROWS_TOTAL * COLS] = {
    {NULL}, {NULL}, {NULL},
    {NULL}, {NULL}, {NULL},
    {NULL}, {NULL}, {"SOS app 規劃中 (待 power button + Z-1)"},
    {NULL}, {NULL}, {"預留位置"},
};

typedef struct {
    lv_obj_t *bg;
    lv_obj_t *cells[ROWS * COLS];        /* visible cells only — rebound on scroll */
    lv_obj_t *cell_lbls[ROWS * COLS];    /* labels inside each cell */
    lv_obj_t *toast_lbl;     /* one-line message under the grid */
    uint8_t   cur_row;       /* absolute row 0..ROWS_TOTAL-1 */
    uint8_t   cur_col;
    uint8_t   view_row;      /* topmost visible row; 0..ROWS_TOTAL-ROWS */
} launcher_t;

/* Lives in PSRAM .bss (no SWD reads / no early-boot use; touched only
 * by lv tasks once view is created). Saves ~84 B SRAM .bss. */
static launcher_t s __attribute__((section(".psram_bss")));

/* SWD-readable mirrors of focus state. Needed because s lives in
 * PSRAM (cached, not SWD-coherent without flush). 3 bytes total —
 * cheap. Updated whenever paint_all() / paint_focus_only() runs. */
volatile uint8_t g_launcher_cur_row  __attribute__((used)) = 0;
volatile uint8_t g_launcher_cur_col  __attribute__((used)) = 0;
volatile uint8_t g_launcher_view_row __attribute__((used)) = 0;

/* ── Style helpers ───────────────────────────────────────────────────── */

/* Repaint one visible cell's border / label-color to reflect both its
 * current absolute tile (after scroll) and whether it has focus. */
static void paint_cell(int viewport_row, int col)
{
    int idx = viewport_row * COLS + col;
    lv_obj_t *cell = s.cells[idx];
    lv_obj_t *lbl  = s.cell_lbls[idx];
    if (!cell || !lbl) return;

    int abs_row = (int)s.view_row + viewport_row;
    int tile_idx = abs_row * COLS + col;
    bool focus = (abs_row == s.cur_row && col == s.cur_col);

    lv_label_set_text(lbl, s_tiles[tile_idx].label);
    lv_obj_set_style_text_color(lbl,
        s_tiles[tile_idx].target == VIEW_ID_COUNT
            ? ui_color(UI_COLOR_TEXT_SECONDARY)
            : ui_color(UI_COLOR_TEXT_PRIMARY), 0);

    lv_obj_set_style_border_color(cell,
        focus ? ui_color(UI_COLOR_ACCENT_FOCUS)
              : ui_color(UI_COLOR_BORDER_NORMAL), 0);
    lv_obj_set_style_border_width(cell, focus ? 2 : 1, 0);
}

static void paint_all(void)
{
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            paint_cell(r, c);
        }
    }
}

/* Repaint just the focus border for a single absolute tile and its
 * (possibly different) old position. Cheaper than paint_all when the
 * viewport hasn't shifted. */
static void paint_focus_only(int old_abs_row, int old_col,
                             int new_abs_row, int new_col)
{
    int old_vp = old_abs_row - (int)s.view_row;
    int new_vp = new_abs_row - (int)s.view_row;
    if (old_vp >= 0 && old_vp < ROWS) paint_cell(old_vp, old_col);
    if (new_vp >= 0 && new_vp < ROWS) paint_cell(new_vp, new_col);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    /* Tile grid layout. Panel is 320 × 224. Tiles 96×60 with 4 px gap.
     *   total w = 96*3 + 4*2 = 296 → x0 = (320-296)/2 = 12
     *   total h = 60*3 + 4*2 = 188 → y0 = (224-188)/2 = 18
     *
     * Only ROWS (3) viewport rows are created — scroll is implemented
     * by rebinding labels to different absolute tile indices in
     * paint_cell(), not by creating/destroying widgets. */
    const int TW = 96, TH = 60, GX = 4, GY = 4;
    const int X0 = 12, Y0 = 18;

    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            int idx = r * COLS + c;
            lv_obj_t *cell = lv_obj_create(panel);
            lv_obj_set_pos(cell, X0 + c * (TW + GX), Y0 + r * (TH + GY));
            lv_obj_set_size(cell, TW, TH);
            lv_obj_set_style_bg_color(cell, ui_color(UI_COLOR_BG_SECONDARY), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, 4, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            lv_obj_set_style_text_font(lbl, ui_font_sm16(), 0);
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

            s.cells[idx] = cell;
            s.cell_lbls[idx] = lbl;
        }
    }

    /* Toast label under the grid (Y0+188+4 = 210 .. 224). Empty by
     * default; set by apply() when user OKs a placeholder tile. */
    s.toast_lbl = lv_label_create(panel);
    lv_obj_set_pos(s.toast_lbl, 4, 210);
    lv_obj_set_size(s.toast_lbl, 312, 14);
    lv_obj_set_style_text_font(s.toast_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.toast_lbl,
                                ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.toast_lbl, 0, 0);
    lv_label_set_long_mode(s.toast_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s.toast_lbl, "");

    paint_all();
    g_launcher_cur_row  = s.cur_row;
    g_launcher_cur_col  = s.cur_col;
    g_launcher_view_row = s.view_row;
}

static void destroy(void)
{
    memset(s.cells, 0, sizeof(s.cells));
    memset(s.cell_lbls, 0, sizeof(s.cell_lbls));
    s.toast_lbl = NULL;
}

static void clear_toast_when_cursor_moves(void)
{
    if (s.toast_lbl) lv_label_set_text(s.toast_lbl, "");
}

static void show_placeholder_toast(uint8_t idx)
{
    if (s.toast_lbl == NULL) return;
    const char *msg = s_tile_meta[idx].placeholder_msg;
    if (msg == NULL) msg = "尚未實作";
    lv_label_set_text(s.toast_lbl, msg);
}

/* Move focus by (drow, dcol) absolute. Auto-shifts the viewport if the
 * focus would land outside it. Returns true if any visible state
 * changed (so caller can suppress redundant repaints). */
static bool move_focus(int drow, int dcol)
{
    int nr = (int)s.cur_row + drow;
    int nc = (int)s.cur_col + dcol;
    if (nr < 0 || nr >= ROWS_TOTAL) return false;
    if (nc < 0 || nc >= COLS)        return false;

    int old_row = s.cur_row, old_col = s.cur_col;
    s.cur_row = (uint8_t)nr;
    s.cur_col = (uint8_t)nc;

    /* Adjust viewport so the new focus row stays visible. */
    uint8_t new_view = s.view_row;
    if (nr < (int)s.view_row)             new_view = (uint8_t)nr;
    else if (nr >= (int)s.view_row + ROWS) new_view = (uint8_t)(nr - ROWS + 1);

    if (new_view != s.view_row) {
        s.view_row = new_view;
        paint_all();      /* every visible cell remaps to a new tile */
    } else {
        paint_focus_only(old_row, old_col, nr, nc);
    }
    /* Mirror state to .bss for SWD inspection. */
    g_launcher_cur_row  = s.cur_row;
    g_launcher_cur_col  = s.cur_col;
    g_launcher_view_row = s.view_row;
    clear_toast_when_cursor_moves();
    return true;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    switch (ev->keycode) {
        case MOKYA_KEY_UP:    (void)move_focus(-1,  0); break;
        case MOKYA_KEY_DOWN:  (void)move_focus(+1,  0); break;
        case MOKYA_KEY_LEFT:  (void)move_focus( 0, -1); break;
        case MOKYA_KEY_RIGHT: (void)move_focus( 0, +1); break;
        case MOKYA_KEY_OK: {
            /* Reachable here only when view_router falls through (target
             * is a placeholder). Real targets are intercepted by the
             * router and dispatched via launcher_view_picked + modal_finish. */
            int idx = (int)s.cur_row * COLS + (int)s.cur_col;
            show_placeholder_toast((uint8_t)idx);
            break;
        }
        default: break;
    }
}

static void refresh(void) {}

static const view_descriptor_t LAUNCHER_DESC = {
    .id      = VIEW_ID_LAUNCHER,
    .name    = "launcher",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "DPad pick", "OK launch", "BACK cancel" },
};

const view_descriptor_t *launcher_view_descriptor(void)
{
    return &LAUNCHER_DESC;
}

/* Read accessor for the router so it can navigate after modal_finish. */
view_id_t launcher_view_picked(void)
{
    int idx = (int)s.cur_row * COLS + (int)s.cur_col;
    return s_tiles[idx].target;
}
