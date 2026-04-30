/* map_nav_view.c — see map_nav_view.h.
 *
 * D-6: full-screen "navigate to peer" view. v1 displays:
 *
 *   y   0..15   header   "D-6 -> <peer-name>" or status
 *   y  32..63   bearing  big-ish cardinal text + degrees, accent colour
 *   y 128..151  range    "range    1.234 km" / "12 m"
 *   y 152..175  bearing  "bearing    045 deg"
 *   y 176..199  ETA      "ETA       mm:ss" / "--" / ">1d"
 *   y 200..223  speed    "speed     5.2 km/h" + freshness hint
 *
 * North reference: GPS true north (DEC-3 in plan map-ppi-radar-v1.md;
 * magnetic-north + LIS2MDL is v2). ETA source: Teseo RMC speed (DEC-9).
 *
 * Lost-target handling: if cache loses the peer (eviction, NodeDB
 * config-replay) or the peer's last_position drops to epoch=0, the
 * header switches to "D-6 LOST" and the metrics freeze on whatever
 * was last seen — user must press BACK to return. v1 does not auto-
 * back to keep the timer surface tiny; auto-back can land in v2 if
 * users find the manual exit awkward.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "map_nav_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"

#include "phoneapi_cache.h"
#include "teseo_liv3fl.h"
#include "node_alias.h"
#include "map_view.h"

#define HEADER_H        16
#define BEARING_TOP     32
#define BEARING_H       32
#define RANGE_TOP      128
#define ROW_H           24
#define PANEL_W        320
#define PANEL_H        224

typedef struct {
    lv_obj_t *header;
    lv_obj_t *bearing_big;     /* big cardinal + deg, accent_focus       */
    lv_obj_t *range_lbl;
    lv_obj_t *bearing_lbl;
    lv_obj_t *eta_lbl;
    lv_obj_t *speed_lbl;

    uint32_t  target_num;      /* 0 = no target                          */
    uint32_t  last_render_ms;
} map_nav_t;

static map_nav_t s;

/* Target override published by external entry paths (e.g. nodes_view
 * C-3 OP_NAVIGATE). create() pulls from here first, falling back to
 * map_view_get_nav_target() if zero. */
static uint32_t s_pending_target;

/* ── Helpers ────────────────────────────────────────────────────────── */

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

/* Flat-earth (DEC-2) range + bearing. Same approximation as map_view's
 * project_peer() but without the polar-pixel mapping. */
static void compute_range_bearing(int32_t my_lat_e7,  int32_t my_lon_e7,
                                  int32_t pe_lat_e7,  int32_t pe_lon_e7,
                                  int    *out_range_m, int *out_az_deg)
{
    const float DEG_PER_E7 = 1e-7f;
    const float DEG_TO_RAD = (float)M_PI / 180.0f;
    const float R_EARTH    = 6371000.0f;

    float lat0    = (float)my_lat_e7  * DEG_PER_E7 * DEG_TO_RAD;
    float dlat    = (float)(pe_lat_e7 - my_lat_e7) * DEG_PER_E7 * DEG_TO_RAD;
    float dlon    = (float)(pe_lon_e7 - my_lon_e7) * DEG_PER_E7 * DEG_TO_RAD;
    float coslat  = cosf(lat0);
    float dx      = coslat * dlon;
    float dy      = dlat;
    float range_m = R_EARTH * sqrtf(dx * dx + dy * dy);
    float az_rad  = atan2f(dx, dy);
    if (az_rad < 0.0f) az_rad += 2.0f * (float)M_PI;
    int   az_deg  = (int)(az_rad * 180.0f / (float)M_PI);
    if (az_deg >= 360) az_deg -= 360;
    if (out_range_m) *out_range_m = (int)range_m;
    if (out_az_deg)  *out_az_deg  = az_deg;
}

