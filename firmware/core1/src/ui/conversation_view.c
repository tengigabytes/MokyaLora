/* conversation_view.c — see conversation_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15  header: peer short_name + node_id
 *   y  16..199 message rows (8 visible × 24 px)
 *   y 200..223 footer: "OK compose | BACK list"
 *
 * Bubble rendering is simplified to row-aligned text in this Phase 3
 * baseline: outbound rows are right-aligned and orange; inbound rows
 * are left-aligned and white. Real chat-bubble graphics defer to a
 * later pass.
 *
 * Send flow:
 *   1. OK in the view → `ime_request_text` with draft_id = peer_node_id
 *      and layout = Mode B (Phase 3 baseline; Mode A inline will be
 *      a later refactor of ime_view).
 *   2. On commit, the trampoline callback calls `messages_send_text`
 *      which mirrors into dm_store via the existing send hook.
 *   3. ack updates flow back through `messages_tx_status_publish` →
 *      `dm_store_update_ack`, which re-renders the bubble state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "conversation_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "global/hint_bar.h"

#include "FreeRTOS.h"
#include "task.h"

#include "key_event.h"
#include "mie/keycode.h"

#include "chat_list_view.h"
#include "dm_store.h"
#include "ime_task.h"
#include "messages_send.h"
#include "phoneapi_cache.h"

/* ── Layout ─────────────────────────────────────────────────────────── */

#define ROW_H        24
#define MAX_VISIBLE   8
#define HEADER_H     16
#define FOOTER_H     24

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_VISIBLE];
    lv_obj_t *footer;
    uint32_t  peer_node_id;
    uint32_t  last_count;       /* used to gate rebuilds */
    uint32_t  last_change_seq;
} conversation_t;

static conversation_t s;

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

static void format_msg(char *buf, size_t cap, const dm_msg_t *m)
{
    char text[80] = {0};
    size_t n = m->text_len < sizeof(text) - 1
               ? m->text_len : sizeof(text) - 1;
    memcpy(text, m->text, n);
    text[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '\n' || text[i] == '\r') text[i] = ' ';
    }

    if (m->outbound) {
        const char *st = "?";
        switch (m->ack_state) {
            case DM_ACK_SENDING:   st = "..."; break;
            case DM_ACK_DELIVERED: st = "ok ";  break;
            case DM_ACK_FAILED:    st = "x  "; break;
            default:               st = "   "; break;
        }
        snprintf(buf, cap, "                    >> %s %s", st, text);
    } else {
        snprintf(buf, cap, "<< %s", text);
    }
}

static void rebuild_rows(void)
{
    /* Header */
    char hdr[64];
    phoneapi_node_t node;
    if (s.peer_node_id != 0u &&
        phoneapi_cache_get_node_by_id(s.peer_node_id, &node) &&
        node.short_name[0] != '\0') {
        snprintf(hdr, sizeof(hdr), "Chat: %s  !%08lx",
                 node.short_name, (unsigned long)s.peer_node_id);
    } else if (s.peer_node_id != 0u) {
        snprintf(hdr, sizeof(hdr), "Chat: !%08lx",
                 (unsigned long)s.peer_node_id);
    } else {
        snprintf(hdr, sizeof(hdr), "Chat: (no peer)");
    }
    lv_label_set_text(s.header, hdr);

    /* Pull peer summary */
    dm_peer_summary_t p;
    bool have = (s.peer_node_id != 0u) &&
                dm_store_get_peer(s.peer_node_id, &p);
    uint8_t total = have ? p.count : 0;

    /* Show the most recent MAX_VISIBLE messages, oldest at top. */
    uint8_t start = (total > MAX_VISIBLE)
                    ? (uint8_t)(total - MAX_VISIBLE) : 0;
    for (int i = 0; i < MAX_VISIBLE; ++i) {
        uint8_t mi = (uint8_t)(start + i);
        if (mi < total) {
            dm_msg_t m;
            if (dm_store_get_msg(s.peer_node_id, mi, &m)) {
                char buf[160];
                format_msg(buf, sizeof(buf), &m);
                lv_label_set_text(s.rows[i], buf);
                lv_obj_set_style_text_color(s.rows[i],
                    m.outbound ? ui_color(UI_COLOR_ACCENT_FOCUS)
                               : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
            } else {
                lv_label_set_text(s.rows[i], "");
            }
        } else if (i == 0 && total == 0) {
            lv_label_set_text(s.rows[i], "  (empty thread)");
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_TEXT_SECONDARY), 0);
        } else {
            lv_label_set_text(s.rows[i], "");
        }
    }

    /* Mark all as read on enter / refresh tick. */
    if (s.peer_node_id != 0u && have && p.unread > 0) {
        dm_store_mark_read(s.peer_node_id);
    }

    s.last_count = total;
}

