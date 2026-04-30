/* pairing_view.c — see pairing_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header  "T-7 配對碼  本機公鑰"
 *   y  20..43   "HEX:"   label (dim)
 *   y  48..71   hex line 1 (bytes  0..15 = 32 chars)
 *   y  72..95   hex line 2 (bytes 16..31 = 32 chars)
 *   y 100..123  "BASE64:" label (dim)
 *   y 124..147  base64 line 1 (32 chars)
 *   y 148..171  base64 line 2 (12 chars + '=')
 *   y 200..223  status / hint
 *
 * Refresh gated on phoneapi_cache_change_seq — re-renders if the
 * security config gets updated by cascade.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pairing_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "base64_url.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define LABEL_H         16
#define ROW_H           24
#define PANEL_W        320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *hex_label;
    lv_obj_t *hex_line1;
    lv_obj_t *hex_line2;
    lv_obj_t *b64_label;
    lv_obj_t *b64_line1;
    lv_obj_t *b64_line2;
    lv_obj_t *status;
    uint32_t  last_change_seq;
    char      hex_buf[65];        /* 64 chars + NUL */
    char      b64_buf[48];        /* 44 chars + NUL with slack */
} pairing_t;

static pairing_t s;

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

static void format_hex_split(const uint8_t *bytes, size_t n,
                             char *line1, char *line2, size_t cap)
{
    static const char HD[] = "0123456789ABCDEF";
    /* Always emit 2 × 16-byte rows. Caller passes 32 bytes. */
    for (size_t i = 0; i < 16u && i < n; ++i) {
        line1[i * 2 + 0] = HD[bytes[i] >> 4];
        line1[i * 2 + 1] = HD[bytes[i] & 0x0Fu];
    }
    line1[32] = '\0';
    if (n >= 32u) {
        for (size_t i = 0; i < 16u; ++i) {
            line2[i * 2 + 0] = HD[bytes[i + 16u] >> 4];
            line2[i * 2 + 1] = HD[bytes[i + 16u] & 0x0Fu];
        }
        line2[32] = '\0';
    } else {
        line2[0] = '\0';
    }
    (void)cap;
}

static void render(void)
{
    phoneapi_config_security_t sec;
    bool have = phoneapi_cache_get_config_security(&sec);
    lv_label_set_text(s.header, "T-7 配對碼  本機公鑰");

    if (!have || sec.public_key_len != 32u) {
        lv_label_set_text(s.hex_label, "HEX:");
        lv_label_set_text(s.hex_line1, "(等待 cascade Security config)");
        lv_label_set_text(s.hex_line2, "");
        lv_label_set_text(s.b64_label, "BASE64:");
        lv_label_set_text(s.b64_line1, "");
        lv_label_set_text(s.b64_line2, "");
        lv_label_set_text(s.status,
            "BACK 工具    (cache miss / public_key_len != 32)");
        return;
    }

    /* HEX dump 32 bytes split 2 × 16 bytes (32 chars each) */
    char hex_line1[33], hex_line2[33];
    format_hex_split(sec.public_key, 32u, hex_line1, hex_line2, 33u);
    memcpy(s.hex_buf, hex_line1, 32);
    memcpy(s.hex_buf + 32, hex_line2, 32);
    s.hex_buf[64] = '\0';
    lv_label_set_text(s.hex_label, "HEX (32 B):");
    lv_label_set_text(s.hex_line1, hex_line1);
    lv_label_set_text(s.hex_line2, hex_line2);

    /* Standard base64 (44 chars with `=` padding for 32-byte input).
     * `meshtastic --info` prints publicKey in this exact form, so we
     * can byte-equal compare for verification. */
    size_t bn = base64_std_encode(sec.public_key, 32u,
                                   s.b64_buf, sizeof(s.b64_buf));
    if (bn == 0u) {
        lv_label_set_text(s.b64_line1, "(base64 buffer overflow)");
        lv_label_set_text(s.b64_line2, "");
    } else {
        /* 44 chars wraps at 32 → split 32 + remainder. */
        char l1[34], l2[16];
        size_t n1 = (bn > 32u) ? 32u : bn;
        memcpy(l1, s.b64_buf, n1);
        l1[n1] = '\0';
        if (bn > 32u) {
            memcpy(l2, s.b64_buf + 32u, bn - 32u);
            l2[bn - 32u] = '\0';
        } else {
            l2[0] = '\0';
        }
        lv_label_set_text(s.b64_label, "BASE64 (44 chars):");
        lv_label_set_text(s.b64_line1, l1);
        lv_label_set_text(s.b64_line2, l2);
    }

    char st[64];
    snprintf(st, sizeof(st), "BACK 工具   admin_chan=%s",
             sec.admin_channel_enabled ? "ON" : "off");
    lv_label_set_text(s.status, st);

    TRACE("pair", "render", "have=%u len=%u",
          (unsigned)have, (unsigned)sec.public_key_len);
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header    = make_label(panel,  4,   0, PANEL_W - 8, HEADER_H,
                             ui_color(UI_COLOR_TEXT_SECONDARY));
    s.hex_label = make_label(panel,  4,  20, PANEL_W - 8, LABEL_H,
                             ui_color(UI_COLOR_TEXT_SECONDARY));
    s.hex_line1 = make_label(panel,  4,  48, PANEL_W - 8, ROW_H,
                             ui_color(UI_COLOR_TEXT_PRIMARY));
    s.hex_line2 = make_label(panel,  4,  72, PANEL_W - 8, ROW_H,
                             ui_color(UI_COLOR_TEXT_PRIMARY));
    s.b64_label = make_label(panel,  4, 100, PANEL_W - 8, LABEL_H,
                             ui_color(UI_COLOR_TEXT_SECONDARY));
    s.b64_line1 = make_label(panel,  4, 124, PANEL_W - 8, ROW_H,
                             ui_color(UI_COLOR_TEXT_PRIMARY));
    s.b64_line2 = make_label(panel,  4, 148, PANEL_W - 8, ROW_H,
                             ui_color(UI_COLOR_TEXT_PRIMARY));
    s.status    = make_label(panel,  4, 200, PANEL_W - 8, ROW_H,
                             ui_color(UI_COLOR_TEXT_SECONDARY));

    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.hex_label = NULL;
    s.hex_line1 = s.hex_line2 = NULL;
    s.b64_label = NULL;
    s.b64_line1 = s.b64_line2 = NULL;
    s.status = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_BACK) {
        view_router_navigate(VIEW_ID_TOOLS);
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

static const view_descriptor_t PAIRING_DESC = {
    .id      = VIEW_ID_T7_PAIRING,
    .name    = "pairing",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 工具" },
};

const view_descriptor_t *pairing_view_descriptor(void)
{
    return &PAIRING_DESC;
}
