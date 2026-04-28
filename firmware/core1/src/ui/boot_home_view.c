/* boot_home_view.c — see boot_home_view.h.
 *
 * Phase 1 minimum-viable layout: 4 telemetry rows (identity, GPS, power,
 * network) + message zone (1 row + header) + event zone (1 row + header).
 * The full spec calls for 5 telemetry rows + 3 messages + 1 event, but
 * Phase 1 panel height is 224 px (240 - status bar 16) which doesn't fit
 * the entire spec; iterate in a later phase. The router hides the hint
 * bar while this view is active.
 *
 * Refresh strategy: 1 Hz tick refreshes telemetry; messages list re-pulls
 * each tick from `phoneapi_msgs_take_at_offset(0)`. The view is
 * non-interactive in this baseline (D-pad / OK / etc. are routed to
 * apply() but ignored — focus model lives in a future phase).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_home_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "global/hint_bar.h"

#include "FreeRTOS.h"
#include "task.h"

#include "bq25622.h"
#include "teseo_liv3fl.h"
#include "phoneapi_cache.h"

/* ── Geometry ────────────────────────────────────────────────────────── */
#define PANEL_W       320
#define PANEL_H       224     /* full content area below status bar */
#define ROW_H          20

/* y offsets within the panel */
#define Y_IDENT         0
#define Y_GPS          (Y_IDENT + ROW_H)
#define Y_POWER        (Y_GPS   + ROW_H)
#define Y_NET          (Y_POWER + ROW_H)
#define Y_DIV1         (Y_NET   + ROW_H)            /* 80 */
#define Y_MSG_HDR      (Y_DIV1  + 2)                /* 82 */
#define Y_MSG_ROW      (Y_MSG_HDR + 18)             /* 100 */
#define Y_DIV2         (Y_MSG_ROW + ROW_H + 1)      /* 121 */
#define Y_EVT_HDR      (Y_DIV2  + 2)                /* 123 */
#define Y_EVT_ROW      (Y_EVT_HDR + 18)             /* 141 */

typedef struct {
    lv_obj_t *bg;
    lv_obj_t *ident_lbl;
    lv_obj_t *gps_lbl;
    lv_obj_t *power_lbl;
    lv_obj_t *net_lbl;
    lv_obj_t *msg_hdr;
    lv_obj_t *msg_row;
    lv_obj_t *evt_hdr;
    lv_obj_t *evt_row;
    uint32_t  last_refresh_ms;
} boot_home_t;

static boot_home_t s;

/* ── Helpers ─────────────────────────────────────────────────────────── */

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

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ── Refresh routines ────────────────────────────────────────────────── */

static void refresh_ident(void)
{
    phoneapi_my_info_t mi;
    char buf[64];
    if (phoneapi_cache_get_my_info(&mi) && mi.my_node_num != 0) {
        snprintf(buf, sizeof(buf), "Node !%08lx",
                 (unsigned long)mi.my_node_num);
    } else {
        snprintf(buf, sizeof(buf), "Node (waiting for cascade)");
    }
    lv_label_set_text(s.ident_lbl, buf);
}

static void refresh_gps(void)
{
    const teseo_state_t *t = teseo_get_state();
    char buf[64];
    if (!t || !t->online) {
        snprintf(buf, sizeof(buf), "GPS  offline");
    } else if (t->fix_valid) {
        snprintf(buf, sizeof(buf), "GPS  %d.%06d, %d.%06d  sats=%u",
                 (int)(t->lat_e7 / 10000000),
                 (int)((t->lat_e7 < 0 ? -t->lat_e7 : t->lat_e7) % 10000000) / 10,
                 (int)(t->lon_e7 / 10000000),
                 (int)((t->lon_e7 < 0 ? -t->lon_e7 : t->lon_e7) % 10000000) / 10,
                 (unsigned)t->num_sats);
    } else {
        snprintf(buf, sizeof(buf), "GPS  searching  sats=%u",
                 (unsigned)t->num_sats);
    }
    lv_label_set_text(s.gps_lbl, buf);
}

