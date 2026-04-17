/* keypad_view.c — see keypad_view.h for design notes.
 *
 * Layout (landscape 320x240) mirroring the physical PCB:
 *
 *   y  0..13    status strip  ("LAST: XXX"  /  "p=N d=N r=N")
 *   y 14..112   upper controls:
 *                 left column    — FUNC (top), BACK (bottom)
 *                 centre DPAD    — UP / LEFT / OK / RIGHT / DOWN (+ cross)
 *                 right 2x2      — SET V+ / DEL V- (column-major)
 *   y 113..119  separator
 *   y 120..239  5x5 half-keyboard
 *                 row 0: 1/2  3/4  5/6  7/8  9/0
 *                 row 1: Q/W  E/R  T/Y  U/I  O/P
 *                 row 2: A/S  D/F  G/H  J/K  L
 *                 row 3: Z/X  C/V  B/N  M    \
 *                 row 4: MOD  TAB  SPC  ,    .
 *
 * Every cell is an independent lv_obj keyed on mokya_keycode_t — we do not
 * use g_keymap here because the scan-order matrix does not match the
 * physical arrangement the user wants to see.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "keypad_view.h"

#include <stdio.h>

#include "key_event.h"
#include "key_name.h"
#include "mie/keycode.h"

/* ── Layout constants ────────────────────────────────────────────────── */
#define SCREEN_W            320
#define SCREEN_H            240

#define STATUS_H            14
#define TOP_Y               (STATUS_H)         /* 14 */
#define TOP_H               98                 /* 14..111 */
#define BOTTOM_Y            120
#define BOTTOM_H            (SCREEN_H - BOTTOM_Y)  /* 120 */

/* 5x5 grid */
#define GRID_COLS           5
#define GRID_ROWS           5
#define GRID_CELL_W         58
#define GRID_CELL_H         20
#define GRID_GAP            4
/* Total width  = 5*58 + 4*4 = 290 + 16 = 306 → side margin 7    */
/* Total height = 5*20 + 4*4 =  100+ 16 = 116 → top margin 2     */
#define GRID_X0             7
#define GRID_Y0             (BOTTOM_Y + 2)

/* Upper-left column (FUNC / BACK) */
#define LEFT_COL_X          6
#define LEFT_COL_W          54
#define LEFT_COL_H          42
#define LEFT_COL_GAP        8

/* Upper-right 2x2 — column-major (SET,DEL) | (V+,V-) */
#define RIGHT_CELL_W        54
#define RIGHT_CELL_H        42
#define RIGHT_GAP_X         4
#define RIGHT_GAP_Y         8
#define RIGHT_X0            (SCREEN_W - 6 - RIGHT_CELL_W * 2 - RIGHT_GAP_X)

/* DPAD — cross shape centred horizontally between the two side groups. */
#define DPAD_CELL           30
#define DPAD_CX             160
#define DPAD_CY             (TOP_Y + TOP_H / 2)  /* ≈ 63 */

/* ── Colour palette ──────────────────────────────────────────────────── */
static const lv_color_t COLOUR_BG      = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t COLOUR_IDLE    = LV_COLOR_MAKE(0x30, 0x30, 0x30);
static const lv_color_t COLOUR_PRESSED = LV_COLOR_MAKE(0x20, 0xC0, 0x20);
static const lv_color_t COLOUR_TEXT    = LV_COLOR_MAKE(0xF0, 0xF0, 0xF0);

/* ── Cell storage — one entry per keycode ────────────────────────────── */
static lv_obj_t *s_cells[MOKYA_KEY_LIMIT];
static lv_color_t s_cell_idle[MOKYA_KEY_LIMIT];

static lv_obj_t *s_header_label;
static lv_obj_t *s_footer_label;

