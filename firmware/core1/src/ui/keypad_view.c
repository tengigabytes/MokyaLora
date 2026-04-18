/* keypad_view.c — see keypad_view.h for design notes.
 *
 * Landscape 320x240 layout, grouped by colour family:
 *
 *   y  0..13    status strip (LAST | counters)  ── with 1 px divider ──
 *   y 16..108   upper controls (92 px tall):
 *                 x  6.. 60  FUNC (top) / BACK (bottom)   [blue-dark]
 *                 x 83..179  DPAD cross (UP/LF/OK/RT/DN)  [amber-dark]
 *                 x 202..314 SET V+ / DEL V- (2x2)        [magenta/warm]
 *   y 110..112  horizontal divider
 *   y 114..238  5x5 half-keyboard (alpha/number)          [slate]
 *
 * Press feedback is a unified cyan accent across every group so the eye
 * can track the last-pressed key regardless of section.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "keypad_view.h"

#include <stdio.h>

#include "key_event.h"
#include "key_name.h"
#include "mie/keycode.h"

/* ── Screen ──────────────────────────────────────────────────────────── */
#define SCREEN_W            320
#define SCREEN_H            240

/* ── Status strip ────────────────────────────────────────────────────── */
#define STATUS_H            14
#define DIVIDER_UPPER_Y     14
#define DIVIDER_LOWER_Y     111

/* ── Upper zone (y 16..108, 92 px) ───────────────────────────────────── */
#define UPPER_Y             16
#define UPPER_H             92

/* Left column (FUNC / BACK) */
#define LEFT_X              6
#define LEFT_W              54
#define SIDE_CELL_H         42
#define SIDE_GAP_Y          8     /* 42 + 8 + 42 = 92  → fills upper zone */

/* Right 2x2 (SET/DEL | V+/V-) — column-major */
#define RIGHT_CELL_W        54
#define RIGHT_GAP_X         4
#define RIGHT_X0            (SCREEN_W - 6 - RIGHT_CELL_W * 2 - RIGHT_GAP_X)  /* 202 */
#define RIGHT_X1            (RIGHT_X0 + RIGHT_CELL_W + RIGHT_GAP_X)          /* 260 */

/* DPAD — cross between left column (ends x=60) and right 2x2 (starts x=202).
 * Usable band 66..196 → centre 131. Cell 28, gap 2 → cross span 88.
 *   LEFT x  87..115    UP/OK/DOWN x 117..145    RIGHT x 147..175
 * Horizontal clearance to neighbours ≥ 27 px on each side. */
#define DPAD_CELL           28
#define DPAD_GAP            2
#define DPAD_CX             131
#define DPAD_CY             (UPPER_Y + UPPER_H / 2)   /* 62 */

/* ── Lower zone (y 114..238, 124 px) — 5x5 half-keyboard ─────────────── */
#define GRID_COLS           5
#define GRID_ROWS           5
#define GRID_CELL_W         58
#define GRID_CELL_H         22
#define GRID_GAP_X          4
#define GRID_GAP_Y          3
/* width  = 5*58 + 4*4 = 306 → side margin (320-306)/2 = 7
 * height = 5*22 + 4*3 = 122 → fits 124 px band with 2 px margin */
#define GRID_X0             7
#define GRID_Y0             116

/* ── Palette ─────────────────────────────────────────────────────────── */
/* Dark navy background — gentler than pure black on IPS, preserves contrast */
static const lv_color_t COLOUR_BG         = LV_COLOR_MAKE(0x0B, 0x0F, 0x14);
static const lv_color_t COLOUR_DIVIDER    = LV_COLOR_MAKE(0x1E, 0x29, 0x3B);
static const lv_color_t COLOUR_BORDER     = LV_COLOR_MAKE(0x2D, 0x3A, 0x4E);
static const lv_color_t COLOUR_TEXT       = LV_COLOR_MAKE(0xE5, 0xE7, 0xEB);
static const lv_color_t COLOUR_TEXT_DIM   = LV_COLOR_MAKE(0x94, 0xA3, 0xB8);

/* Group idle fills — each group has its own hue so press feedback is readable
 * even when several keys flash in sequence. Luminance kept low (~15%) so the
 * accent colour pops clearly on press. */