static void refresh_power(void)
{
    const bq25622_state_t *b = bq25622_get_state();
    char buf[64];
    if (!b || !b->online) {
        snprintf(buf, sizeof(buf), "PWR  charger offline");
    } else {
        int pct = ((int)b->vbat_mv - 3300) * 100 / (4200 - 3300);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        const char *st = (b->vbus_stat != 0) ? "chg" : "bat";
        snprintf(buf, sizeof(buf), "PWR  %s %d%%  %u.%02uV  %dmA",
                 st, pct,
                 (unsigned)(b->vbat_mv / 1000),
                 (unsigned)((b->vbat_mv % 1000) / 10),
                 (int)b->ibat_ma);
    }
    lv_label_set_text(s.power_lbl, buf);
}

static void refresh_net(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "NET  nodes=%lu  msgs=%lu",
             (unsigned long)phoneapi_cache_node_count(),
             (unsigned long)phoneapi_msgs_count());
    lv_label_set_text(s.net_lbl, buf);
}

static void refresh_msg_row(void)
{
    phoneapi_text_msg_t m;
    if (phoneapi_msgs_count() > 0 && phoneapi_msgs_take_at_offset(0, &m)) {
        char preview[64];
        size_t n = m.text_len < sizeof(preview) - 1 ? m.text_len : sizeof(preview) - 1;
        memcpy(preview, m.text, n);
        preview[n] = '\0';
        char buf[96];
        snprintf(buf, sizeof(buf), "  !%08lx  %s",
                 (unsigned long)m.from_node_id, preview);
        lv_label_set_text(s.msg_row, buf);
    } else {
        lv_label_set_text(s.msg_row, "  (no messages)");
    }
}

static void refresh_evt_row(void)
{
    /* Event diff source not wired yet — Phase 1 placeholder. */
    lv_label_set_text(s.evt_row, "  (no events)");
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    lv_color_t white = ui_color(UI_COLOR_TEXT_PRIMARY);
    lv_color_t dim   = ui_color(UI_COLOR_TEXT_SECONDARY);
    lv_color_t green = ui_color(UI_COLOR_ACCENT_SUCCESS);

    s.ident_lbl = make_label(panel, 4, Y_IDENT, PANEL_W - 8, ROW_H, white);
    s.gps_lbl   = make_label(panel, 4, Y_GPS,   PANEL_W - 8, ROW_H, white);
    s.power_lbl = make_label(panel, 4, Y_POWER, PANEL_W - 8, ROW_H, white);
    s.net_lbl   = make_label(panel, 4, Y_NET,   PANEL_W - 8, ROW_H, white);

    s.msg_hdr = make_label(panel, 4, Y_MSG_HDR, PANEL_W - 8, 18, dim);
    lv_label_set_text(s.msg_hdr, "v Inbox");
    lv_obj_set_style_text_color(s.msg_hdr, green, 0);

    s.msg_row = make_label(panel, 4, Y_MSG_ROW, PANEL_W - 8, ROW_H, white);

    s.evt_hdr = make_label(panel, 4, Y_EVT_HDR, PANEL_W - 8, 18, dim);
    lv_label_set_text(s.evt_hdr, "o Events");

    s.evt_row = make_label(panel, 4, Y_EVT_ROW, PANEL_W - 8, ROW_H, white);

    /* Initial paint */
    refresh_ident();
    refresh_gps();
    refresh_power();
    refresh_net();
    refresh_msg_row();
    refresh_evt_row();

    /* L-0 hides the hint bar per spec. */
    hint_bar_clear();
}

static void destroy(void)
{
    s.ident_lbl = s.gps_lbl = s.power_lbl = s.net_lbl = NULL;
    s.msg_hdr = s.msg_row = s.evt_hdr = s.evt_row = NULL;
}

static void apply(const key_event_t *ev)
{
    /* Phase 1: non-interactive. FUNC short → launcher is handled by
     * router; everything else is intentionally ignored here. */
    (void)ev;
}

static void refresh(void)
{
    uint32_t t = now_ms();
    if (t - s.last_refresh_ms < 1000) return;
    s.last_refresh_ms = t;

    refresh_ident();
    refresh_gps();
    refresh_power();
    refresh_net();
    refresh_msg_row();
    refresh_evt_row();
}

static const view_descriptor_t BOOT_HOME_DESC = {
    .id      = VIEW_ID_BOOT_HOME,
    .name    = "boot_home",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *boot_home_view_descriptor(void)
{
    return &BOOT_HOME_DESC;
}