static const char *cardinal_for(int az_deg)
{
    /* 8 sectors, 45° each, with ±22.5° around each cardinal. */
    static const char *names[] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW"
    };
    int sec = ((az_deg + 22) / 45) % 8;
    if (sec < 0) sec = 0;
    if (sec >= 8) sec = 7;
    return names[sec];
}

static void format_range(int range_m, char *buf, size_t cap)
{
    if (range_m < 1000) {
        snprintf(buf, cap, "%d m", range_m);
    } else if (range_m < 100000) {
        snprintf(buf, cap, "%d.%03d km",
                 range_m / 1000, range_m % 1000);
    } else {
        snprintf(buf, cap, "%d km", range_m / 1000);
    }
}

static void format_eta(int range_m, int speed_kmh_x10,
                       char *buf, size_t cap)
{
    /* DEC-9: speed_kmh_x10 < 10  (< 1 km/h)  -> stationary, no ETA. */
    if (speed_kmh_x10 < 10) {
        snprintf(buf, cap, "--  (stationary)");
        return;
    }
    /* sec = range_m / (speed_kmh / 3.6) = range_m * 36 / speed_kmh_x10 */
    int sec = (int)((int64_t)range_m * 36 / (int64_t)speed_kmh_x10);
    if (sec < 60) {
        snprintf(buf, cap, "%d s", sec);
    } else if (sec < 3600) {
        snprintf(buf, cap, "%d:%02d", sec / 60, sec % 60);
    } else if (sec < 86400) {
        snprintf(buf, cap, "%d:%02d:%02d",
                 sec / 3600, (sec / 60) % 60, sec % 60);
    } else {
        snprintf(buf, cap, ">1d");
    }
}

/* ── Render ─────────────────────────────────────────────────────────── */

