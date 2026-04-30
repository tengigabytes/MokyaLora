/* channels_view.c — see channels_view.h.
 *
 * Read-only list of all 8 ChannelSettings entries from
 * phoneapi_cache_get_channel.  Per-row format:
 *
 *   "0  PRIM  LongFast      [PSK]  pos=10  muted"
 *
 * The cache is populated by the cascade FR_TAG_CONFIG_COMPLETE walk
 * (B3-P3 expanded the per-channel decoder), so values reflect the
 * latest device state without needing an explicit IPC GET burst.
 *
 * Refresh pattern: gate on phoneapi_cache_change_seq() — same as
 * nodes_view.  Channel mutations bump the seq via
 * phoneapi_cache_set_channel().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "channels_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "mokya_trace.h"

#define ROW_H        24
#define HEADER_H     16
#define ROW_COUNT    PHONEAPI_CHANNEL_COUNT       /* 8 */
#define PANEL_W     320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[ROW_COUNT];
    uint8_t   cursor;                              /* 0..7 */
    uint32_t  last_change_seq;
} channels_t;

static channels_t s;

static uint8_t s_active_idx = 0xFFu;

void channels_view_set_active_index(uint8_t idx)
{
    s_active_idx = idx;
}

uint8_t channels_view_get_active_index(void)
{
    return s_active_idx;
}

static const char *role_short(uint8_t role)
{
    switch (role) {
        case PHONEAPI_CHAN_ROLE_DISABLED:  return "OFF ";
        case PHONEAPI_CHAN_ROLE_PRIMARY:   return "PRIM";
        case PHONEAPI_CHAN_ROLE_SECONDARY: return "SEC ";
        default:                           return "??? ";
    }
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

static void format_row(uint8_t idx, char *buf, size_t cap, bool focused)
{
    phoneapi_channel_t ch;
    if (!phoneapi_cache_get_channel(idx, &ch) || !ch.in_use) {
        snprintf(buf, cap, "%s%u  ---  (empty)",
                 focused ? ">" : " ", (unsigned)idx);
        return;
    }

    /* Encryption tag: psk_len 1 = default key (single byte index 1
     * means "Default" per Meshtastic), 16 / 32 = real key. 0 = open. */
    const char *enc;
    if      (ch.psk_len == 0u)  enc = "OPEN";
    else if (ch.psk_len == 1u)  enc = "DEF ";
    else                        enc = "PSK ";

    /* Build the row.  Position-precision and muted only meaningful
     * when has_module_settings is set. */
    char extra[24];
    if (ch.has_module_settings) {
        snprintf(extra, sizeof(extra), "  pos=%u%s",
                 (unsigned)ch.module_position_precision,
                 ch.module_is_muted ? "  muted" : "");
    } else {
        extra[0] = '\0';
    }

    snprintf(buf, cap, "%s%u  %s  %-11s  %s%s",
             focused ? ">" : " ",
             (unsigned)idx,
             role_short(ch.role),
             ch.name[0] ? ch.name : "(no name)",
             enc, extra);
}

static void render(void)
{
    char hdr[64];
    /* Tally enabled channels for the header summary. */
    uint8_t enabled = 0;
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        phoneapi_channel_t c;
        if (phoneapi_cache_get_channel(i, &c) && c.in_use &&
            c.role != PHONEAPI_CHAN_ROLE_DISABLED) {
            enabled++;
        }
    }
    snprintf(hdr, sizeof(hdr), "B-1 頻道列表  %u/%u 啟用",
             (unsigned)enabled, (unsigned)ROW_COUNT);
    lv_label_set_text(s.header, hdr);

    char buf[96];
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        bool focused = (i == s.cursor);
        format_row(i, buf, sizeof(buf), focused);
        lv_label_set_text(s.rows[i], buf);
        lv_obj_set_style_text_color(s.rows[i],
            focused ? ui_color(UI_COLOR_ACCENT_FOCUS)
                    : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }
}

static void create(lv_obj_t *panel)
{
    /* Preserve cursor across LRU evict for a stable UX. */
    uint8_t saved_cursor = s.cursor;
    memset(&s, 0, sizeof(s));
    s.cursor = saved_cursor;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    for (int i = 0; i < ROW_COUNT; ++i) {
        s.rows[i] = make_label(panel, 4, HEADER_H + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    render();
}

static void destroy(void)
{
    s.header = NULL;
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
            /* Empty slot → channel_add_view (B-3); occupied → channel_edit_view
             * (existing). "Empty" means role==DISABLED — Meshtastic populates
             * all 8 ChannelSettings entries so `in_use` is always true; the
             * role is the meaningful occupancy flag. Cache miss treated as
             * empty so cold-boot before cascade replay still routes to add. */
            phoneapi_channel_t ch;
            bool have = phoneapi_cache_get_channel(s.cursor, &ch);
            bool occupied = have && ch.in_use &&
                            ch.role != PHONEAPI_CHAN_ROLE_DISABLED;
            channels_view_set_active_index(s.cursor);
            if (occupied) {
                TRACE("chlist", "open_edit", "idx=%u", (unsigned)s.cursor);
                view_router_navigate(VIEW_ID_CHANNEL_EDIT);
            } else {
                TRACE("chlist", "open_add", "idx=%u", (unsigned)s.cursor);
                view_router_navigate(VIEW_ID_CHANNEL_ADD);
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

static const view_descriptor_t CHANNELS_DESC = {
    .id      = VIEW_ID_CHANNELS,
    .name    = "channels",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN 選頻道", "OK 編輯", "BACK 返回" },
};

const view_descriptor_t *channels_view_descriptor(void)
{
    return &CHANNELS_DESC;
}
