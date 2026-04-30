/* gnss_sky_view.c — see gnss_sky_view.h.
 *
 * Polar projection r = (90 - elev_deg) * R/90.  Azimuth measured from
 * north clockwise (NMEA convention) so x = cx + r*sin(az), y = cy -
 * r*cos(az).  Per-sat label uses the PRN as a 2-char tag (`%2u`) and
 * is coloured by C/N0:
 *   ≥ 40  green (UI_COLOR_ACCENT_SUCCESS)
 *   30–39 white (UI_COLOR_TEXT_PRIMARY)
 *   < 30  yellow (UI_COLOR_WARN_YELLOW)
 *   = 0   dim grey (UI_COLOR_TEXT_SECONDARY) — visible but not tracked
 *
 * No lv_canvas — rings are 3 lv_obj_t with full-radius border (cheap),
 * sat dots are lv_label (one per visible sat, capped at TS_SATS_MAX).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss_sky_view.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "teseo_liv3fl.h"

#define HEADER_H        16
#define CHART_TOP       HEADER_H            /* 16 */
#define CHART_H         144
#define CHART_BOT       (CHART_TOP + CHART_H) /* 160 */
#define SUMMARY_TOP     (CHART_BOT + 2)     /* 162 */
#define SUMMARY_ROW_H   20
#define PANEL_W        320
#define DISC_R          70                  /* radius of outer ring */
#define DISC_CX        160                  /* horizontal centre    */
#define DISC_CY        (CHART_TOP + 2 + DISC_R)  /* 16 + 2 + 70 = 88 */

#define MAX_SATS_DRAWN  16   /* upper bound on labels in the polar disc */
#define SAT_LABEL_W     16
#define SAT_LABEL_H     14

typedef struct {
    lv_obj_t *header;
    lv_obj_t *ring30;
    lv_obj_t *ring60;
    lv_obj_t *ring90;
    lv_obj_t *cardinal[4];          /* N E S W */
    lv_obj_t *sat[MAX_SATS_DRAWN];
    lv_obj_t *summary[3];
    uint32_t  last_sat_seq;
    uint32_t  last_refresh_ms;
} sky_t;

static sky_t s;

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

