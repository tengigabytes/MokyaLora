/* chat_list_view.c — see chat_list_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15  group header "DM chats" + total unread
 *   y  16..207 peer rows (8 visible @ 24 px each)
 *   y 208..223 hint bar overlay (managed by hint_bar_set)
 *
 * Each row: [focus '>'] short_name (8 ch) | last_preview (≈24 ch) |
 *           unread badge.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "chat_list_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "global/hint_bar.h"

#include "FreeRTOS.h"
#include "task.h"

#include "key_event.h"
#include "mie/keycode.h"

#include "dm_store.h"
#include "phoneapi_cache.h"

/* ── Cross-view stash ────────────────────────────────────────────────── */

static uint32_t s_active_peer;

void chat_list_set_active_peer(uint32_t peer) { s_active_peer = peer; }
uint32_t chat_list_get_active_peer(void)      { return s_active_peer; }

/* ── View state ──────────────────────────────────────────────────────── */

#define ROW_H        24
#define MAX_VISIBLE   8

typedef struct {
    lv_obj_t *bg;
    lv_obj_t *header;
    lv_obj_t *rows[MAX_VISIBLE];
    uint8_t   cur_row;
    uint32_t  last_change_seq;   /* gate refresh */
} chat_list_t;

static chat_list_t s;

/* ── Helpers ─────────────────────────────────────────────────────────── */

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

static void format_row(char *buf, size_t cap,
                       bool focused,
                       const dm_peer_summary_t *p)
{
    /* Look up short_name from phoneapi_cache; fall back to id-hex. */
    char who[16] = {0};
    phoneapi_node_t node;
    if (phoneapi_cache_get_node_by_id(p->peer_node_id, &node) &&
        node.short_name[0] != '\0') {
        snprintf(who, sizeof(who), "%-4s", node.short_name);
    } else {
        snprintf(who, sizeof(who), "!%08lx",
                 (unsigned long)p->peer_node_id);
    }

    /* Pull the most-recent message preview. */
    char preview[40] = {0};
    if (p->count > 0) {
        dm_msg_t m;
        if (dm_store_get_msg(p->peer_node_id, (uint8_t)(p->count - 1), &m)) {
            size_t n = m.text_len < sizeof(preview) - 1
                       ? m.text_len : sizeof(preview) - 1;
            memcpy(preview, m.text, n);
            preview[n] = '\0';
            /* Strip trailing newline for cleaner one-line preview. */
            for (size_t i = 0; i < n; ++i) {
                if (preview[i] == '\n' || preview[i] == '\r') preview[i] = ' ';
            }
        }
    }

    char unread[8] = "";
    if (p->unread > 0) {
        snprintf(unread, sizeof(unread), "(%u)", (unsigned)p->unread);
    }

    snprintf(buf, cap, "%s %-12s %-22s %s",
             focused ? ">" : " ", who, preview, unread);
}

static void rebuild_rows(void)
{
    uint32_t n = dm_store_peer_count();
    if (s.cur_row >= n && n > 0) s.cur_row = (uint8_t)(n - 1);

    char hdr[48];
    uint32_t total = dm_store_total_unread();
    snprintf(hdr, sizeof(hdr), "DM chats   peers=%lu  unread=%lu",
             (unsigned long)n, (unsigned long)total);
    lv_label_set_text(s.header, hdr);

    char buf[96];
    for (int i = 0; i < MAX_VISIBLE; ++i) {
        if (i < (int)n) {
            dm_peer_summary_t p;
            if (dm_store_peer_at((uint32_t)i, &p)) {
                format_row(buf, sizeof(buf), i == s.cur_row, &p);
                lv_label_set_text(s.rows[i], buf);
                lv_obj_set_style_text_color(s.rows[i],
                    i == s.cur_row
                        ? ui_color(UI_COLOR_ACCENT_FOCUS)
                        : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
            } else {
                lv_label_set_text(s.rows[i], "");
            }
        } else if (i == 0) {
            lv_label_set_text(s.rows[i], "  (no DMs yet)");
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_TEXT_SECONDARY), 0);
        } else {
            lv_label_set_text(s.rows[i], "");
        }
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, 16);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "DM chats");

    for (int i = 0; i < MAX_VISIBLE; ++i) {
        s.rows[i] = make_row(panel, 16 + i * ROW_H);
    }

    rebuild_rows();
    hint_bar_set("up/dn pick", "OK chat", "BACK home");
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < MAX_VISIBLE; ++i) s.rows[i] = NULL;
    hint_bar_clear();
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    uint32_t n = dm_store_peer_count();
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cur_row > 0) { s.cur_row--; rebuild_rows(); }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cur_row + 1 < n && s.cur_row + 1 < MAX_VISIBLE) {
                s.cur_row++; rebuild_rows();
            }
            break;
        case MOKYA_KEY_OK: {
            if (n == 0) break;
            dm_peer_summary_t p;
            if (dm_store_peer_at(s.cur_row, &p)) {
                chat_list_set_active_peer(p.peer_node_id);
                view_router_navigate(VIEW_ID_MESSAGES_CHAT);
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
    /* Single source of truth: dm_store_change_seq bumps on every mutation
     * path (ingest_inbound / ingest_outbound / update_ack / mark_read).
     * Saves a peer_count + total_unread mutex round-trip every tick when
     * nothing changed, AND avoids the function-static ghost values that
     * could persist across LRU evictions. */
    uint32_t seq = dm_store_change_seq();
    if (seq != s.last_change_seq) {
        s.last_change_seq = seq;
        rebuild_rows();
    }
}

static const view_descriptor_t CHAT_LIST_DESC = {
    .id      = VIEW_ID_MESSAGES,
    .name    = "chat_list",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *chat_list_view_descriptor(void)
{
    return &CHAT_LIST_DESC;
}
