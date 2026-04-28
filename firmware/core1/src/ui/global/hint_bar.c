/* hint_bar.c — see hint_bar.h.
 *
 * Three labels (left / centre / right) on a 320 × 16 strip. Hidden by
 * default; views opt-in via hint_bar_set(). Text is dim (secondary
 * colour) so it doesn't compete with view content.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hint_bar.h"
#include "ui_theme.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *left_lbl;
    lv_obj_t *ok_lbl;
    lv_obj_t *right_lbl;
} hint_bar_t;

static hint_bar_t s;

static lv_obj_t *make_label(lv_obj_t *parent, lv_align_t align, int x_off)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, x_off, 0);
    return l;
}

void hint_bar_init(lv_obj_t *screen)
{
    memset(&s, 0, sizeof(s));

    s.bar = lv_obj_create(screen);
    lv_obj_set_pos(s.bar, 0, 240 - HINT_BAR_HEIGHT);
    lv_obj_set_size(s.bar, 320, HINT_BAR_HEIGHT);
    lv_obj_set_style_bg_color(s.bar, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s.bar, 0, 0);
    lv_obj_set_style_pad_all(s.bar, 0, 0);
    lv_obj_set_style_radius(s.bar, 0, 0);
    lv_obj_clear_flag(s.bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.bar, LV_OBJ_FLAG_HIDDEN);

    s.left_lbl  = make_label(s.bar, LV_ALIGN_LEFT_MID,   4);
    s.ok_lbl    = make_label(s.bar, LV_ALIGN_CENTER,     0);
    s.right_lbl = make_label(s.bar, LV_ALIGN_RIGHT_MID, -4);
}

void hint_bar_set(const char *left, const char *ok, const char *right)
{
    if (s.bar == NULL) return;
    lv_label_set_text(s.left_lbl,  left  ? left  : "");
    lv_label_set_text(s.ok_lbl,    ok    ? ok    : "");
    lv_label_set_text(s.right_lbl, right ? right : "");
    bool any = (left && *left) || (ok && *ok) || (right && *right);
    if (any) lv_obj_clear_flag(s.bar, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(s.bar, LV_OBJ_FLAG_HIDDEN);
}

void hint_bar_clear(void)
{
    if (s.bar == NULL) return;
    lv_label_set_text(s.left_lbl,  "");
    lv_label_set_text(s.ok_lbl,    "");
    lv_label_set_text(s.right_lbl, "");
    lv_obj_add_flag(s.bar, LV_OBJ_FLAG_HIDDEN);
}

void hint_bar_set_visible(bool visible)
{
    if (s.bar == NULL) return;
    if (visible) lv_obj_clear_flag(s.bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s.bar, LV_OBJ_FLAG_HIDDEN);
}
