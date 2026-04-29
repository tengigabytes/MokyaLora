/* template_toggle.c — see template_toggle.h.
 *
 * Layout (within parent panel area, anchored to settings_app_view's
 * existing breadcrumb header at y=0..15 — we render below at y=20+):
 *
 *   y  20.. 39  title (key label)
 *   y  40.. 71  ── padding ──
 *   y  72..103  slider track  ("LEFT  ●  RIGHT")
 *   y 104..127  current value summary "Now: <left/right>"
 *   y 128..213  description text (4 lines)
 *
 * Per spec §50 模板 C: D-pad ◀▶ flips, OK applies, BACK cancels.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "template_toggle.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "mie/keycode.h"

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *title_lbl;
    lv_obj_t *slider_lbl;
    lv_obj_t *now_lbl;
    lv_obj_t *desc_lbl;

    const settings_key_def_t *key;
    bool initial;       /* value at open() — for "Now:" line          */
    bool edit;          /* current edit value (toggled by ◀▶)         */
    bool done;          /* set true on OK or BACK                     */
    bool committed;     /* OK = true, BACK = false                    */
} tt_state_t;

static tt_state_t s;

/* ── Render ─────────────────────────────────────────────────────────── */

static void render_slider(void)
{
    /* Spec §50: "啟用 ────●──── 停用". Until generic left/right labels
     * are added to settings_key_def_t, fall back to "On / Off" for
     * BOOL kind. */
    const char *left  = "On";
    const char *right = "Off";
    /* edit=true → On (left); edit=false → Off (right). Convention: BOOL
     * settings store 1=enabled. */
    char buf[80];
    snprintf(buf, sizeof(buf),
             "%s   %s   %s",
             left,
             s.edit ? "[*]   .  " : " .   [*]",
             right);
    lv_label_set_text(s.slider_lbl, buf);
}

static void render_now(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Now: %s   New: %s",
             s.initial ? "On" : "Off",
             s.edit    ? "On" : "Off");
    lv_label_set_text(s.now_lbl, buf);
}

/* ── Public API ─────────────────────────────────────────────────────── */

void template_toggle_open(lv_obj_t *parent,
                          const settings_key_def_t *key,
                          bool current)
{
    memset(&s, 0, sizeof(s));
    s.key     = key;
    s.initial = current;
    s.edit    = current;

    lv_color_t white = ui_color(UI_COLOR_TEXT_PRIMARY);
    lv_color_t dim   = ui_color(UI_COLOR_TEXT_SECONDARY);
    lv_color_t fg    = ui_color(UI_COLOR_ACCENT_FOCUS);

    /* Title */
    s.title_lbl = lv_label_create(parent);
    lv_obj_set_pos(s.title_lbl, 4, 20);
    lv_obj_set_size(s.title_lbl, 320 - 8, 20);
    lv_obj_set_style_text_font(s.title_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.title_lbl, white, 0);
    lv_obj_set_style_pad_all(s.title_lbl, 0, 0);
    lv_label_set_text(s.title_lbl,
                      key && key->label ? key->label : "(setting)");

    /* Slider */
    s.slider_lbl = lv_label_create(parent);
    lv_obj_set_pos(s.slider_lbl, 4, 72);
    lv_obj_set_size(s.slider_lbl, 320 - 8, 24);
    lv_obj_set_style_text_font(s.slider_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.slider_lbl, fg, 0);
    lv_obj_set_style_pad_all(s.slider_lbl, 0, 0);
    lv_obj_set_style_text_align(s.slider_lbl, LV_TEXT_ALIGN_CENTER, 0);
    render_slider();

    /* Now */
    s.now_lbl = lv_label_create(parent);
    lv_obj_set_pos(s.now_lbl, 4, 110);
    lv_obj_set_size(s.now_lbl, 320 - 8, 20);
    lv_obj_set_style_text_font(s.now_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.now_lbl, dim, 0);
    lv_obj_set_style_pad_all(s.now_lbl, 0, 0);
    lv_obj_set_style_text_align(s.now_lbl, LV_TEXT_ALIGN_CENTER, 0);
    render_now();

    /* Description (footer hint) */
    s.desc_lbl = lv_label_create(parent);
    lv_obj_set_pos(s.desc_lbl, 4, 150);
    lv_obj_set_size(s.desc_lbl, 320 - 8, 60);
    lv_obj_set_style_text_font(s.desc_lbl, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.desc_lbl, dim, 0);
    lv_obj_set_style_pad_all(s.desc_lbl, 0, 0);
    lv_label_set_long_mode(s.desc_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s.desc_lbl,
                      "DPad LEFT/RIGHT to flip. OK applies; "
                      "BACK cancels and returns to the list.");
}

void template_toggle_close(void)
{
    if (s.title_lbl)  lv_obj_del(s.title_lbl);
    if (s.slider_lbl) lv_obj_del(s.slider_lbl);
    if (s.now_lbl)    lv_obj_del(s.now_lbl);
    if (s.desc_lbl)   lv_obj_del(s.desc_lbl);
    memset(&s, 0, sizeof(s));
}

bool template_toggle_apply_key(const key_event_t *ev)
{
    if (!ev->pressed) return false;
    switch (ev->keycode) {
        case MOKYA_KEY_LEFT:
            if (!s.edit) { s.edit = true; render_slider(); render_now(); }
            return true;
        case MOKYA_KEY_RIGHT:
            if (s.edit) { s.edit = false; render_slider(); render_now(); }
            return true;
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

bool template_toggle_done(void)      { return s.done; }
bool template_toggle_committed(void) { return s.committed; }
bool template_toggle_value(void)     { return s.edit; }
