/* channel_share_view.c — see channel_share_view.h.
 *
 * Layout (panel 320 × 224, Phase 5b QR enabled):
 *   y   0..15   header   "B-4 分享 ch%u  %s"
 *   y  20..163  QR       lv_qrcode 144×144 px centered (x=88..231)
 *                         — internal I1 (1 bpp) buffer ≈ 2.5 KB
 *   y 168..199  URL text lv_label LONG_WRAP, smaller area than 5a
 *   y 200..223  status   URL length + BACK hint
 *
 * lv_qrcode auto-selects QR Version (1-40) and Mask (0-7) based on
 * the input data length; for our typical 57–170 char URL it picks
 * Version 4-8 (33×33 to 49×49 modules) at ECC L. Output is scaled
 * to fill the requested 144 px.
 *
 * Refresh gated on phoneapi_cache_change_seq — a B-2 rename or
 * PSK change automatically rebuilds the URL + re-encodes the QR.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "channel_share_view.h"
#include "channels_view.h"
#include "channel_share_url.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"   /* lv_qrcode_* lives under src/libs/qrcode and is
                     * pulled by lvgl.h master include */

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define QR_TOP          20
#define QR_SIZE        144
#define QR_LEFT        ((PANEL_W - QR_SIZE) / 2)   /* 88 */
#define URL_TOP        168
#define URL_H           32
#define STATUS_TOP     200
#define STATUS_H        24
#define PANEL_W        320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *qr;       /* lv_qrcode widget (Phase 5b)               */
    lv_obj_t *url;
    lv_obj_t *status;
    uint8_t   active_idx;
    uint32_t  last_change_seq;
    char      url_buf[256];
    size_t    url_len;
} cshare_t;

/* SRAM-resident (was .psram_bss but PSRAM is write-back cached and
 * SWD reads bypass cache → uncached alias returns stale data; the
 * audit test test_b4_share_url.py needs SWD-coherent reads of
 * url_buf/url_len, so park `s` in regular .bss).
 * Memory cost: ~280 B SRAM. */
static cshare_t s;

/* SWD diagnostic exports — published on every successful QR render
 * so a host script can dump the canvas pixel buffer and decode it.
 * Cleared to zero when QR fails. Memory cost: 16 B. */
volatile uint8_t  *g_b4_qr_data __attribute__((used)) = NULL;
volatile uint32_t  g_b4_qr_size __attribute__((used)) = 0u;
volatile uint16_t  g_b4_qr_w    __attribute__((used)) = 0u;
volatile uint16_t  g_b4_qr_h    __attribute__((used)) = 0u;
volatile uint32_t  g_b4_qr_stride __attribute__((used)) = 0u;
volatile uint8_t   g_b4_qr_cf   __attribute__((used)) = 0u;

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
        /* Push URL into the QR widget. lv_qrcode_update returns
         * LV_RESULT_INVALID if the data exceeds the encodable
         * capacity at the chosen size; we pass a generous 144 px
         * which the widget maps onto Version 1..40 internally. */
        lv_result_t qrres = LV_RESULT_INVALID;
        if (s.qr) {
            qrres = lv_qrcode_update(s.qr, s.url_buf, (uint32_t)s.url_len);
        }
        char st[80];
        snprintf(st, sizeof(st), "URL %u chars  QR %s  BACK",
                 (unsigned)s.url_len,
                 (qrres == LV_RESULT_OK) ? "OK" : "FAIL");
        lv_label_set_text(s.status, st);

        /* SWD diagnostic — publish canvas data pointer + dims so a host
         * dumper can read the raw I1 buffer. Cleared on failure to make
         * mismatch obvious. */
        if (qrres == LV_RESULT_OK && s.qr) {
            lv_draw_buf_t *db = lv_canvas_get_draw_buf(s.qr);
            if (db != NULL) {
                g_b4_qr_data   = db->data;
                g_b4_qr_size   = db->data_size;
                g_b4_qr_w      = (uint16_t)db->header.w;
                g_b4_qr_h      = (uint16_t)db->header.h;
                g_b4_qr_stride = db->header.stride;
                g_b4_qr_cf     = (uint8_t)db->header.cf;
            }
        } else {
            g_b4_qr_data = NULL;
            g_b4_qr_size = 0u;
        }
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

    /* QR canvas — lv_qrcode_set_size internally allocates the I1
     * draw buffer (size² / 8 bytes ≈ 2.5 KB at 144 px). Dark/light
     * follow the global theme so it stays readable on the dark BG. */
    s.qr = lv_qrcode_create(panel);
    lv_obj_set_pos(s.qr, QR_LEFT, QR_TOP);
    lv_qrcode_set_size(s.qr, QR_SIZE);
    lv_qrcode_set_dark_color(s.qr,  ui_color(UI_COLOR_TEXT_PRIMARY));
    lv_qrcode_set_light_color(s.qr, ui_color(UI_COLOR_BG_PRIMARY));

    s.url    = make_label(panel, 4, URL_TOP, PANEL_W - 8, URL_H,
                          ui_color(UI_COLOR_TEXT_PRIMARY));
    s.status = make_label(panel, 4, STATUS_TOP, PANEL_W - 8, STATUS_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.qr     = NULL;
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
