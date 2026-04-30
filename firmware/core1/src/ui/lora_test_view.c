/* lora_test_view.c — see lora_test_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header   "T-5 LoRa 自我測試"
 *   y  16..183  7 rows × 24 px metrics
 *   y 200..223  hint
 *
 * Refresh gated on lora_test_log_change_seq + 1 Hz fallback so the
 * "Last RX age" stays current.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lora_test_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "lora_test_log.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define ROW_H           24
#define ROW_COUNT        7
#define HINT_TOP       200
#define HINT_H          24
#define PANEL_W        320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *hint;
    uint32_t  last_change_seq;
    uint32_t  last_refresh_ms;
} ltest_t;

static ltest_t s;

/* SWD diag exports — host test reads these directly to avoid
 * walking the lora_test_log struct layout. */
volatile uint32_t g_t5_rx_count    __attribute__((used)) = 0u;
volatile uint32_t g_t5_ack_count   __attribute__((used)) = 0u;
volatile uint32_t g_t5_nack_count  __attribute__((used)) = 0u;
volatile int8_t   g_t5_last_snr_x4 __attribute__((used)) = INT8_MIN;
volatile int16_t  g_t5_last_rssi   __attribute__((used)) = 0;
volatile uint32_t g_t5_queue_free  __attribute__((used)) = 0u;
volatile uint32_t g_t5_queue_max   __attribute__((used)) = 0u;

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

static void render(void)
{
    const lora_test_state_t *st = lora_test_log_get();
    char buf[80];

    lv_label_set_text(s.header, "T-5 LoRa 自我測試  (passive metrics)");

    snprintf(buf, sizeof(buf), "RX packets   :  %lu", (unsigned long)st->rx_count);
    lv_label_set_text(s.rows[0], buf);

    if (st->last_rx_snr_x4 == INT8_MIN) {
        snprintf(buf, sizeof(buf), "Last RX SNR  :  --   RSSI: --");
    } else {
        int s4 = st->last_rx_snr_x4;
        int sign = (s4 < 0) ? -1 : 1;
        int abs4 = s4 * sign;
        snprintf(buf, sizeof(buf),
                 "Last RX SNR  :  %s%d.%d dB  RSSI: %d",
                 sign < 0 ? "-" : "+", abs4 / 4, (abs4 % 4) * 25,
                 (int)st->last_rx_rssi);
    }
    lv_label_set_text(s.rows[1], buf);

    snprintf(buf, sizeof(buf), "TX queued    :  %lu", (unsigned long)st->tx_status_count);
    lv_label_set_text(s.rows[2], buf);

    snprintf(buf, sizeof(buf), "ACK delivered:  %lu", (unsigned long)st->ack_count);
    lv_label_set_text(s.rows[3], buf);

    snprintf(buf, sizeof(buf), "NACK / errors:  %lu", (unsigned long)st->nack_count);
    lv_label_set_text(s.rows[4], buf);

    if (st->queue_maxlen > 0u) {
        snprintf(buf, sizeof(buf), "Queue free   :  %lu / %lu",
                 (unsigned long)st->queue_free,
                 (unsigned long)st->queue_maxlen);
    } else {
        snprintf(buf, sizeof(buf), "Queue free   :  --");
    }
    lv_label_set_text(s.rows[5], buf);

    if (st->last_ack_pid != 0u) {
        snprintf(buf, sizeof(buf), "Last ACK     :  pid=%#lx err=%u",
                 (unsigned long)st->last_ack_pid, (unsigned)st->last_ack_err);
    } else {
        snprintf(buf, sizeof(buf), "Last ACK     :  --");
    }
    lv_label_set_text(s.rows[6], buf);

    /* Update SWD diag exports. */
    g_t5_rx_count    = st->rx_count;
    g_t5_ack_count   = st->ack_count;
    g_t5_nack_count  = st->nack_count;
    g_t5_last_snr_x4 = st->last_rx_snr_x4;
    g_t5_last_rssi   = st->last_rx_rssi;
    g_t5_queue_free  = st->queue_free;
    g_t5_queue_max   = st->queue_maxlen;

    TRACE("ltst", "render", "rx=%lu ack=%lu nack=%lu",
          (unsigned long)st->rx_count,
          (unsigned long)st->ack_count,
          (unsigned long)st->nack_count);
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    for (int i = 0; i < ROW_COUNT; ++i) {
        s.rows[i] = make_label(panel, 4, HEADER_H + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.hint = make_label(panel, 4, HINT_TOP, PANEL_W - 8, HINT_H,
                        ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.hint, "BACK 工具   (active loopback v2)");

    render();
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < ROW_COUNT; ++i) s.rows[i] = NULL;
    s.hint = NULL;
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
    uint32_t cur = lora_test_log_change_seq();
    uint32_t t   = now_ms();
    if (cur == s.last_change_seq && (t - s.last_refresh_ms) < 1000u) return;
    s.last_change_seq = cur;
    s.last_refresh_ms = t;
    render();
}

static const view_descriptor_t LORA_TEST_DESC = {
    .id      = VIEW_ID_T5_LORA_TEST,
    .name    = "lora_test",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 工具" },
};

const view_descriptor_t *lora_test_view_descriptor(void)
{
    return &LORA_TEST_DESC;
}