static void render(void)
{
    char hdr[80], bearing_big[32];
    char range_s[40], bearing_s[40], eta_s[40], speed_s[40];

    if (s.target_num == 0u) {
        lv_label_set_text(s.header,      "D-6 (no target)");
        lv_label_set_text(s.bearing_big, "--");
        lv_label_set_text(s.range_lbl,   "Lock a peer in D-1 first");
        lv_label_set_text(s.bearing_lbl, "");
        lv_label_set_text(s.eta_lbl,     "");
        lv_label_set_text(s.speed_lbl,   "BACK to return");
        return;
    }

    phoneapi_node_t e;
    bool have_node = phoneapi_cache_get_node_by_id(s.target_num, &e);
    if (!have_node || e.last_position.epoch == 0u) {
        char nm[16];
        node_alias_format_display(s.target_num,
                                  have_node ? e.short_name : NULL,
                                  nm, sizeof(nm));
        snprintf(hdr, sizeof(hdr), "D-6 LOST  %s", nm);
        lv_label_set_text(s.header,      hdr);
        lv_label_set_text(s.bearing_big, "??");
        lv_label_set_text(s.range_lbl,   "peer no longer in cache");
        lv_label_set_text(s.bearing_lbl, "");
        lv_label_set_text(s.eta_lbl,     "");
        lv_label_set_text(s.speed_lbl,   "BACK to return");
        return;
    }

    const teseo_state_t *t = teseo_get_state();
    char nm[16];
    node_alias_format_display(s.target_num, e.short_name, nm, sizeof(nm));
    snprintf(hdr, sizeof(hdr), "D-6 -> %s !%08lx",
             nm, (unsigned long)s.target_num);
    lv_label_set_text(s.header, hdr);

    if (t == NULL || !t->fix_valid) {
        lv_label_set_text(s.bearing_big, "GPS searching");
        lv_label_set_text(s.range_lbl,   "");
        lv_label_set_text(s.bearing_lbl, "");
        lv_label_set_text(s.eta_lbl,     "");
        lv_label_set_text(s.speed_lbl,   "");
        return;
    }

    int range_m, az_deg;
    compute_range_bearing(t->lat_e7, t->lon_e7,
                          e.last_position.lat_e7,
                          e.last_position.lon_e7,
                          &range_m, &az_deg);
    const char *card = cardinal_for(az_deg);
    snprintf(bearing_big, sizeof(bearing_big), "%-2s  %3d°", card, az_deg);
    lv_label_set_text(s.bearing_big, bearing_big);

    char rng_buf[24];
    format_range(range_m, rng_buf, sizeof(rng_buf));
    snprintf(range_s, sizeof(range_s), "range    %s", rng_buf);
    lv_label_set_text(s.range_lbl, range_s);

    snprintf(bearing_s, sizeof(bearing_s),
             "bearing  %s  %d°", card, az_deg);
    lv_label_set_text(s.bearing_lbl, bearing_s);

    char eta_buf[24];
    format_eta(range_m, t->speed_kmh_x10, eta_buf, sizeof(eta_buf));
    snprintf(eta_s, sizeof(eta_s), "ETA      %s", eta_buf);
    lv_label_set_text(s.eta_lbl, eta_s);

    snprintf(speed_s, sizeof(speed_s),
             "speed    %d.%01d km/h",
             (int)(t->speed_kmh_x10 / 10),
             (int)(t->speed_kmh_x10 % 10));
    lv_label_set_text(s.speed_lbl, speed_s);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    /* Pull pending target if set, else fall back to map_view's last
     * D-1 OK pick. Then clear the pending so the next entry without
     * an explicit set falls through to map_view's value. */
    uint32_t target = s_pending_target;
    if (target == 0u) target = map_view_get_nav_target();
    s_pending_target = 0u;

    memset(&s, 0, sizeof(s));
    s.target_num = target;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    /* Big bearing display — same font (16 px) but rendered prominently
     * via accent colour and centred in a 32 px tall band. Future v2
     * could swap in a 24 px or 32 px font if MIEF gains larger sizes. */
    s.bearing_big = make_label(panel, 0, BEARING_TOP,
                               PANEL_W, BEARING_H,
                               ui_color(UI_COLOR_ACCENT_FOCUS));
    lv_obj_set_style_text_align(s.bearing_big, LV_TEXT_ALIGN_CENTER, 0);

    s.range_lbl   = make_label(panel, 4, RANGE_TOP + 0 * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    s.bearing_lbl = make_label(panel, 4, RANGE_TOP + 1 * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    s.eta_lbl     = make_label(panel, 4, RANGE_TOP + 2 * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    s.speed_lbl   = make_label(panel, 4, RANGE_TOP + 3 * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_SECONDARY));

    TRACE("mapnav", "create", "target=0x%08lx",
          (unsigned long)s.target_num);
    render();
}

static void destroy(void)
{
    TRACE_BARE("mapnav", "destroy");
    s.header = NULL;
    s.bearing_big = NULL;
    s.range_lbl = s.bearing_lbl = s.eta_lbl = s.speed_lbl = NULL;
    /* target_num preserved across destroy so an LRU-evicted view
     * comes back showing the same peer. */
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_BACK) {
        TRACE_BARE("mapnav", "back");
        view_router_navigate(VIEW_ID_MAP);
    }
}

static void refresh(void)
{
    if (s.header == NULL) return;
    /* 1 Hz repaint — gates wall-time only; teseo_get_state() returns
     * the same pointer continuously, so no change-seq comparison is
     * useful here. (Cache-side change_seq IS bumped on
     * upsert_node / set_last_position; we could gate on it for the
     * 'lost' path, but the 1 Hz wall-time tick is enough.) */
    uint32_t t = now_ms();
    if (t - s.last_render_ms < 1000u) return;
    s.last_render_ms = t;
    render();
}

static const view_descriptor_t MAP_NAV_DESC = {
    .id      = VIEW_ID_MAP_NAV,
    .name    = "map_nav",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 地圖" },
};

const view_descriptor_t *map_nav_view_descriptor(void)
{
    return &MAP_NAV_DESC;
}

void map_nav_view_set_target(uint32_t node_num)
{
    s_pending_target = node_num;
}
