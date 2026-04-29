/* boot_home_view.c — see boot_home_view.h.
 *
 * Spec-aligned layout (docs/ui/20-launcher-home.md):
 *   5 telemetry rows  — identity / GPS / power / environment / network
 *   3 message rows    — most-recent inbox previews
 *   1 event row       — node up/down/position-request feed (stub)
 *
 * Total content height = 5×20 + 18 + 3×20 + 18 + 1×20 + 2 dividers =
 *   100 + 18 + 60 + 18 + 20 + 2 = 218 px (within 224 px panel; trailing
 *   6 px stays clear matching the spec's "5 px 留白" target).
 *
 * Refresh strategy: 1 Hz tick refreshes telemetry; message rows re-pull
 * each tick from `phoneapi_msgs_take_at_offset(0..2)`. Non-interactive
 * baseline — focus / scroll model lives in a future phase.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "boot_home_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"

#include "FreeRTOS.h"
#include "task.h"

#include "bq25622.h"
#include "teseo_liv3fl.h"
#include "lps22hh.h"
#include "phoneapi_cache.h"

/* ── Geometry ────────────────────────────────────────────────────────── */
#define PANEL_W       320
#define PANEL_H       224     /* full content area below status bar */
#define ROW_H          20
#define HDR_H          18

/* y offsets within the panel — spec mapping */
#define Y_IDENT         0                            /* 0..19   identity+date */
#define Y_GPS          (Y_IDENT + ROW_H)             /* 20..39  GPS           */
#define Y_POWER        (Y_GPS   + ROW_H)             /* 40..59  power         */
#define Y_ENV          (Y_POWER + ROW_H)             /* 60..79  environment   */
#define Y_NET          (Y_ENV   + ROW_H)             /* 80..99  network       */
#define Y_DIV1         (Y_NET   + ROW_H)             /* 100     divider 1 px  */
#define Y_MSG_HDR      (Y_DIV1  + 1)                 /* 101..118 msg header   */
#define Y_MSG_ROW0     (Y_MSG_HDR + HDR_H)           /* 119..138 msg 0        */
#define Y_MSG_ROW1     (Y_MSG_ROW0 + ROW_H)          /* 139..158 msg 1        */
#define Y_MSG_ROW2     (Y_MSG_ROW1 + ROW_H)          /* 159..178 msg 2        */
#define Y_DIV2         (Y_MSG_ROW2 + ROW_H)          /* 179     divider 1 px  */
#define Y_EVT_HDR      (Y_DIV2  + 1)                 /* 180..197 evt header   */
#define Y_EVT_ROW      (Y_EVT_HDR + HDR_H)           /* 198..217 evt 0        */

typedef struct {
    lv_obj_t *bg;
    lv_obj_t *ident_lbl;
    lv_obj_t *gps_lbl;
    lv_obj_t *power_lbl;
    lv_obj_t *env_lbl;
    lv_obj_t *net_lbl;
    lv_obj_t *msg_hdr;
    lv_obj_t *msg_rows[3];
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

static void refresh_env(void)
{
    char buf[64];
    const lps22hh_state_t *p = lps22hh_get_state();
    const teseo_state_t   *t = teseo_get_state();
    if (p && p->online) {
        unsigned hpa_int = p->pressure_hpa_x100 / 100u;
        unsigned hpa_dec = p->pressure_hpa_x100 % 100u;
        int      tc      = p->temperature_cx10 / 10;
        int      td      = (p->temperature_cx10 < 0
                            ? -p->temperature_cx10 : p->temperature_cx10) % 10;
        if (t && t->fix_valid) {
            snprintf(buf, sizeof(buf), "ENV  %d.%01dC %u.%02uhPa  alt=%dm",
                     tc, td, hpa_int, hpa_dec, (int)t->altitude_m);
        } else {
            snprintf(buf, sizeof(buf), "ENV  %d.%01dC %u.%02uhPa",
                     tc, td, hpa_int, hpa_dec);
        }
    } else {
        snprintf(buf, sizeof(buf), "ENV  baro offline");
    }
    lv_label_set_text(s.env_lbl, buf);
}

static void refresh_net(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "NET  nodes=%lu  msgs=%lu",
             (unsigned long)phoneapi_cache_node_count(),
             (unsigned long)phoneapi_msgs_count());
    lv_label_set_text(s.net_lbl, buf);
}

static void refresh_msg_rows(void)
{
    /* Render 3 most-recent message previews. phoneapi_msgs_take_at_offset
     * accepts 0 = newest. Empty rows show " " (clears stale text from
     * before a new message arrived). */
    uint32_t total = phoneapi_msgs_count();
    for (int i = 0; i < 3; ++i) {
        phoneapi_text_msg_t m;
        if ((uint32_t)i < total &&
            phoneapi_msgs_take_at_offset((uint32_t)i, &m)) {
            char preview[64];
            size_t n = m.text_len < sizeof(preview) - 1
                       ? m.text_len : sizeof(preview) - 1;
            memcpy(preview, m.text, n);
            preview[n] = '\0';
            char buf[96];
            snprintf(buf, sizeof(buf), "  !%08lx  %s",
                     (unsigned long)m.from_node_id, preview);
            lv_label_set_text(s.msg_rows[i], buf);
        } else if (i == 0) {
            lv_label_set_text(s.msg_rows[i], "  (no messages)");
        } else {
            lv_label_set_text(s.msg_rows[i], "");
        }
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
    s.env_lbl   = make_label(panel, 4, Y_ENV,   PANEL_W - 8, ROW_H, white);
    s.net_lbl   = make_label(panel, 4, Y_NET,   PANEL_W - 8, ROW_H, white);

    s.msg_hdr = make_label(panel, 4, Y_MSG_HDR, PANEL_W - 8, HDR_H, dim);
    lv_label_set_text(s.msg_hdr, "v Inbox");
    lv_obj_set_style_text_color(s.msg_hdr, green, 0);

    s.msg_rows[0] = make_label(panel, 4, Y_MSG_ROW0, PANEL_W - 8, ROW_H, white);
    s.msg_rows[1] = make_label(panel, 4, Y_MSG_ROW1, PANEL_W - 8, ROW_H, white);
    s.msg_rows[2] = make_label(panel, 4, Y_MSG_ROW2, PANEL_W - 8, ROW_H, white);

    s.evt_hdr = make_label(panel, 4, Y_EVT_HDR, PANEL_W - 8, HDR_H, dim);
    lv_label_set_text(s.evt_hdr, "o Events");

    s.evt_row = make_label(panel, 4, Y_EVT_ROW, PANEL_W - 8, ROW_H, white);

    /* Initial paint */
    refresh_ident();
    refresh_gps();
    refresh_power();
    refresh_env();
    refresh_net();
    refresh_msg_rows();
    refresh_evt_row();
}

static void destroy(void)
{
    s.ident_lbl = s.gps_lbl = s.power_lbl = s.env_lbl = s.net_lbl = NULL;
    s.msg_hdr = s.evt_hdr = s.evt_row = NULL;
    s.msg_rows[0] = s.msg_rows[1] = s.msg_rows[2] = NULL;
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
    refresh_env();
    refresh_net();
    refresh_msg_rows();
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
    .hints   = { NULL, NULL, NULL },  /* L-0 hides the hint bar per spec */
};

const view_descriptor_t *boot_home_view_descriptor(void)
{
    return &BOOT_HOME_DESC;
}
