/* template_enum.c — see template_enum.h.
 *
 * Layout (within parent panel, anchored below settings_app_view's
 * breadcrumb at y=0..15):
 *
 *   y  20.. 39  title (key label)
 *   y  44.. 51  separator (1 px line)
 *   y  56..177  options list (5 rows × 22 px = 110 px + 12 px scroll hint)
 *   y 184..213  status footer ("●current ▶focus  OK apply / BACK cancel")
 *
 * Per spec §50 模板 A:
 *   ● = currently applied, ○ = not applied
 *   ▶ = focus marker, orange
 *   D-pad UP/DOWN moves focus, OK applies, BACK cancels
 *   Up to 5 rows visible, focus auto-scrolls to centre
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "template_enum.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "mie/keycode.h"

/* ── Layout constants ───────────────────────────────────────────────── */
#define ROW_H        22
#define MAX_VISIBLE   5

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *title_lbl;
    lv_obj_t *rows[MAX_VISIBLE];
    lv_obj_t *status_lbl;

    const settings_key_def_t *key;
    uint8_t initial;       /* current value at open()       */
    uint8_t focus;         /* selection cursor 0..count-1   */
    uint8_t scroll_top;    /* first visible row index       */
    uint8_t count;         /* enum_count, cached            */
    bool    done;
    bool    committed;
} te_state_t;

static te_state_t s;

/* ── Render helpers ─────────────────────────────────────────────────── */

static void clamp_scroll(void)
{
    /* Centre the focus when possible. With 5 visible slots, centre =
     * scroll_top + 2. Adjust scroll so focus stays at row 2 (or as
     * close as the bounds allow). */
    if (s.count <= MAX_VISIBLE) {
        s.scroll_top = 0;
        return;
    }
    int target_top = (int)s.focus - 2;
    if (target_top < 0) target_top = 0;
    int max_top = (int)s.count - MAX_VISIBLE;
    if (target_top > max_top) target_top = max_top;
    s.scroll_top = (uint8_t)target_top;
}

static void render_rows(void)
{
    char buf[80];
    for (uint8_t i = 0; i < MAX_VISIBLE; ++i) {
        uint8_t idx = (uint8_t)(s.scroll_top + i);
        if (idx >= s.count) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        const char *opt = s.key && s.key->enum_values
                          ? s.key->enum_values[idx] : "?";
        bool is_current = (idx == s.initial);
        bool is_focus   = (idx == s.focus);
        const char *cur_mark = is_current ? "*" : " ";
        const char *foc_mark = is_focus   ? ">" : " ";
        snprintf(buf, sizeof(buf), "%s%s %s", cur_mark, foc_mark,
                 opt ? opt : "(null)");
        lv_label_set_text(s.rows[i], buf);
        lv_obj_set_style_text_color(s.rows[i],
            is_focus ? ui_color(UI_COLOR_ACCENT_FOCUS)
                     : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }
}

static void render_status(void)
{
    char buf[80];
    if (s.count > MAX_VISIBLE) {
        uint8_t off = s.scroll_top;
        uint8_t end = (uint8_t)(off + MAX_VISIBLE);
        snprintf(buf, sizeof(buf),
                 "%u/%u  *=current  >=focus   OK apply / BACK cancel",
                 (unsigned)(s.focus + 1), (unsigned)s.count);
        (void)off; (void)end;
    } else {
        snprintf(buf, sizeof(buf),
                 "*=current  >=focus   OK apply / BACK cancel");
    }
    lv_label_set_text(s.status_lbl, buf);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void template_enum_open(lv_obj_t *parent,
                        const settings_key_def_t *key,
                        uint8_t current)
{
    memset(&s, 0, sizeof(s));
    s.key     = key;
    s.count   = key ? key->enum_count : 0u;
    s.initial = (current < s.count) ? current : 0u;
    s.focus   = s.initial;
    clamp_scroll();

    lv_color_t white = ui_color(UI_COLOR_TEXT_PRIMARY);
    lv_color_t dim   = ui_color(UI_COLOR_TEXT_SECONDARY);

    s.title_lbl = make_label(parent, 4, 20, 320 - 8, 20, white);
    lv_label_set_text(s.title_lbl,
                      key && key->label ? key->label : "(setting)");

    for (uint8_t i = 0; i < MAX_VISIBLE; ++i) {
        s.rows[i] = make_label(parent, 8, 56 + i * ROW_H, 320 - 16,
                               ROW_H, white);
    }

    s.status_lbl = make_label(parent, 4, 184, 320 - 8, 20, dim);

    render_rows();
    render_status();
}

void template_enum_close(void)
{
    if (s.title_lbl)  lv_obj_del(s.title_lbl);
    for (uint8_t i = 0; i < MAX_VISIBLE; ++i) {
        if (s.rows[i]) lv_obj_del(s.rows[i]);
    }
    if (s.status_lbl) lv_obj_del(s.status_lbl);
    memset(&s, 0, sizeof(s));
}

bool template_enum_apply_key(const key_event_t *ev)
{
    if (!ev->pressed) return false;
    if (s.count == 0u) {
        /* Empty enum table — nothing to pick. BACK only. */
        if (ev->keycode == MOKYA_KEY_BACK) {
            s.done = true; s.committed = false; return true;
        }
        return false;
    }
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.focus > 0u) {
                s.focus--;
                clamp_scroll();
                render_rows(); render_status();
            }
            return true;
        case MOKYA_KEY_DOWN:
            if (s.focus + 1u < s.count) {
                s.focus++;
                clamp_scroll();
                render_rows(); render_status();
            }
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

bool    template_enum_done(void)      { return s.done; }
bool    template_enum_committed(void) { return s.committed; }
uint8_t template_enum_value(void)     { return s.focus; }