static const lv_color_t COLOUR_ALPHA_IDLE = LV_COLOR_MAKE(0x1F, 0x29, 0x37);  /* slate  */
static const lv_color_t COLOUR_DPAD_IDLE  = LV_COLOR_MAKE(0x2D, 0x24, 0x18);  /* amber-dark */
static const lv_color_t COLOUR_OK_IDLE    = LV_COLOR_MAKE(0x5A, 0x40, 0x18);  /* amber stronger */
static const lv_color_t COLOUR_NAV_IDLE   = LV_COLOR_MAKE(0x1A, 0x2A, 0x3A);  /* blue-dark  (FUNC/BACK) */
static const lv_color_t COLOUR_EDIT_IDLE  = LV_COLOR_MAKE(0x2A, 0x1A, 0x2A);  /* magenta-dark (SET/DEL) */
static const lv_color_t COLOUR_VOL_IDLE   = LV_COLOR_MAKE(0x2A, 0x1F, 0x15);  /* warm-dark (V+/V-) */

/* Unified press accent — cyan reads cleanly against every idle hue */
static const lv_color_t COLOUR_PRESSED    = LV_COLOR_MAKE(0x22, 0xD3, 0xEE);
static const lv_color_t COLOUR_PRESSED_BR = LV_COLOR_MAKE(0x67, 0xE8, 0xF9);

/* ── Cell storage ────────────────────────────────────────────────────── */
static lv_obj_t *s_cells[MOKYA_KEY_LIMIT];
static lv_color_t s_cell_idle[MOKYA_KEY_LIMIT];

static lv_obj_t *s_header_label;
static lv_obj_t *s_footer_label;

/* ── Widget helpers ──────────────────────────────────────────────────── */
static void style_cell(lv_obj_t *cell, lv_color_t bg, bool pressed)
{
    lv_obj_set_style_bg_color(cell, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell,
        pressed ? COLOUR_PRESSED_BR : COLOUR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
}

static lv_obj_t *make_cell(lv_obj_t *parent,
                           mokya_keycode_t kc,
                           int16_t x, int16_t y,
                           int16_t w, int16_t h,
                           lv_color_t idle)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_pos(cell, x, y);
    style_cell(cell, idle, false);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cell);
    lv_label_set_text(lbl, key_name_short(kc));
    lv_obj_set_style_text_color(lbl, COLOUR_TEXT, LV_PART_MAIN);
    lv_obj_center(lbl);

    s_cells[kc] = cell;
    s_cell_idle[kc] = idle;
    return cell;
}

