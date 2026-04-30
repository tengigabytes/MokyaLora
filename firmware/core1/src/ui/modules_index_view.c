/* modules_index_view.c — see modules_index_view.h.
 *
 * 10 spec-named module entries; each row is one S-7.N sub-page.  The
 * static row table couples the spec ordering directly so visual order
 * doesn't depend on the settings_group_t enum (which orders by
 * implementation maturity, not spec).
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header "S-7 模組設定"
 *   y  16..195  10 rows × 18 px (last row at 178..195, 10 px slack)
 *   y 196..223  status line (last action / SET ack)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "modules_index_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"

#include "settings_keys.h"
#include "settings/settings_app_view.h"
#include "settings/settings_tree.h"

#define HEADER_H        16
#define ROW_H           18
#define ROW_COUNT       10
#define LIST_TOP        HEADER_H
#define STATUS_TOP      (LIST_TOP + ROW_COUNT * ROW_H + 2)
#define PANEL_W        320

typedef struct {
    const char       *spec_id;       /* "S-7.1" .. */
    const char       *label;         /* user-visible name */
    bool              wired;
    settings_group_t  target_group;  /* only valid if wired */
    const char       *tbd_reason;    /* only valid if !wired */
} module_entry_t;

/* Spec order from docs/ui/01-page-architecture.md §S-7. */
static const module_entry_t k_modules[ROW_COUNT] = {
    { "S-7.1",  "Canned Message",    true,  SG_CANNED_MSG,    NULL },
    { "S-7.2",  "External Notif.",   true,  SG_EXT_NOTIF,     NULL },
    { "S-7.3",  "Range Test",        true,  SG_RANGE_TEST,    NULL },
    { "S-7.4",  "Store & Forward",   true,  SG_STORE_FORWARD, NULL },
    { "S-7.5",  "Telemetry",         true,  SG_TELEMETRY,     NULL },
    { "S-7.6",  "Detection Sensor",  true,  SG_DETECT_SENSOR, NULL },
    { "S-7.7",  "Paxcounter",        true,  SG_PAXCOUNTER,    NULL },
    { "S-7.8",  "Neighbor Info",     true,  SG_NEIGHBOR,      NULL },
    { "S-7.9",  "Serial",            true,  SG_SERIAL,        NULL },
    { "S-7.10", "Remote Hardware",   true,  SG_REMOTE_HW,     "available_pins[] 編輯器另案" },
};

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *status;
    uint8_t   cursor;
} modidx_t;

static modidx_t s;

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

static void render(void)
{
    char hdr[40];
    /* Tally wired count for header context. */
    int wired = 0;
    for (int i = 0; i < ROW_COUNT; ++i) if (k_modules[i].wired) wired++;
    snprintf(hdr, sizeof(hdr), "S-7 模組設定  %d/%d 已接 IPC", wired, ROW_COUNT);
    lv_label_set_text(s.header, hdr);

    char buf[80];
    for (int i = 0; i < ROW_COUNT; ++i) {
        const module_entry_t *m = &k_modules[i];
        bool focused = (i == s.cursor);
        if (m->wired) {
            uint8_t leaf_count = 0;
            (void)settings_keys_in_group(m->target_group, &leaf_count);
            snprintf(buf, sizeof(buf), "%s%s  %-18s  %u keys",
                     focused ? ">" : " ",
                     m->spec_id, m->label,
                     (unsigned)leaf_count);
        } else {
            snprintf(buf, sizeof(buf), "%s%s  %-18s  TBD",
                     focused ? ">" : " ",
                     m->spec_id, m->label);
        }
        lv_label_set_text(s.rows[i], buf);
        if (focused) {
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_ACCENT_FOCUS), 0);
        } else if (!m->wired) {
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_TEXT_SECONDARY), 0);
        } else {
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_TEXT_PRIMARY), 0);
        }
    }
}

static void create(lv_obj_t *panel)
{
    uint8_t saved_cursor = s.cursor;
    memset(&s, 0, sizeof(s));
    s.cursor = (saved_cursor < ROW_COUNT) ? saved_cursor : 0u;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    for (int i = 0; i < ROW_COUNT; ++i) {
        s.rows[i] = make_label(panel, 4, LIST_TOP + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    s.status = make_label(panel, 4, STATUS_TOP, PANEL_W - 8, ROW_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.status = NULL;
    for (int i = 0; i < ROW_COUNT; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cursor > 0u) { s.cursor--; render(); }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cursor + 1u < ROW_COUNT) { s.cursor++; render(); }
            break;
        case MOKYA_KEY_OK: {
            const module_entry_t *m = &k_modules[s.cursor];
            if (m->wired) {
                settings_app_view_set_initial_group(m->target_group, true);
                TRACE("modidx", "open",
                      "spec=%s grp=%u",
                      m->spec_id, (unsigned)m->target_group);
                view_router_navigate(VIEW_ID_SETTINGS);
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s %s — %s",
                         m->spec_id, m->label,
                         m->tbd_reason ? m->tbd_reason : "TBD");
                lv_label_set_text(s.status, buf);
                TRACE("modidx", "tbd", "spec=%s", m->spec_id);
            }
            break;
        }
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_SETTINGS);
            break;
        default: break;
    }
}

static void refresh(void) { /* static list; nothing to poll */ }

static const view_descriptor_t MODULES_INDEX_DESC = {
    .id      = VIEW_ID_MODULES_INDEX,
    .name    = "modidx",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN 選頁", "OK 進入", "BACK 設定" },
};

const view_descriptor_t *modules_index_view_descriptor(void)
{
    return &MODULES_INDEX_DESC;
}
