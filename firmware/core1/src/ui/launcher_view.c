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

#define COLS 3
#define ROWS 3

/* Slot order — matches docs/ui/01-page-architecture.md L-1 spec:
 *
 *    Msg(A)  Chan(B)  Nodes(C)
 *    Map(D)  Tele(F)  Tools(T)
 *    Set(S)  Admin    Power
 *
 * Active in this build: Msg, Nodes, Tools, Set. The other five are
 * spec-named placeholders so the user can see what's coming and the
 * 9-grid layout looks complete. Placeholders render dimmed and OK on
 * them is a no-op. As each app lands, swap its target from
 * VIEW_ID_COUNT to the new view id. */
static tile_t s_tiles[ROWS * COLS] = {
    { "Msg",    VIEW_ID_MESSAGES }, { "Chan",  VIEW_ID_COUNT },     { "Nodes", VIEW_ID_NODES   },
    { "Map",    VIEW_ID_COUNT    }, { "Tele",  VIEW_ID_COUNT },     { "Tools", VIEW_ID_TOOLS   },
    { "Set",    VIEW_ID_SETTINGS }, { "Me",    VIEW_ID_MY_NODE },   { "Power", VIEW_ID_COUNT   },
};

typedef struct {
    lv_obj_t *bg;
    lv_obj_t *cells[ROWS * COLS];
    uint8_t   cur_row;
    uint8_t   cur_col;
} launcher_t;

static launcher_t s;

/* ── Style helpers ───────────────────────────────────────────────────── */

static void paint_focus(void)
{
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            lv_obj_t *cell = s.cells[r * COLS + c];
            if (!cell) continue;
            bool focus = (r == s.cur_row && c == s.cur_col);
            lv_obj_set_style_border_color(cell,
                focus ? ui_color(UI_COLOR_ACCENT_FOCUS)
                      : ui_color(UI_COLOR_BORDER_NORMAL), 0);
            lv_obj_set_style_border_width(cell, focus ? 2 : 1, 0);
        }
    }
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
     */
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
            lv_obj_set_style_border_color(cell, ui_color(UI_COLOR_BORDER_NORMAL), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_radius(cell, 4, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            lv_obj_set_style_text_font(lbl, ui_font_sm16(), 0);
            lv_obj_set_style_text_color(lbl,
                s_tiles[idx].target == VIEW_ID_COUNT
                    ? ui_color(UI_COLOR_TEXT_SECONDARY)
                    : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
            lv_label_set_text(lbl, s_tiles[idx].label);
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

            s.cells[idx] = cell;
        }
    }

    paint_focus();
}

static void destroy(void)
{
    memset(s.cells, 0, sizeof(s.cells));
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cur_row > 0) { s.cur_row--; paint_focus(); }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cur_row + 1 < ROWS) { s.cur_row++; paint_focus(); }
            break;
        case MOKYA_KEY_LEFT:
            if (s.cur_col > 0) { s.cur_col--; paint_focus(); }
            break;
        case MOKYA_KEY_RIGHT:
            if (s.cur_col + 1 < COLS) { s.cur_col++; paint_focus(); }
            break;
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
    int idx = s.cur_row * COLS + s.cur_col;
    return s_tiles[idx].target;
}
