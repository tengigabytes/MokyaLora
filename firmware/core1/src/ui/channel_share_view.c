/* channel_share_view.c — see channel_share_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header  "B-4 分享 ch%u  %s"
 *   y  16..199  URL text — lv_label with LONG_WRAP, monospace 16-px
 *               (ASCII fits 40 chars per line; typical URL ≈ 80–170
 *               chars → 2–5 lines)
 *   y 200..223  status / hint
 *
 * Refresh: gated on phoneapi_cache_change_seq — re-renders only when
 * channelFile changes. Cheap; URL build runs in ~1 ms.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "channel_share_view.h"
#include "channels_view.h"
#include "channel_share_url.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define URL_TOP         16
#define URL_H          184
#define STATUS_TOP     200
#define STATUS_H        24
#define PANEL_W        320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *url;
    lv_obj_t *status;
    uint8_t   active_idx;
    uint32_t  last_change_seq;
    char      url_buf[256];
    size_t    url_len;
} cshare_t;

/* PSRAM-resident — 256 B URL buffer + widgets bump the SRAM ledger
 * otherwise. Single-task access (lvgl_task) so no mutex needed. */
static cshare_t s __attribute__((section(".psram_bss")));

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

static void render(void)
{
    /* Header — channel name from cache. */
    phoneapi_channel_t ch;
    bool have = phoneapi_cache_get_channel(s.active_idx, &ch);
    char hdr[80];
    if (have && ch.name[0] != '\0') {
        snprintf(hdr, sizeof(hdr), "B-4 分享 ch%u  %s",
                 (unsigned)s.active_idx, ch.name);
    } else if (have) {
        snprintf(hdr, sizeof(hdr), "B-4 分享 ch%u", (unsigned)s.active_idx);
    } else {
        snprintf(hdr, sizeof(hdr), "B-4 分享 ch%u  (cache miss)",
                 (unsigned)s.active_idx);
    }
    lv_label_set_text(s.header, hdr);

    /* Build URL into buf. */
    s.url_len = channel_share_url_build(s.active_idx,
                                         s.url_buf, sizeof(s.url_buf));
    if (s.url_len > 0u) {
        lv_label_set_text(s.url, s.url_buf);
        char st[64];
        snprintf(st, sizeof(st), "URL %u chars  BACK 返回",
                 (unsigned)s.url_len);
        lv_label_set_text(s.status, st);
    } else {
        lv_label_set_text(s.url,
            "(URL build failed — channel/lora cache empty?)");
        lv_label_set_text(s.status, "BACK 返回");
    }
    TRACE("cshare", "render", "idx=%u url_len=%u",
          (unsigned)s.active_idx, (unsigned)s.url_len);
}

static void create(lv_obj_t *panel)
{
    /* Pull active index from the B-1 stash; channels_view set this
     * before navigating. Default 0 if stale / unset. */
    uint8_t target = channels_view_get_active_index();
    if (target >= PHONEAPI_CHANNEL_COUNT) target = 0u;

    /* Preserve nothing across LRU re-create — view is stateless beyond
     * the active_idx we pull on entry. */
    memset(&s, 0, sizeof(s));
    s.active_idx = target;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    s.url    = make_label(panel, 4, URL_TOP, PANEL_W - 8, URL_H,
                          ui_color(UI_COLOR_TEXT_PRIMARY));
    s.status = make_label(panel, 4, STATUS_TOP, PANEL_W - 8, STATUS_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.url    = NULL;
    s.status = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_BACK) {
        view_router_navigate(VIEW_ID_CHANNEL_EDIT);
    }
}

static void refresh(void)
{
    if (s.header == NULL) return;
    /* Re-render only on cache mutation — channel rename or PSK change
     * via B-2 should re-build the URL. */
    uint32_t cur = phoneapi_cache_change_seq();
    if (cur == s.last_change_seq) return;
    s.last_change_seq = cur;
    render();
}

static const view_descriptor_t CHANNEL_SHARE_DESC = {
    .id      = VIEW_ID_CHANNEL_SHARE,
    .name    = "ch_share",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 編輯" },
};

const view_descriptor_t *channel_share_view_descriptor(void)
{
    return &CHANNEL_SHARE_DESC;
}