static lv_obj_t *make_ring(lv_obj_t *parent, int cx, int cy, int r)
{
    /* lv_obj with circular border — radius = LV_RADIUS_CIRCLE makes
     * the corners round to half the smaller dimension, so a square
     * 2r×2r object becomes a perfect circle. */
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(o,
        ui_color(UI_COLOR_BORDER_NORMAL), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_color_t color_for_snr(uint8_t cn0)
{
    if (cn0 == 0u)  return ui_color(UI_COLOR_TEXT_SECONDARY);
    if (cn0 >= 40u) return ui_color(UI_COLOR_ACCENT_SUCCESS);
    if (cn0 >= 30u) return ui_color(UI_COLOR_TEXT_PRIMARY);
    return ui_color(UI_COLOR_WARN_YELLOW);
}

static void render_chart(const teseo_sat_view_t *sv)
{
    /* Hide all sat labels first; we only re-show those we actually
     * place this frame. */
    for (int i = 0; i < MAX_SATS_DRAWN; ++i) {
        if (s.sat[i]) lv_obj_add_flag(s.sat[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (sv == NULL || sv->count == 0u) return;

    int placed = 0;
    for (uint32_t i = 0; i < sv->count && placed < MAX_SATS_DRAWN; ++i) {
        const teseo_sat_info_t *sat = &sv->sats[i];
        /* Skip sats with no elevation/azimuth fix (Teseo emits these
         * as elevation=0 azimuth=0 sometimes during cold start). A
         * "real" sat with elev=0 az=0 (south horizon) is uncommon and
         * indistinguishable; accept the false positive. */
        if (sat->prn == 0u) continue;

        /* Polar mapping. */
        float r_norm = (90.0f - (float)sat->elevation_deg) / 90.0f;
        if (r_norm < 0.0f) r_norm = 0.0f;
        if (r_norm > 1.0f) r_norm = 1.0f;
        float r = r_norm * (float)DISC_R;
        float az_rad = ((float)sat->azimuth_deg) * (float)M_PI / 180.0f;
        int   px = DISC_CX + (int)(r * sinf(az_rad));
        int   py = DISC_CY - (int)(r * cosf(az_rad));

        /* Centre the label on (px, py). */
        int x = px - SAT_LABEL_W / 2;
        int y = py - SAT_LABEL_H / 2;

        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)sat->prn);
        lv_label_set_text(s.sat[placed], buf);
        lv_obj_set_pos(s.sat[placed], x, y);
        lv_obj_set_style_text_color(s.sat[placed],
            color_for_snr(sat->snr_dbhz), 0);
        lv_obj_clear_flag(s.sat[placed], LV_OBJ_FLAG_HIDDEN);
        placed++;
    }
}

static void render_summary(const teseo_state_t *st,
                           const teseo_sat_view_t *sv)
{
    char buf[80];

    /* Row 0: fix status + sats */
    if (st && st->fix_valid) {
        snprintf(buf, sizeof(buf),
                 "FIX OK  q=%u  used=%u  view=%u  hdop=%u.%01u",
                 (unsigned)st->fix_quality,
                 (unsigned)st->num_sats,
                 (unsigned)(sv ? sv->count : 0),
                 (unsigned)(st->hdop_x10 / 10u),
                 (unsigned)(st->hdop_x10 % 10u));
    } else if (st) {
        snprintf(buf, sizeof(buf),
                 "Searching  used=%u  view=%u",
                 (unsigned)st->num_sats,
                 (unsigned)(sv ? sv->count : 0));
    } else {
        snprintf(buf, sizeof(buf), "GNSS offline");
    }
    lv_label_set_text(s.summary[0], buf);

    /* Row 1: SNR histogram (rough quartiles).  Helps spot RF issues
     * even before a fix locks. */
    int b_strong = 0, b_med = 0, b_weak = 0, b_zero = 0;
    if (sv) {
        for (uint32_t i = 0; i < sv->count; ++i) {
            uint8_t c = sv->sats[i].snr_dbhz;
            if (c == 0u)        b_zero++;
            else if (c >= 40u)  b_strong++;
            else if (c >= 30u)  b_med++;
            else                b_weak++;
        }
    }
    snprintf(buf, sizeof(buf),
             "C/N0  >=40:%d  30-39:%d  <30:%d  none:%d",
             b_strong, b_med, b_weak, b_zero);
    lv_label_set_text(s.summary[1], buf);

    /* Row 2: hint */
    lv_label_set_text(s.summary[2], "BACK 工具");
    lv_obj_set_style_text_color(s.summary[2],
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
}

static void render(void)
{
    const teseo_state_t    *st = teseo_get_state();
    const teseo_sat_view_t *sv = teseo_get_sat_view();
    render_chart(sv);
    render_summary(st, sv);
    char buf[64];
    snprintf(buf, sizeof(buf), "T-6 GNSS Sky  view=%u",
             (unsigned)(sv ? sv->count : 0u));
    lv_label_set_text(s.header, buf);
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    /* Concentric rings at elev 30 / 60 / 90.  90 is the centre dot:
     * draw it as a tiny solid filled rect via a lv_obj with no border
     * but coloured background. */
    s.ring30 = make_ring(panel, DISC_CX, DISC_CY, DISC_R);
    s.ring60 = make_ring(panel, DISC_CX, DISC_CY, (DISC_R * 2) / 3);
    s.ring90 = make_ring(panel, DISC_CX, DISC_CY, DISC_R / 3);

    /* Cardinal labels just outside the outer ring. */
    static const struct { const char *txt; int dx; int dy; } card[4] = {
        { "N",  -4, -DISC_R - 16 },
        { "E",   DISC_R + 2, -8  },
        { "S",  -4,  DISC_R + 2  },
        { "W",  -DISC_R - 14, -8 },
    };
    for (int i = 0; i < 4; ++i) {
        s.cardinal[i] = make_label(panel,
            DISC_CX + card[i].dx,
            DISC_CY + card[i].dy,
            16, 16,
            ui_color(UI_COLOR_TEXT_SECONDARY));
        lv_label_set_text(s.cardinal[i], card[i].txt);
    }

    for (int i = 0; i < MAX_SATS_DRAWN; ++i) {
        s.sat[i] = make_label(panel, 0, 0, SAT_LABEL_W, SAT_LABEL_H,
                              ui_color(UI_COLOR_TEXT_PRIMARY));
        lv_obj_add_flag(s.sat[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 3; ++i) {
        s.summary[i] = make_label(panel, 4, SUMMARY_TOP + i * SUMMARY_ROW_H,
                                  PANEL_W - 8, SUMMARY_ROW_H,
                                  ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    render();
}

static void destroy(void)
{
    /* LVGL frees the children when the panel is destroyed; just null
     * our caches. */
    s.header = NULL;
    s.ring30 = s.ring60 = s.ring90 = NULL;
    for (int i = 0; i < 4; ++i) s.cardinal[i] = NULL;
    for (int i = 0; i < MAX_SATS_DRAWN; ++i) s.sat[i] = NULL;
    for (int i = 0; i < 3; ++i) s.summary[i] = NULL;
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
    /* Drive off Teseo's update_count rather than a wall-clock gate so
     * we don't redraw between talker cycles for no reason. */
    const teseo_sat_view_t *sv = teseo_get_sat_view();
    uint32_t cur = sv ? sv->update_count : 0u;
    uint32_t t   = now_ms();
    if (cur == s.last_sat_seq && (t - s.last_refresh_ms) < 1000u) return;
    s.last_sat_seq    = cur;
    s.last_refresh_ms = t;
    render();
}

static const view_descriptor_t GNSS_SKY_DESC = {
    .id      = VIEW_ID_GNSS_SKY,
    .name    = "gnss_sky",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 工具" },
};

const view_descriptor_t *gnss_sky_view_descriptor(void)
{
    return &GNSS_SKY_DESC;
}
