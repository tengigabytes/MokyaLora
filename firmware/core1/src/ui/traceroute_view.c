/* traceroute_view.c — see traceroute_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15  header ("T-1 Traceroute    pending pid=0xABCD")
 *   y  16..127 5 visible peer rows × 22 px (row 0 highlighted = cursor)
 *   y 128      1 px divider
 *   y 129..223 result panel (~95 px) — 4 lines:
 *       peer name + epoch
 *       fwd: hop chain + per-hop SNR
 *       back: hop chain + per-hop SNR
 *       hint: OK = send, UP/DOWN = pick
 *
 * The view doesn't own the radio — phoneapi_encode_traceroute pushes
 * a TRACEROUTE_APP frame onto the cascade TX ring and returns
 * immediately with a self-assigned MeshPacket.id.  The reply lands
 * later in phoneapi_session.c::FR_TAG_PACKET → phoneapi_decode_route
 * → phoneapi_cache_set_last_route → bumps change_seq → our refresh
 * picks it up.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "traceroute_view.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"

#include "phoneapi_cache.h"
#include "phoneapi_encode.h"
#include "node_alias.h"

#define HEADER_H        16
#define LIST_ROW_H      22
#define LIST_ROWS        5
#define LIST_TOP        HEADER_H
#define LIST_BOT        (LIST_TOP + LIST_ROW_H * LIST_ROWS)  /* 16 + 110 = 126 */
#define DIVIDER_Y       (LIST_BOT + 1)                        /* 127 */
#define RESULT_TOP      (DIVIDER_Y + 2)                       /* 129 */
#define RESULT_ROW_H    20
#define RESULT_ROWS      4
#define PANEL_W        320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[LIST_ROWS];
    lv_obj_t *divider;
    lv_obj_t *result[RESULT_ROWS];

    uint32_t  cursor;            /* node-list offset 0..total-1 */
    uint32_t  scroll_top;
    uint32_t  cache_seq;         /* re-render gate */

    /* Last sent traceroute (for the header status badge). */
    uint32_t  pending_peer;      /* 0 = none in flight */
    uint32_t  pending_pid;
    uint32_t  pending_sent_ms;
    uint32_t  last_result_epoch; /* tracks when result panel last updated */
} traceroute_t;

static traceroute_t s;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
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

static void clamp_scroll(uint32_t total)
{
    if (total == 0u) {
        s.cursor = 0u;
        s.scroll_top = 0u;
        return;
    }
    if (s.cursor >= total) s.cursor = total - 1u;
    if (s.cursor < s.scroll_top) s.scroll_top = s.cursor;
    if (s.cursor >= s.scroll_top + LIST_ROWS) {
        s.scroll_top = s.cursor - LIST_ROWS + 1u;
    }
    if (s.scroll_top + LIST_ROWS > total) {
        s.scroll_top = (total > LIST_ROWS) ? (total - LIST_ROWS) : 0u;
    }
}

static void format_peer_label(uint32_t peer, char *out, size_t cap)
{
    char nm[16] = {0};
    phoneapi_node_t e;
    if (phoneapi_cache_get_node_by_id(peer, &e)) {
        node_alias_format_display(peer, e.short_name, nm, sizeof(nm));
    } else {
        snprintf(nm, sizeof(nm), "!%08lx", (unsigned long)peer);
    }
    snprintf(out, cap, "%s", nm);
}

/* Compose one route line: "fwd: A -> B -> C -> me  (+5.3, +3.0 dB)".
 * `dir` = "fwd" | "back".  `hops` may be empty (count==0) — caller
 * guarantees that path is suppressed earlier. */
static void format_route_line(const char *dir,
                              const uint32_t *hops, uint8_t hops_count,
                              const int8_t *snr, char *out, size_t cap)
{
    char buf[96];
    int  off = snprintf(buf, sizeof(buf), "%s:", dir);
    for (uint8_t i = 0; i < hops_count && off < (int)sizeof(buf) - 16; ++i) {
        char nm[16];
        format_peer_label(hops[i], nm, sizeof(nm));
        off += snprintf(buf + off, sizeof(buf) - off, " %s%s",
                        nm, (i + 1u < hops_count) ? " ->" : "");
    }
    /* Append SNR string if any. snr[] is dB×4 (signed int8). */
    char snr_buf[64] = {0};
    int  s_off = 0;
    for (uint8_t i = 0; i < hops_count && s_off < (int)sizeof(snr_buf) - 8; ++i) {
        if (snr[i] == INT8_MIN) {
            s_off += snprintf(snr_buf + s_off, sizeof(snr_buf) - s_off,
                              "%s--", i ? "," : "");
        } else {
            int q  = snr[i];               /* dB × 4 */
            int dB10 = (q * 10) / 4;       /* 0.1 dB */
            int hi = dB10 / 10;
            int lo = dB10 < 0 ? (-dB10) % 10 : dB10 % 10;
            s_off += snprintf(snr_buf + s_off, sizeof(snr_buf) - s_off,
                              "%s%+d.%01d", i ? "," : "", hi, lo);
        }
    }
    if (snr_buf[0]) {
        snprintf(out, cap, "%s  (%s dB)", buf, snr_buf);
    } else {
        snprintf(out, cap, "%s", buf);
    }
}

