/* nodes_view.c — see nodes_view.h.
 *
 * C-1 list view (multi-row). 8 visible rows × 24 px below a 16 px
 * header, sourced from phoneapi_cache (cascade NodeInfo decoder).
 *
 * Each row format: "[focus] short_name  long_name  SNR  hops  age"
 *
 * Sort: most-recently-active first (phoneapi_cache_take_node_at(0) =
 * the node we last heard from). Cursor stays put when new nodes
 * arrive — sticky-to-newest behaviour from the previous single-node
 * variant is dropped because it would jump the user out of their
 * current focus when a new node appears.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nodes_view.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "phoneapi_cache.h"
#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"
#include "node_alias.h"

/* ── Layout ─────────────────────────────────────────────────────────── */

#define ROW_H        24
#define MAX_VISIBLE   8
#define HEADER_H     16
#define LIST_END    (HEADER_H + ROW_H * MAX_VISIBLE)   /* 16 + 192 = 208 */

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_VISIBLE];
    uint32_t  cursor;            /* node-list offset 0..total-1   */
    uint32_t  scroll_top;        /* first visible offset          */
    uint32_t  last_change_seq;
} nodes_t;

static nodes_t s;

/* Cross-view stash for C-2 / C-3 hand-off. */
static uint32_t s_active_node;

void     nodes_view_set_active_node(uint32_t n) { s_active_node = n; }
uint32_t nodes_view_get_active_node(void)        { return s_active_node; }

/* ── Helpers ────────────────────────────────────────────────────────── */

static lv_obj_t *make_row(lv_obj_t *parent, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, 4, y);
    lv_obj_set_size(l, 320 - 8, ROW_H);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static void format_row(char *buf, size_t cap, bool focused,
                       const phoneapi_node_t *e)
{
    char snr_str[16];
    if (e->snr_x100 == INT32_MIN) {
        snprintf(snr_str, sizeof(snr_str), "-- ");
    } else {
        snprintf(snr_str, sizeof(snr_str), "%+.1fdB", e->snr_x100 / 100.0);
    }
    char hops_str[8];
    if (e->hops_away == 0xFFu) snprintf(hops_str, sizeof(hops_str), "?");
    else snprintf(hops_str, sizeof(hops_str), "%uh", (unsigned)e->hops_away);

    /* Prefer local alias over broadcast short_name — node_alias_format
     * falls back to short_name then "!hex" when no alias is set. */
    char nm[24];
    node_alias_format_display(e->num, e->short_name, nm, sizeof(nm));
    snprintf(buf, cap, "%s%-8s  %-12s  %s %s",
             focused ? ">" : " ",
             nm,
             e->long_name[0] ? e->long_name : "(no name)",
             snr_str, hops_str);
}

static void clamp_scroll(uint32_t total)
{
    if (total == 0u) {
        s.cursor = 0u;
        s.scroll_top = 0u;
        return;
    }
    if (s.cursor >= total) s.cursor = total - 1u;
    if (s.cursor < s.scroll_top) s.scroll_top = s.cursor;
    if (s.cursor >= s.scroll_top + MAX_VISIBLE) {
        s.scroll_top = s.cursor - MAX_VISIBLE + 1u;
    }
    if (s.scroll_top + MAX_VISIBLE > total) {
        s.scroll_top = (total > MAX_VISIBLE) ? (total - MAX_VISIBLE) : 0u;
    }
}

static void render(void)
{
    uint32_t total = phoneapi_cache_node_count();

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Nodes  (%lu total)",
             (unsigned long)total);
    lv_label_set_text(s.header, hdr);

    clamp_scroll(total);

    char buf[96];
    for (uint32_t i = 0; i < MAX_VISIBLE; ++i) {
        uint32_t off = s.scroll_top + i;
        if (off >= total) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        phoneapi_node_t e;
        if (!phoneapi_cache_take_node_at(off, &e)) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        bool focused = (off == s.cursor);
        format_row(buf, sizeof(buf), focused, &e);
        lv_label_set_text(s.rows[i], buf);
        lv_obj_set_style_text_color(s.rows[i],
            focused ? ui_color(UI_COLOR_ACCENT_FOCUS)
                    : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, HEADER_H);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "Nodes");

    for (int i = 0; i < MAX_VISIBLE; ++i) {
        s.rows[i] = make_row(panel, HEADER_H + i * ROW_H);
    }

    render();
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < MAX_VISIBLE; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    uint32_t total = phoneapi_cache_node_count();

    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (total > 0u && s.cursor > 0u) {
                s.cursor--;
                render();
            }
            break;
        case MOKYA_KEY_DOWN:
            if (total > 0u && s.cursor + 1u < total) {
                s.cursor++;
                render();
            }
            break;
        case MOKYA_KEY_OK: {
            if (total == 0u) break;
            phoneapi_node_t e;
            if (phoneapi_cache_take_node_at(s.cursor, &e)) {
                nodes_view_set_active_node(e.num);
                view_router_navigate(VIEW_ID_NODE_DETAIL);
            }
            break;
        }
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_BOOT_HOME);
            break;
        default: break;
    }
}

static void refresh(void)
{
    if (s.header == NULL) return;
    uint32_t cur = phoneapi_cache_change_seq();
    if (cur == s.last_change_seq) return;
    s.last_change_seq = cur;
    render();
}

static const view_descriptor_t NODES_DESC = {
    .id      = VIEW_ID_NODES,
    .name    = "nodes",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "up/dn pick", "OK detail", "BACK home" },
};

const view_descriptor_t *nodes_view_descriptor(void)
{
    return &NODES_DESC;
}