/* ── Compose flow ───────────────────────────────────────────────────── */

static void compose_done(bool committed,
                         const char *utf8,
                         uint16_t    byte_len,
                         void       *ctx)
{
    (void)ctx;
    if (!committed || byte_len == 0u || s.peer_node_id == 0u) return;
    /* Send via the cascade encoder; messages_send_text mirrors the
     * outbound bubble into dm_store automatically. */
    uint32_t pid = 0u;
    (void)messages_send_text(s.peer_node_id, /*channel=*/0u,
                             /*want_ack=*/true,
                             (const uint8_t *)utf8, byte_len, &pid);
    /* Force a redraw on the next refresh. */
    s.last_count = 0;
}

static void open_compose(void)
{
    if (s.peer_node_id == 0u) return;
    if (ime_request_text_active()) return;
    ime_text_request_t req = {
        .prompt    = "Reply",
        .initial   = NULL,
        .max_bytes = (uint16_t)MESSAGES_SEND_TEXT_MAX,
        .mode_hint = IME_TEXT_MODE_DEFAULT,
        .flags     = IME_TEXT_FLAG_NONE,
        .layout    = IME_TEXT_LAYOUT_FULLSCREEN,   /* Mode B baseline */
        .draft_id  = s.peer_node_id,               /* per-peer draft */
    };
    (void)ime_request_text(&req, compose_done, NULL);
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));
    s.peer_node_id = chat_list_get_active_peer();

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, HEADER_H);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "");

    for (int i = 0; i < MAX_VISIBLE; ++i) {
        s.rows[i] = make_row(panel, HEADER_H + i * ROW_H);
    }

    s.footer = lv_label_create(panel);
    lv_obj_set_pos(s.footer, 4, HEADER_H + MAX_VISIBLE * ROW_H);
    lv_obj_set_size(s.footer, 320 - 8, FOOTER_H);
    lv_obj_set_style_text_font(s.footer, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.footer,
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.footer, 0, 0);
    lv_label_set_text(s.footer, "OK compose  BACK list   (in IME: SET=send / BACK=cancel)");

    rebuild_rows();
    hint_bar_clear();   /* footer carries the hint */
}

static void destroy(void)
{
    s.header = s.footer = NULL;
    for (int i = 0; i < MAX_VISIBLE; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_OK:
            open_compose();
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_MESSAGES);
            break;
        default: break;
    }
}

static void refresh(void)
{
    /* Detect new messages or ack changes by comparing the peer's count
     * + a coarse "any change" tick. */
    if (s.peer_node_id == 0u) return;
    dm_peer_summary_t p;
    if (!dm_store_get_peer(s.peer_node_id, &p)) return;
    if (p.count != s.last_count || p.unread != 0) {
        rebuild_rows();
    }
}

static const view_descriptor_t CONV_DESC = {
    .id      = VIEW_ID_MESSAGES_CHAT,
    .name    = "conversation",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *conversation_view_descriptor(void)
{
    return &CONV_DESC;
}