static void make_divider(lv_obj_t *parent, int16_t y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, SCREEN_W, 1);
    lv_obj_set_pos(line, 0, y);
    lv_obj_set_style_bg_color(line, COLOUR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

/* Physical 5x5 half-keyboard, top-to-bottom. */
static const mokya_keycode_t s_grid_layout[GRID_ROWS][GRID_COLS] = {
    { MOKYA_KEY_1,    MOKYA_KEY_3,   MOKYA_KEY_5,     MOKYA_KEY_7,    MOKYA_KEY_9         },
    { MOKYA_KEY_Q,    MOKYA_KEY_E,   MOKYA_KEY_T,     MOKYA_KEY_U,    MOKYA_KEY_O         },
    { MOKYA_KEY_A,    MOKYA_KEY_D,   MOKYA_KEY_G,     MOKYA_KEY_J,    MOKYA_KEY_L         },
    { MOKYA_KEY_Z,    MOKYA_KEY_C,   MOKYA_KEY_B,     MOKYA_KEY_M,    MOKYA_KEY_BACKSLASH },
    { MOKYA_KEY_MODE, MOKYA_KEY_TAB, MOKYA_KEY_SPACE, MOKYA_KEY_SYM1, MOKYA_KEY_SYM2      },
};

void keypad_view_init(lv_obj_t *parent)
{
    for (uint16_t i = 0; i < MOKYA_KEY_LIMIT; ++i) {
        s_cells[i] = NULL;
    }

    /* Background */
    lv_obj_set_style_bg_color(parent, COLOUR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(parent, COLOUR_TEXT, LV_PART_MAIN);

    /* Status strip — LAST on the left, counters right-aligned */
    s_header_label = lv_label_create(parent);
    lv_obj_set_pos(s_header_label, 6, 0);
    lv_obj_set_style_text_color(s_header_label, COLOUR_TEXT, LV_PART_MAIN);
    lv_label_set_text(s_header_label, "LAST: ---");

    s_footer_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_footer_label, COLOUR_TEXT_DIM, LV_PART_MAIN);
    lv_label_set_text(s_footer_label, "p=0 d=0 r=0");
    lv_obj_align(s_footer_label, LV_ALIGN_TOP_RIGHT, -6, 0);

    /* Dividers */
    make_divider(parent, DIVIDER_UPPER_Y);
    make_divider(parent, DIVIDER_LOWER_Y);

    /* Upper-left column — FUNC, BACK (blue-dark) */
    make_cell(parent, MOKYA_KEY_FUNC,
              LEFT_X, UPPER_Y,
              LEFT_W, SIDE_CELL_H, COLOUR_NAV_IDLE);
    make_cell(parent, MOKYA_KEY_BACK,
              LEFT_X, UPPER_Y + SIDE_CELL_H + SIDE_GAP_Y,
              LEFT_W, SIDE_CELL_H, COLOUR_NAV_IDLE);

    /* Upper-right 2x2 — left col SET/DEL (magenta), right col V+/V- (warm) */
    int ry0 = UPPER_Y;
    int ry1 = UPPER_Y + SIDE_CELL_H + SIDE_GAP_Y;
    make_cell(parent, MOKYA_KEY_SET,      RIGHT_X0, ry0, RIGHT_CELL_W, SIDE_CELL_H, COLOUR_EDIT_IDLE);
    make_cell(parent, MOKYA_KEY_DEL,      RIGHT_X0, ry1, RIGHT_CELL_W, SIDE_CELL_H, COLOUR_EDIT_IDLE);
    make_cell(parent, MOKYA_KEY_VOL_UP,   RIGHT_X1, ry0, RIGHT_CELL_W, SIDE_CELL_H, COLOUR_VOL_IDLE);
    make_cell(parent, MOKYA_KEY_VOL_DOWN, RIGHT_X1, ry1, RIGHT_CELL_W, SIDE_CELL_H, COLOUR_VOL_IDLE);

    /* Centre DPAD — cross around (DPAD_CX, DPAD_CY); OK cell brighter */
    int dc = DPAD_CELL;
    int dhalf = dc / 2;
    int step = dc + DPAD_GAP;
    make_cell(parent, MOKYA_KEY_UP,
              DPAD_CX - dhalf, DPAD_CY - dhalf - step,
              dc, dc, COLOUR_DPAD_IDLE);
    make_cell(parent, MOKYA_KEY_LEFT,
              DPAD_CX - dhalf - step, DPAD_CY - dhalf,
              dc, dc, COLOUR_DPAD_IDLE);
    make_cell(parent, MOKYA_KEY_OK,
              DPAD_CX - dhalf, DPAD_CY - dhalf,
              dc, dc, COLOUR_OK_IDLE);
    make_cell(parent, MOKYA_KEY_RIGHT,
              DPAD_CX + dhalf + DPAD_GAP, DPAD_CY - dhalf,
              dc, dc, COLOUR_DPAD_IDLE);
    make_cell(parent, MOKYA_KEY_DOWN,
              DPAD_CX - dhalf, DPAD_CY + dhalf + DPAD_GAP,
              dc, dc, COLOUR_DPAD_IDLE);

    /* 5x5 lower half-keyboard (slate) */
    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int c = 0; c < GRID_COLS; ++c) {
            mokya_keycode_t kc = s_grid_layout[r][c];
            make_cell(parent, kc,
                      GRID_X0 + c * (GRID_CELL_W + GRID_GAP_X),
                      GRID_Y0 + r * (GRID_CELL_H + GRID_GAP_Y),
                      GRID_CELL_W, GRID_CELL_H, COLOUR_ALPHA_IDLE);
        }
    }
}

static void apply_event(const key_event_t *ev)
{
    if (ev->keycode == MOKYA_KEY_NONE || ev->keycode >= MOKYA_KEY_LIMIT) {
        return;
    }
    lv_obj_t *cell = s_cells[ev->keycode];
    if (cell == NULL) {
        return;
    }

    style_cell(cell,
               ev->pressed ? COLOUR_PRESSED : s_cell_idle[ev->keycode],
               ev->pressed);

    char buf[24];
    snprintf(buf, sizeof(buf), "LAST: %s %s",
             key_name_short(ev->keycode),
             ev->pressed ? "P" : "R");
    lv_label_set_text(s_header_label, buf);
}

void keypad_view_tick(void)
{
    key_event_t ev;
    bool any = false;
    while (key_event_pop(&ev, 0)) {
        apply_event(&ev);
        any = true;
    }

    if (any) {
        char buf[32];
        snprintf(buf, sizeof(buf), "p=%lu d=%lu r=%lu",
                 (unsigned long)g_key_event_pushed,
                 (unsigned long)g_key_event_dropped,
                 (unsigned long)g_key_event_rejected);
        lv_label_set_text(s_footer_label, buf);
        lv_obj_align(s_footer_label, LV_ALIGN_TOP_RIGHT, -6, 0);
    }
}
