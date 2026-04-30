/* range_test_view.c — see range_test_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   title  "T-2 Range Test  total=N  module: on/off"
 *   y  16..31   header "Peer       hits  last  SNR    RSSI"
 *   y  32..199  data   7 rows × 24 px (RANGE_TEST_PEERS_MAX)
 *   y 200..223  hint   "BACK 工具" (dim)
 *
 * Refresh gated on range_test_log_change_seq() so we only re-render
 * when a new packet has been recorded (or once at create()).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "range_test_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "node_alias.h"
#include "phoneapi_cache.h"
#include "range_test_log.h"

#define HEADER_H        16
#define COLHDR_TOP      16
#define ROW_TOP         32
#define ROW_H           24
#define HINT_TOP       200
#define HINT_H          24
#define PANEL_W        320

typedef struct {
    lv_obj_t *title;
    lv_obj_t *colhdr;
    lv_obj_t *rows[RANGE_TEST_PEERS_MAX];
    lv_obj_t *hint;
    uint32_t  last_change_seq;
    uint32_t  last_refresh_ms;
} rng_t;

static rng_t s;

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

static void render_title(uint32_t total_hits)
{
    if (!s.title) return;
    /* Module enabled state — read from cache snapshot. If cascade hasn't
     * delivered ModuleConfig.RangeTest yet (cold-boot pre-config-replay),
     * label as `?` so the user knows it's not "off" specifically. */
    phoneapi_module_range_test_t m;
    bool have_m = phoneapi_cache_get_module_range_test(&m);
    const char *state = have_m ? (m.enabled ? "ON " : "OFF") : "?? ";
    char buf[64];
    snprintf(buf, sizeof(buf), "T-2 Range Test  total=%lu  mod:%s",
             (unsigned long)total_hits, state);
    lv_label_set_text(s.title, buf);
}

static void render_rows(void)
{
    if (!s.colhdr) return;
    /* Column header. */
    lv_label_set_text(s.colhdr, "Peer       hits last  SNR    RSSI");

    uint32_t n = range_test_log_count();
    /* Capture wall-clock-ish reference for an "age" feel. P2-18 means
     * we don't have RTC sync; use newest entry's epoch as the clock. */
    uint32_t now_epoch = 0u;
    for (uint32_t i = 0; i < n; ++i) {
        range_test_entry_t e;
        if (range_test_log_get(i, &e) && e.last_epoch > now_epoch) {
            now_epoch = e.last_epoch;
        }
    }

    for (uint32_t i = 0; i < RANGE_TEST_PEERS_MAX; ++i) {
        if (!s.rows[i]) continue;
        if (i >= n) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        range_test_entry_t e;
        if (!range_test_log_get(i, &e)) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        char nm[12];
        node_alias_format_display(e.node_num, NULL, nm, sizeof(nm));

        char snr_s[12];
        if (e.last_snr_x4 == INT8_MIN) {
            snprintf(snr_s, sizeof(snr_s), "--");
        } else {
            int x4   = e.last_snr_x4;
            int sign = (x4 < 0) ? -1 : 1;
            int abs4 = x4 * sign;
            snprintf(snr_s, sizeof(snr_s), "%s%d.%d",
                     sign < 0 ? "-" : "+", abs4 / 4, (abs4 % 4) * 25);
        }

        char rssi_s[10];
        if (e.last_rssi == 0) snprintf(rssi_s, sizeof(rssi_s), "--");
        else                   snprintf(rssi_s, sizeof(rssi_s), "%d", (int)e.last_rssi);

        char buf[80];
        snprintf(buf, sizeof(buf),
                 "%-9s %4lu %4lu  %-6s %s",
                 nm,
                 (unsigned long)e.hits,
                 (unsigned long)e.last_seq,
                 snr_s, rssi_s);
        lv_label_set_text(s.rows[i], buf);
    }
}

static void render(void)
{
    render_title(range_test_log_total_hits());
    render_rows();
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.title  = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    s.colhdr = make_label(panel, 4, COLHDR_TOP, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    for (uint32_t i = 0; i < RANGE_TEST_PEERS_MAX; ++i) {
        s.rows[i] = make_label(panel, 4, ROW_TOP + (int)i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.hint = make_label(panel, 4, HINT_TOP, PANEL_W - 8, HINT_H,
                        ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.hint, "BACK 工具    (S-7.3 設定模組開關 + 間隔)");

    render();
}

static void destroy(void)
{
    s.title  = NULL;
    s.colhdr = NULL;
    s.hint   = NULL;
    for (uint32_t i = 0; i < RANGE_TEST_PEERS_MAX; ++i) s.rows[i] = NULL;
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
    if (s.title == NULL) return;
    /* Re-render only when range_test_log mutates (or once a second so
     * the module-state field can pick up a fresh phoneapi_cache update
     * from a concurrent ModuleConfig replay). */
    uint32_t cur = range_test_log_change_seq();
    uint32_t t   = now_ms();
    if (cur == s.last_change_seq && (t - s.last_refresh_ms) < 1000u) return;
    s.last_change_seq  = cur;
    s.last_refresh_ms  = t;
    render();
}

static const view_descriptor_t RANGE_TEST_DESC = {
    .id      = VIEW_ID_RANGE_TEST,
    .name    = "range_test",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 工具" },
};

const view_descriptor_t *range_test_view_descriptor(void)
{
    return &RANGE_TEST_DESC;
}