/* ── Widget helpers ──────────────────────────────────────────────────── */
static void style_cell(lv_obj_t *cell, lv_color_t bg)
{
    lv_obj_set_style_bg_color(cell, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 3, LV_PART_MAIN);
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
    style_cell(cell, idle);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cell);
    lv_label_set_text(lbl, key_name_short(kc));
    lv_obj_center(lbl);

    s_cells[kc] = cell;
    s_cell_idle[kc] = idle;
    return cell;
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
    /* Zero reverse map */
    for (uint16_t i = 0; i < MOKYA_KEY_LIMIT; ++i) {
        s_cells[i] = NULL;
    }

    /* Background */
    lv_obj_set_style_bg_color(parent, COLOUR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(parent, COLOUR_TEXT, LV_PART_MAIN);

    /* Status strip */
    s_header_label = lv_label_create(parent);
    lv_obj_set_pos(s_header_label, 4, 0);
    lv_label_set_text(s_header_label, "LAST: ---");

    s_footer_label = lv_label_create(parent);
    lv_obj_set_pos(s_footer_label, SCREEN_W / 2 + 10, 0);
    lv_label_set_text(s_footer_label, "p=0 d=0 r=0");

    /* Upper-left column — FUNC, BACK */
    make_cell(parent, MOKYA_KEY_FUNC,
              LEFT_COL_X, TOP_Y + 4,
              LEFT_COL_W, LEFT_COL_H, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_BACK,
              LEFT_COL_X, TOP_Y + 4 + LEFT_COL_H + LEFT_COL_GAP,
              LEFT_COL_W, LEFT_COL_H, COLOUR_IDLE);

    /* Upper-right 2x2 — left col SET(top)/DEL(bot), right col V+(top)/V-(bot) */
    int rx0 = RIGHT_X0;
    int rx1 = rx0 + RIGHT_CELL_W + RIGHT_GAP_X;
    int ry0 = TOP_Y + 4;
    int ry1 = ry0 + RIGHT_CELL_H + RIGHT_GAP_Y;
    make_cell(parent, MOKYA_KEY_SET,      rx0, ry0, RIGHT_CELL_W, RIGHT_CELL_H, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_DEL,      rx0, ry1, RIGHT_CELL_W, RIGHT_CELL_H, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_VOL_UP,   rx1, ry0, RIGHT_CELL_W, RIGHT_CELL_H, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_VOL_DOWN, rx1, ry1, RIGHT_CELL_W, RIGHT_CELL_H, COLOUR_IDLE);

    /* Centre DPAD — cross around (DPAD_CX, DPAD_CY) */
    int dc = DPAD_CELL;
    int dhalf = dc / 2;
    make_cell(parent, MOKYA_KEY_UP,
              DPAD_CX - dhalf, DPAD_CY - dhalf - dc - 2,
              dc, dc, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_LEFT,
              DPAD_CX - dhalf - dc - 2, DPAD_CY - dhalf,
              dc, dc, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_OK,
              DPAD_CX - dhalf, DPAD_CY - dhalf,
              dc, dc, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_RIGHT,
              DPAD_CX + dhalf + 2, DPAD_CY - dhalf,
              dc, dc, COLOUR_IDLE);
    make_cell(parent, MOKYA_KEY_DOWN,
              DPAD_CX - dhalf, DPAD_CY + dhalf + 2,
              dc, dc, COLOUR_IDLE);

    /* 5x5 lower half-keyboard */
    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int c = 0; c < GRID_COLS; ++c) {
            mokya_keycode_t kc = s_grid_layout[r][c];
            make_cell(parent, kc,
                      GRID_X0 + c * (GRID_CELL_W + GRID_GAP),
                      GRID_Y0 + r * (GRID_CELL_H + GRID_GAP),
                      GRID_CELL_W, GRID_CELL_H, COLOUR_IDLE);
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
        return; /* key not represented in this view (e.g. POWER) */
    }

    style_cell(cell,
               ev->pressed ? COLOUR_PRESSED : s_cell_idle[ev->keycode]);

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
    }
}
