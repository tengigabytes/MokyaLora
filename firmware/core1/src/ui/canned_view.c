/* canned_view.c — see canned_view.h.
 *
 * A-4 canned-message picker. Modal: caller (typically conversation_view)
 * invokes `canned_view_set_target_peer(peer_node_id)` then opens this
 * via `view_router_modal_enter(VIEW_ID_CANNED, ...)`. Layout:
 *
 *   y   0..15   header: "Send to <name>"  (peer alias / short_name)
 *   y  16..207  one row per canned msg (8 rows × 24 px = 192 px)
 *   y 208..223  reserved for global hint_bar overlay (up/dn / OK / BACK)
 *
 * On OK: send via messages_send_text(peer, primary_channel=0, want_ack=true,
 * canned_at(cursor)) and call view_router_modal_finish(true). On BACK
 * the router's built-in BACK-in-modal handler cancels (modal_finish
 * with committed=false) before this view's apply() runs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "canned_view.h"
#include "canned_messages.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "messages_send.h"
#include "phoneapi_cache.h"
#include "node_alias.h"
#include "mokya_trace.h"

#define ROW_H        24
#define HEADER_H     16
#define MAX_VISIBLE   8

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_VISIBLE];
    uint8_t   cursor;
    uint32_t  target_peer;
} canned_t;

static canned_t s;

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

static void render(void)
{
    /* Header — peer name lookup chain matches conversation_view: alias
     * first, then cached short_name, then "!hex". target_peer == 0
     * means caller forgot to set; show a clear "(no peer)" so OK is
     * obviously a no-op. */
    char hdr[64];
    if (s.target_peer == 0u) {
        snprintf(hdr, sizeof(hdr), "Quick send  (no peer)");
    } else {
        char nm[24];
        phoneapi_node_t e;
        const char *short_name = NULL;
        if (phoneapi_cache_get_node_by_id(s.target_peer, &e)) {
            short_name = e.short_name;
        }
        node_alias_format_display(s.target_peer, short_name, nm, sizeof(nm));
        snprintf(hdr, sizeof(hdr), "Send to %s", nm);
    }
    lv_label_set_text(s.header, hdr);

    uint8_t total = canned_count();
    if (total > MAX_VISIBLE) total = MAX_VISIBLE;
    for (uint8_t i = 0; i < MAX_VISIBLE; ++i) {
        if (i < total) {
            const char *msg = canned_at(i);
            char buf[CANNED_MAX_LEN + 4];
            snprintf(buf, sizeof(buf), "%s %s",
                     i == s.cursor ? ">" : " ",
                     msg ? msg : "");
            lv_label_set_text(s.rows[i], buf);
            lv_obj_set_style_text_color(s.rows[i],
                i == s.cursor ? ui_color(UI_COLOR_ACCENT_FOCUS)
                              : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
        } else {
            lv_label_set_text(s.rows[i], "");
        }
    }
}

static void create(lv_obj_t *panel)
{
    /* Don't memset s — target_peer may have just been set by the caller
     * (canned_view_set_target_peer) before view_router_modal_enter. We
     * clear cursor + header only. */
    s.cursor = 0u;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, HEADER_H);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);

    for (uint8_t i = 0; i < MAX_VISIBLE; ++i) {
        s.rows[i] = make_row(panel, HEADER_H + i * ROW_H);
    }

    render();
}

static void destroy(void)
{
    s.header = NULL;
    for (uint8_t i = 0; i < MAX_VISIBLE; ++i) s.rows[i] = NULL;
}

static void send_picked(void)
{
    if (s.target_peer == 0u) return;
    uint8_t total = canned_count();
    if (s.cursor >= total) return;
    const char *msg = canned_at(s.cursor);
    if (msg == NULL) return;

    uint16_t len = (uint16_t)strnlen(msg, MESSAGES_SEND_TEXT_MAX);
    uint32_t pid = 0u;
    bool ok = messages_send_text(s.target_peer, /*channel=*/0u,
                                  /*want_ack=*/true,
                                  (const uint8_t *)msg, len, &pid);
    TRACE("canned", "send",
          "to=%lu cur=%u len=%u pid=0x%lx ok=%u",
          (unsigned long)s.target_peer, (unsigned)s.cursor,
          (unsigned)len, (unsigned long)pid, (unsigned)ok);
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    uint8_t total = canned_count();
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cursor > 0u) { s.cursor--; render(); }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cursor + 1u < total &&
                s.cursor + 1u < MAX_VISIBLE) {
                s.cursor++; render();
            }
            break;
        case MOKYA_KEY_OK:
            send_picked();
            view_router_modal_finish(true);
            break;
        /* BACK is consumed by view_router (BACK-in-modal cancels). */
        default: break;
    }
}

static void refresh(void) {}

void canned_view_set_target_peer(uint32_t peer_node_id)
{
    s.target_peer = peer_node_id;
}

static const view_descriptor_t CANNED_DESC = {
    .id      = VIEW_ID_CANNED,
    .name    = "canned",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "up/dn pick", "OK send", "BACK cancel" },
};

const view_descriptor_t *canned_view_descriptor(void)
{
    return &CANNED_DESC;
}
