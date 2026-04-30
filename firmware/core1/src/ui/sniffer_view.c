/* sniffer_view.c — T-4 packet sniffer.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header  "T-4 嗅探  N/16 (total=M)"
 *   y  16..183  7 rows × 24 px (visible window into 16-entry ring)
 *   y 184..207  detail line — full hex of cursor packet (next 16 B)
 *   y 208..223  hint
 *
 * Each row format:
 *   `<from>  P<num>  <8-hex-bytes>  ±SS dB`
 *
 * Cursor selects newest-first packet 0..(count-1); UP/DOWN walks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sniffer_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "node_alias.h"
#include "packet_log.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define ROW_H           24
#define VISIBLE_ROWS     7
#define ROWS_TOP        16
#define DETAIL_TOP     184
#define DETAIL_H        24
#define HINT_TOP       208
#define HINT_H          16
#define PANEL_W        320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[VISIBLE_ROWS];
    lv_obj_t *detail;
    lv_obj_t *hint;
    uint8_t   cursor;          /* 0 = newest, walks toward older */
    uint8_t   scroll_top;      /* index of first visible row */
    uint32_t  last_change_seq;
} sniff_t;

static sniff_t s;

/* SWD diag exports — for test_t4_sniffer.py to verify */
volatile uint32_t g_t4_count       __attribute__((used)) = 0u;
volatile uint32_t g_t4_total       __attribute__((used)) = 0u;
volatile uint32_t g_t4_newest_from __attribute__((used)) = 0u;
volatile uint32_t g_t4_newest_pn   __attribute__((used)) = 0u;
volatile uint8_t  g_t4_newest_payload[16] __attribute__((used)) = {0};
volatile uint8_t  g_t4_newest_payload_len __attribute__((used)) = 0u;

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

static const char *portnum_short(uint32_t pn)
{
    switch (pn) {
        case 1u:  return "TXT";
        case 3u:  return "POS";
        case 4u:  return "NDI";    /* NodeInfo */
        case 5u:  return "ROU";    /* Routing  */
        case 6u:  return "ADM";    /* Admin    */
        case 66u: return "RNG";
        case 67u: return "TLM";
        case 70u: return "TR ";
        case 71u: return "NBR";
        default:  return NULL;
    }
}

static void format_row(const packet_log_entry_t *e, char *buf, size_t cap,
                       bool focused)
{
    char nm[10];
    node_alias_format_display(e->from_node, NULL, nm, sizeof(nm));

    /* portnum mnemonic if known, else numeric */
    const char *pn_short = portnum_short(e->portnum);
    char pn_buf[6];
    if (pn_short) snprintf(pn_buf, sizeof(pn_buf), "%s", pn_short);
    else          snprintf(pn_buf, sizeof(pn_buf), "%-3lu", (unsigned long)e->portnum);

    /* First 8 byte payload hex = 16 chars */
    char hex[20] = {0};
    size_t hp = 0;
    static const char HD[] = "0123456789abcdef";
    for (size_t i = 0; i < e->payload_len && i < 8u && hp + 2 < sizeof(hex); ++i) {
        hex[hp++] = HD[(e->payload[i] >> 4) & 0xF];
        hex[hp++] = HD[e->payload[i] & 0xF];
    }
    /* Pad to 16 chars */
    while (hp < 16u && hp + 1 < sizeof(hex)) hex[hp++] = ' ';
    hex[hp] = '\0';

    /* SNR display */
    char snr_s[8];
    if (e->snr_x4 == INT8_MIN) snprintf(snr_s, sizeof(snr_s), "  --");
    else {
        int s4 = e->snr_x4;
        int sign = (s4 < 0) ? -1 : 1;
        int abs4 = s4 * sign;
        snprintf(snr_s, sizeof(snr_s), "%s%d.%d",
                 sign < 0 ? "-" : "+", abs4 / 4, (abs4 % 4) * 25);
    }

    snprintf(buf, cap, "%s%-8s %-3s %s %s",
             focused ? ">" : " ",
             nm, pn_buf, hex, snr_s);
}

