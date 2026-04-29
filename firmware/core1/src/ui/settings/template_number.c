/* template_number.c — see template_number.h.
 *
 * Layout (within parent panel, anchored below settings_app_view's
 * breadcrumb at y=0..15):
 *
 *   y  20.. 39  title (key label)
 *   y  60.. 99  big-font value display "[ 22 ]" centered
 *   y 110..127  range hint  "Range: min..max"
 *   y 130..147  current     "Now: <initial>"
 *   y 170..213  instruction footer (D-pad mapping)
 *
 * Spec §50 模板 B mapping:
 *   UP    +1
 *   DOWN  -1
 *   RIGHT +10
 *   LEFT  -10
 *   OK    apply, BACK cancel
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "template_number.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "mie/keycode.h"

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *title_lbl;
    lv_obj_t *value_lbl;
    lv_obj_t *range_lbl;
    lv_obj_t *now_lbl;
    lv_obj_t *foot_lbl;

    const settings_key_def_t *key;
    int32_t initial;
    int32_t edit;
    int32_t lo;
    int32_t hi;
    bool    done;
    bool    committed;
} tn_state_t;

static tn_state_t s;

/* ── Render ─────────────────────────────────────────────────────────── */

static void clamp_edit(void)
{
    if (s.edit < s.lo) s.edit = s.lo;
    if (s.edit > s.hi) s.edit = s.hi;
}

static void render_value(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "[ %ld ]", (long)s.edit);
    lv_label_set_text(s.value_lbl, buf);
}

static void render_now(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Now: %ld   New: %ld",
             (long)s.initial, (long)s.edit);
    lv_label_set_text(s.now_lbl, buf);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t col, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void template_number_open(lv_obj_t *parent,
                          const settings_key_def_t *key,
                          int32_t current)
{
    memset(&s, 0, sizeof(s));
    s.key     = key;
    s.lo      = key ? key->min : INT32_MIN;
    s.hi      = key ? key->max : INT32_MAX;
    s.initial = current;
    s.edit    = current;
    clamp_edit();

    lv_color_t white = ui_color(UI_COLOR_TEXT_PRIMARY);
    lv_color_t dim   = ui_color(UI_COLOR_TEXT_SECONDARY);
    lv_color_t fg    = ui_color(UI_COLOR_ACCENT_FOCUS);

    s.title_lbl = make_label(parent, 4, 20, 320 - 8, 20, white,
                             LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s.title_lbl,
                      key && key->label ? key->label : "(setting)");

    s.value_lbl = make_label(parent, 4, 60, 320 - 8, 40, fg,
                             LV_TEXT_ALIGN_CENTER);
    render_value();

    char rbuf[64];
    snprintf(rbuf, sizeof(rbuf), "Range: %ld..%ld", (long)s.lo, (long)s.hi);
    s.range_lbl = make_label(parent, 4, 110, 320 - 8, 18, dim,
                             LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s.range_lbl, rbuf);

    s.now_lbl = make_label(parent, 4, 130, 320 - 8, 18, dim,
                           LV_TEXT_ALIGN_CENTER);
    render_now();

    s.foot_lbl = make_label(parent, 4, 170, 320 - 8, 44, dim,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s.foot_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s.foot_lbl,
                      "UP/DOWN +/- 1   LEFT/RIGHT +/- 10\n"
                      "OK applies   BACK cancels");
}

void template_number_close(void)
{
    if (s.title_lbl) lv_obj_del(s.title_lbl);
    if (s.value_lbl) lv_obj_del(s.value_lbl);
    if (s.range_lbl) lv_obj_del(s.range_lbl);
    if (s.now_lbl)   lv_obj_del(s.now_lbl);
    if (s.foot_lbl)  lv_obj_del(s.foot_lbl);
    memset(&s, 0, sizeof(s));
}

static void step(int32_t delta)
{
    int32_t prev = s.edit;
    s.edit += delta;
    clamp_edit();
    if (s.edit != prev) {
        render_value();
        render_now();
    }
}

bool template_number_apply_key(const key_event_t *ev)
{
    if (!ev->pressed) return false;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:    step(+1);  return true;
        case MOKYA_KEY_DOWN:  step(-1);  return true;
        case MOKYA_KEY_RIGHT: step(+10); return true;
        case MOKYA_KEY_LEFT:  step(-10); return true;
        case MOKYA_KEY_OK:
            s.done = true;
            s.committed = true;
            return true;
        case MOKYA_KEY_BACK:
            s.done = true;
            s.committed = false;
            return true;
        default: return false;
    }
}

bool    template_number_done(void)      { return s.done; }
bool    template_number_committed(void) { return s.committed; }
int32_t template_number_value(void)     { return s.edit; }