static void render_list(uint32_t total)
{
    char buf[96];
    clamp_scroll(total);
    for (uint32_t i = 0; i < LIST_ROWS; ++i) {
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
        char nm[16];
        node_alias_format_display(e.num, e.short_name, nm, sizeof(nm));
        bool focused = (off == s.cursor);
        bool has_route = (e.last_route.epoch != 0u);
        snprintf(buf, sizeof(buf), "%s%-9s  hops=%s%s",
                 focused ? ">" : " ",
                 nm,
                 (e.hops_away == 0xFFu) ? "?"
                                         : (char[]){(char)('0' + e.hops_away % 10), 0},
                 has_route ? "  [route ✓]" : "");
        /* Workaround: `(char[]){...}` above won't work for hops > 9.
         * Re-do with snprintf to cover that. */
        char hops_s[6];
        if (e.hops_away == 0xFFu) snprintf(hops_s, sizeof(hops_s), "?");
        else snprintf(hops_s, sizeof(hops_s), "%u", (unsigned)e.hops_away);
        snprintf(buf, sizeof(buf), "%s%-9s  hops=%s%s",
                 focused ? ">" : " ",
                 nm, hops_s,
                 has_route ? "  [route OK]" : "");
        lv_label_set_text(s.rows[i], buf);
        lv_obj_set_style_text_color(s.rows[i],
            focused ? ui_color(UI_COLOR_ACCENT_FOCUS)
                    : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }
}

static void render_result(uint32_t total)
{
    if (total == 0u || s.cursor >= total) {
        lv_label_set_text(s.result[0], "(no peer)");
        for (int i = 1; i < RESULT_ROWS; ++i) lv_label_set_text(s.result[i], "");
        return;
    }
    phoneapi_node_t e;
    if (!phoneapi_cache_take_node_at(s.cursor, &e)) return;

    char nm[16];
    node_alias_format_display(e.num, e.short_name, nm, sizeof(nm));

    char buf[112];
    if (e.last_route.epoch == 0u) {
        snprintf(buf, sizeof(buf), "Peer %s  (no route reply yet)", nm);
        lv_label_set_text(s.result[0], buf);
        lv_label_set_text(s.result[1], "");
        lv_label_set_text(s.result[2], "");
        snprintf(buf, sizeof(buf),
                 "OK 發送  UP/DOWN 選人  BACK 工具");
        lv_label_set_text(s.result[3], buf);
        if (s.result[3]) {
            lv_obj_set_style_text_color(s.result[3],
                ui_color(UI_COLOR_TEXT_SECONDARY), 0);
        }
        return;
    }

    /* Have a stored route reply — render hop counts + per-hop SNRs. */
    snprintf(buf, sizeof(buf), "Peer %s  fwd=%u  back=%u  ep=%lu",
             nm,
             (unsigned)e.last_route.hop_count,
             (unsigned)e.last_route.hops_back_count,
             (unsigned long)e.last_route.epoch);
    lv_label_set_text(s.result[0], buf);

    if (e.last_route.hop_count > 0u) {
        format_route_line("fwd",
                          e.last_route.hops_full, e.last_route.hop_count,
                          e.last_route.snr_fwd, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "fwd: (direct, 0 hops)");
    }
    lv_label_set_text(s.result[1], buf);

    if (e.last_route.hops_back_count > 0u) {
        format_route_line("back",
                          e.last_route.hops_back_full,
                          e.last_route.hops_back_count,
                          e.last_route.snr_back, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "back: (no return path captured)");
    }
    lv_label_set_text(s.result[2], buf);

    snprintf(buf, sizeof(buf),
             "OK 重發  UP/DOWN 選人  BACK 工具");
    lv_label_set_text(s.result[3], buf);
    if (s.result[3]) {
        lv_obj_set_style_text_color(s.result[3],
            ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    }
}

static void render_header(uint32_t total)
{
    char buf[96];
    if (s.pending_peer == 0u) {
        snprintf(buf, sizeof(buf), "T-1 Traceroute  (%lu peer)",
                 (unsigned long)total);
    } else {
        uint32_t age_s = (now_ms() - s.pending_sent_ms) / 1000u;
        snprintf(buf, sizeof(buf),
                 "T-1 Traceroute  pending pid=%#lx  %us",
                 (unsigned long)s.pending_pid, (unsigned)age_s);
    }
    lv_label_set_text(s.header, buf);
}

static void render(void)
{
    uint32_t total = phoneapi_cache_node_count();
    render_header(total);
    render_list(total);
    render_result(total);
}

static void create(lv_obj_t *panel)
{
    /* Preserve cursor + pending across LRU evict — same pattern as
     * telemetry_view.  pending_peer staying set means user navigates
     * away and back, header still shows the in-flight badge until the
     * timeout below clears it. */
    uint32_t saved_cursor      = s.cursor;
    uint32_t saved_scroll      = s.scroll_top;
    uint32_t saved_pending_p   = s.pending_peer;
    uint32_t saved_pending_pid = s.pending_pid;
    uint32_t saved_pending_t   = s.pending_sent_ms;
    memset(&s, 0, sizeof(s));
    s.cursor          = saved_cursor;
    s.scroll_top      = saved_scroll;
    s.pending_peer    = saved_pending_p;
    s.pending_pid     = saved_pending_pid;
    s.pending_sent_ms = saved_pending_t;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    for (int i = 0; i < LIST_ROWS; ++i) {
        s.rows[i] = make_label(panel, 4, LIST_TOP + i * LIST_ROW_H,
                               PANEL_W - 8, LIST_ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    /* 1-px divider line via a thin label.  An lv_obj_t with a
     * background colour would cost an extra style; reuse the label
     * scaffold (text empty, height 1). */
    s.divider = lv_obj_create(panel);
    lv_obj_set_pos(s.divider, 4, DIVIDER_Y);
    lv_obj_set_size(s.divider, PANEL_W - 8, 1);
    lv_obj_set_style_bg_color(s.divider,
        ui_color(UI_COLOR_BORDER_NORMAL), 0);
    lv_obj_set_style_bg_opa(s.divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s.divider, 0, 0);
    lv_obj_set_style_pad_all(s.divider, 0, 0);
    lv_obj_clear_flag(s.divider, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < RESULT_ROWS; ++i) {
        s.result[i] = make_label(panel, 4, RESULT_TOP + i * RESULT_ROW_H,
                                 PANEL_W - 8, RESULT_ROW_H,
                                 ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.divider = NULL;
    for (int i = 0; i < LIST_ROWS; ++i) s.rows[i] = NULL;
    for (int i = 0; i < RESULT_ROWS; ++i) s.result[i] = NULL;
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
            if (total == 0u || s.cursor >= total) break;
            phoneapi_node_t e;
            if (!phoneapi_cache_take_node_at(s.cursor, &e)) break;
            uint32_t pid = 0u;
            bool ok = phoneapi_encode_traceroute(e.num, 0u, &pid);
            if (ok) {
                s.pending_peer    = e.num;
                s.pending_pid     = pid;
                s.pending_sent_ms = now_ms();
                TRACE("trview", "send",
                      "peer=%lu pid=%#lx",
                      (unsigned long)e.num, (unsigned long)pid);
            } else {
                /* Mark a one-shot failure in the header — caller can
                 * retry; no badge state persisted. */
                lv_label_set_text(s.header,
                    "T-1 Traceroute  push failed (queue full?)");
                TRACE("trview", "send_fail", "peer=%lu", (unsigned long)e.num);
            }
            render();
            break;
        }
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_TOOLS);
            break;
        default: break;
    }
}

static void refresh(void)
{
    if (s.header == NULL) return;

    /* Clear the pending badge after 30 s of no reply — Meshtastic's
     * default ack window is ~10 s but propagation across multiple hops
     * can push beyond that.  30 s is the same threshold the rate-limit
     * project memory mentions for traceroute. */
    if (s.pending_peer != 0u && (now_ms() - s.pending_sent_ms) > 30000u) {
        s.pending_peer = 0u;
    }

    uint32_t cur = phoneapi_cache_change_seq();
    bool seq_changed = (cur != s.cache_seq);
    if (seq_changed) {
        s.cache_seq = cur;
    }
    /* If nothing changed AND no pending badge ticking, skip the
     * redraw.  Otherwise render — header age counter or new last_route
     * needs to flow. */
    if (!seq_changed && s.pending_peer == 0u) return;
    render();
}

static const view_descriptor_t TRACEROUTE_DESC = {
    .id      = VIEW_ID_TRACEROUTE,
    .name    = "trview",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN 選人", "OK 發送", "BACK 工具" },
};

const view_descriptor_t *traceroute_view_descriptor(void)
{
    return &TRACEROUTE_DESC;
}