static void render(void)
{
    uint32_t count = packet_log_count();
    uint32_t total = packet_log_total();

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "T-4 嗅探  %lu/%u  total=%lu",
             (unsigned long)count, (unsigned)PACKET_LOG_CAP,
             (unsigned long)total);
    lv_label_set_text(s.header, hdr);

    /* Clamp cursor + scroll. */
    if (count == 0u) {
        s.cursor = 0u;
        s.scroll_top = 0u;
    } else {
        if (s.cursor >= count) s.cursor = (uint8_t)(count - 1u);
        if (s.cursor < s.scroll_top) s.scroll_top = s.cursor;
        if (s.cursor >= s.scroll_top + VISIBLE_ROWS) {
            s.scroll_top = s.cursor - VISIBLE_ROWS + 1u;
        }
        if (s.scroll_top + VISIBLE_ROWS > count) {
            s.scroll_top = (count > VISIBLE_ROWS) ? (count - VISIBLE_ROWS) : 0u;
        }
    }

    /* Visible rows */
    char buf[80];
    for (uint8_t r = 0; r < VISIBLE_ROWS; ++r) {
        uint32_t idx = s.scroll_top + r;
        if (idx >= count) {
            lv_label_set_text(s.rows[r], "");
            continue;
        }
        packet_log_entry_t e;
        if (!packet_log_get_newest(idx, &e)) {
            lv_label_set_text(s.rows[r], "");
            continue;
        }
        format_row(&e, buf, sizeof(buf), idx == s.cursor);
        lv_label_set_text(s.rows[r], buf);
        lv_obj_set_style_text_color(s.rows[r],
            (idx == s.cursor) ? ui_color(UI_COLOR_ACCENT_FOCUS)
                              : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }

    /* Detail row — bytes 8..15 of cursor's payload + RSSI. */
    if (count > 0u) {
        packet_log_entry_t cur;
        if (packet_log_get_newest(s.cursor, &cur)) {
            char hex_more[40] = {0};
            size_t hp = 0;
            static const char HD[] = "0123456789abcdef";
            for (size_t i = 8; i < cur.payload_len && hp + 2 < sizeof(hex_more); ++i) {
                hex_more[hp++] = HD[(cur.payload[i] >> 4) & 0xF];
                hex_more[hp++] = HD[cur.payload[i] & 0xF];
            }
            hex_more[hp] = '\0';
            char det[80];
            snprintf(det, sizeof(det),
                     "more: %s  RSSI=%d  ep=%lu",
                     hp > 0 ? hex_more : "(none)",
                     (int)cur.rssi, (unsigned long)cur.epoch);
            lv_label_set_text(s.detail, det);
        }
    } else {
        lv_label_set_text(s.detail, "(no packets — wait for cascade RX)");
    }

    /* SWD diag exports */
    g_t4_count = count;
    g_t4_total = total;
    if (count > 0u) {
        packet_log_entry_t newest;
        if (packet_log_get_newest(0, &newest)) {
            g_t4_newest_from = newest.from_node;
            g_t4_newest_pn   = newest.portnum;
            g_t4_newest_payload_len = newest.payload_len;
            for (size_t i = 0; i < 16; ++i) {
                g_t4_newest_payload[i] = (i < newest.payload_len)
                    ? newest.payload[i] : 0u;
            }
        }
    }

    TRACE("snif", "render", "count=%lu total=%lu cursor=%u",
          (unsigned long)count, (unsigned long)total, (unsigned)s.cursor);
}

static void create(lv_obj_t *panel)
{
    /* Preserve cursor across LRU re-create. */
    uint8_t saved_cursor = s.cursor;
    uint8_t saved_scroll = s.scroll_top;
    memset(&s, 0, sizeof(s));
    s.cursor = saved_cursor;
    s.scroll_top = saved_scroll;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    for (int i = 0; i < VISIBLE_ROWS; ++i) {
        s.rows[i] = make_label(panel, 4, ROWS_TOP + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.detail = make_label(panel, 4, DETAIL_TOP, PANEL_W - 8, DETAIL_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    s.hint   = make_label(panel, 4, HINT_TOP, PANEL_W - 8, HINT_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.hint, "UP/DN 翻包  BACK 工具");

    render();
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < VISIBLE_ROWS; ++i) s.rows[i] = NULL;
    s.detail = NULL;
    s.hint = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cursor > 0u) { s.cursor--; render(); }
            break;
        case MOKYA_KEY_DOWN: {
            uint32_t cnt = packet_log_count();
            if (cnt > 0u && s.cursor + 1u < cnt) {
                s.cursor++;
                render();
            }
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
    uint32_t cur = packet_log_change_seq();
    if (cur == s.last_change_seq) return;
    s.last_change_seq = cur;
    render();
}

static const view_descriptor_t SNIFFER_DESC = {
    .id      = VIEW_ID_T4_SNIFFER,
    .name    = "sniffer",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN 翻包", NULL, "BACK 工具" },
};

const view_descriptor_t *sniffer_view_descriptor(void)
{
    return &SNIFFER_DESC;
}
