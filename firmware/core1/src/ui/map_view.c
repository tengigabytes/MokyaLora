/* map_view.c — see map_view.h.
 *
 * Phase A: empty radar dish skeleton.
 * Phase B (this commit): peer rendering, layer mask, cursor, scale zoom
 *   reflected on peers.
 *   - cascade phoneapi_cache walked every refresh (gated on change_seq)
 *   - flat-earth projection (DEC-2 in ~/.claude/plans/map-ppi-radar-v1.md):
 *       Δlat / Δlon in radians from e7-fixed-point integers,
 *       range_m  ≈ R · sqrt(Δlat² + (cos(lat₀)·Δlon)²)
 *       az_rad   = atan2(cos(lat₀)·Δlon, Δlat)   (0=N, +clockwise)
 *   - peer label = node_alias_format_display() (short_name or alias)
 *   - SNR colours peer label (>=+5 green, >=0 white, >=-10 yellow,
 *     <-10 / unset grey) — analogue of gnss_sky's color_for_snr()
 *   - layer_mask: NODES (default) / ALL / ME_ONLY (sub-mode, SET cycles)
 *   - LEFT / RIGHT zoom: scale steps {100m..100km}; r_norm = range/scale
 *   - UP / DOWN: cursor walks peers with valid last_position; cursor
 *     peer label paints accent-orange + " * " marker prefix
 *   - OK: hand off to D-6 navigation — Phase C wires that; for now
 *     just emits a TRACE line.
 *
 * Phase C will add: VIEW_ID_MAP_NAV + map_view_set_nav_target() so OK
 * truly hands off.
 *
 * Geometry (panel 320 × 224)
 *   header     y   0..15
 *   disc       centre (160, 108), radius 88
 *   rings      r = 29 / 59 / 88  (≈ 1/3, 2/3, 3/3 of DISC_R)
 *   status     y 198..223
 *
 * Peer label cell: PEER_LABEL_W × PEER_LABEL_H (centre-aligned at the
 * projected pixel). Width 32 holds 4 ASCII chars or 2 CJK glyphs. If a
 * peer's projected r > DISC_R we hide it instead of clipping at the
 * edge (DEC-3 — peers beyond the current scale are simply not shown).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "map_view.h"

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

/* ── Layout ─────────────────────────────────────────────────────────── */

#define HEADER_H        16
#define DISC_R          88
#define DISC_CX        160
#define DISC_CY        (HEADER_H + 4 + DISC_R)   /* 108 */
#define PANEL_W        320
#define PANEL_H        224
#define STATUS_LINE_Y  (DISC_CY + DISC_R + 2)    /* 198 */
#define STATUS_LINE_H  (PANEL_H - STATUS_LINE_Y) /* 26 */

#define PEER_LABEL_W    32
#define PEER_LABEL_H    14

/* Outer-ring scale steps in metres. Index = s.scale_idx. */
static const int s_scale_m[] = {
    100, 500, 1000, 5000, 10000, 50000, 100000
};
#define SCALE_COUNT   (sizeof(s_scale_m) / sizeof(s_scale_m[0]))
#define SCALE_DEFAULT 2  /* 1 km */

/* Layer mask (D-2 sub-mode). v1:航跡/航點 placeholders only — see
 * plan §不在範圍. */
typedef enum {
    LAYER_NODES   = 0,    /* show all peers with last_position           */
    LAYER_ALL     = 1,    /* peers + waypoints + tracks (waypoints/tracks
                           * not implemented in v1, behaves like NODES)  */
    LAYER_ME_ONLY = 2,    /* hide all peers — sanity-check the dish      */
    LAYER_COUNT
} layer_mask_t;

static const char *s_layer_names[LAYER_COUNT] = {
    "nodes", "all", "me-only"
};

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *header;
    lv_obj_t *ring_inner;
    lv_obj_t *ring_mid;
    lv_obj_t *ring_outer;
    lv_obj_t *cardinal_n;
    lv_obj_t *me_cross;
    lv_obj_t *status_line;
    lv_obj_t *peer[PHONEAPI_NODES_CAP];

    uint8_t   scale_idx;       /* 0..SCALE_COUNT-1                       */
    uint8_t   layer_mask;      /* layer_mask_t                           */
    int8_t    cursor;          /* index into visible peers, -1 = none    */

    uint32_t  visible_count;   /* peers placed on the dish this frame    */
    uint32_t  visible_node_id[PHONEAPI_NODES_CAP];

    uint32_t  last_cache_seq;
    uint32_t  last_render_ms;
} map_t;

static map_t s;

/* Cold-boot vs LRU-recreate disambiguation (see Phase A note). */
static bool s_first_create_done = false;

/* Selected peer for D-6 hand-off (Phase C will read this). */
static uint32_t s_nav_target_node_num;

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

static lv_obj_t *make_ring(lv_obj_t *parent, int cx, int cy, int r)
{
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

static void format_scale(int metres, char *buf, size_t cap)
{
    if (metres < 1000) snprintf(buf, cap, "%dm", metres);
    else               snprintf(buf, cap, "%dkm", metres / 1000);
}

/* SNR colour scale for peers. snr_x100 = dB×100, INT32_MIN = unset. */
static lv_color_t color_for_peer_snr(int32_t snr_x100)
{
    if (snr_x100 == INT32_MIN)  return ui_color(UI_COLOR_TEXT_SECONDARY);
    if (snr_x100 >=  500)       return ui_color(UI_COLOR_ACCENT_SUCCESS);
    if (snr_x100 >=    0)       return ui_color(UI_COLOR_TEXT_PRIMARY);
    if (snr_x100 >= -1000)      return ui_color(UI_COLOR_WARN_YELLOW);
    return ui_color(UI_COLOR_TEXT_SECONDARY);
}

/* Project peer (lat_e7, lon_e7) onto the dish given local fix
 * (my_lat_e7, my_lon_e7) and current scale_m. Output:
 *   *out_x, *out_y:  pixel position (centre of the label cell)
 *   *out_range_m:    great-circle range used for ETA / cursor info
 *   *out_az_deg:     azimuth (0=N, +CW)
 * Returns true iff the peer fits inside the disc (range <= scale).
 *
 * Flat-earth approximation (DEC-2): valid for <50 km. Avoids haversine
 * and doesn't need any spherical-trig identities — single cosf call. */
static bool project_peer(int32_t my_lat_e7, int32_t my_lon_e7,
                         int32_t  pe_lat_e7, int32_t pe_lon_e7,
                         int       scale_m,
                         int      *out_x, int *out_y,
                         int      *out_range_m, int *out_az_deg)
{
    /* Convert e7 fixed-point integers to radians. */
    const float DEG_PER_E7 = 1e-7f;
    const float DEG_TO_RAD = (float)M_PI / 180.0f;
    const float R_EARTH    = 6371000.0f;     /* metres */

    float lat0_rad = (float)my_lat_e7 * DEG_PER_E7 * DEG_TO_RAD;
    float dlat_rad = (float)(pe_lat_e7 - my_lat_e7) * DEG_PER_E7 * DEG_TO_RAD;
    float dlon_rad = (float)(pe_lon_e7 - my_lon_e7) * DEG_PER_E7 * DEG_TO_RAD;

    float coslat   = cosf(lat0_rad);
    float dx       = coslat * dlon_rad;       /* east  */
    float dy       = dlat_rad;                /* north */
    float range_m  = R_EARTH * sqrtf(dx * dx + dy * dy);
    /* atan2(east, north) gives 0=N and increases clockwise — exactly
     * compass-bearing convention. Full 0..2π then mod to 0..360.   */
    float az_rad   = atan2f(dx, dy);
    if (az_rad < 0.0f) az_rad += 2.0f * (float)M_PI;
    int   az_deg   = (int)(az_rad * 180.0f / (float)M_PI);
    if (az_deg >= 360) az_deg -= 360;

    if (out_range_m) *out_range_m = (int)range_m;
    if (out_az_deg)  *out_az_deg  = az_deg;

    if (range_m > (float)scale_m) {
        /* Beyond outer ring — caller should hide. */
        return false;
    }
    float r_norm = range_m / (float)scale_m;
    float r_pix  = r_norm * (float)DISC_R;
    int   px = DISC_CX + (int)(r_pix * sinf(az_rad));
    int   py = DISC_CY - (int)(r_pix * cosf(az_rad));
    if (out_x) *out_x = px;
    if (out_y) *out_y = py;
    return true;
}

/* ── Render: peers ──────────────────────────────────────────────────── */

static void hide_all_peers(void)
{
    for (uint32_t i = 0; i < PHONEAPI_NODES_CAP; ++i) {
        if (s.peer[i]) lv_obj_add_flag(s.peer[i], LV_OBJ_FLAG_HIDDEN);
    }
    s.visible_count = 0;
}

static void render_peers(const teseo_state_t *t,
                         const phoneapi_my_info_t *mi,
                         char *status_buf, size_t status_cap,
                         uint32_t total)
{
    hide_all_peers();
    if (s.layer_mask == LAYER_ME_ONLY) {
        snprintf(status_buf, status_cap, "me-only  (peers hidden)");
        return;
    }
    if (t == NULL || !t->fix_valid) {
        snprintf(status_buf, status_cap, "GPS searching — peers hidden");
        return;
    }

    int scale_m = s_scale_m[s.scale_idx];
    int placed = 0;
    int beyond = 0;
    int no_pos = 0;

    for (uint32_t i = 0;
         i < total && (uint32_t)placed < PHONEAPI_NODES_CAP;
         ++i) {
        phoneapi_node_t e;
        if (!phoneapi_cache_take_node_at(i, &e)) continue;
        if (mi && e.num == mi->my_node_num) continue;     /* skip self */
        if (e.last_position.epoch == 0u) { no_pos++; continue; }

        int px, py, rng, az;
        bool inside = project_peer(t->lat_e7, t->lon_e7,
                                   e.last_position.lat_e7,
                                   e.last_position.lon_e7,
                                   scale_m, &px, &py, &rng, &az);
        if (!inside) { beyond++; continue; }

        char nm[16];
        node_alias_format_display(e.num, e.short_name, nm, sizeof(nm));

        /* Cursor highlight: prefix "*" + accent colour. Cursor is the
         * index INTO THE PLACED LIST, so we tag it after the placement
         * decision. */
        bool is_cursor = (s.cursor >= 0 && (int)placed == (int)s.cursor);
        char buf[20];
        if (is_cursor) snprintf(buf, sizeof(buf), "*%s", nm);
        else           snprintf(buf, sizeof(buf), "%s",  nm);

        lv_obj_t *p = s.peer[placed];
        lv_obj_set_pos(p, px - PEER_LABEL_W / 2, py - PEER_LABEL_H / 2);
        lv_label_set_text(p, buf);
        lv_obj_set_style_text_color(p,
            is_cursor ? ui_color(UI_COLOR_ACCENT_FOCUS)
                      : color_for_peer_snr(e.snr_x100), 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);

        s.visible_node_id[placed] = e.num;
        placed++;
    }

    s.visible_count = (uint32_t)placed;
    /* Clamp cursor to current visible list. */
    if (placed == 0)              s.cursor = -1;
    else if (s.cursor >= placed)  s.cursor = (int8_t)(placed - 1);
    else if (s.cursor < 0)        s.cursor = 0;

    snprintf(status_buf, status_cap,
             "peers=%d  beyond=%d  no_pos=%d  cursor=%d",
             placed, beyond, no_pos, (int)s.cursor);
}

static void render_header(uint32_t peer_count)
{
    if (!s.header) return;
    char scale_s[16];
    format_scale(s_scale_m[s.scale_idx], scale_s, sizeof(scale_s));
    char buf[64];
    snprintf(buf, sizeof(buf), "D-1 %s scale=%s peers=%lu",
             s_layer_names[s.layer_mask],
             scale_s,
             (unsigned long)peer_count);
    lv_label_set_text(s.header, buf);
}

static void render(void)
{
    const teseo_state_t      *t  = teseo_get_state();
    phoneapi_my_info_t        mi = (phoneapi_my_info_t){0};
    bool                      mi_ok = phoneapi_cache_get_my_info(&mi);
    uint32_t                  total = phoneapi_cache_node_count();
    char                      status[80];

    render_peers(t, mi_ok ? &mi : NULL, status, sizeof(status), total);
    render_header(s.visible_count);
    if (s.status_line) lv_label_set_text(s.status_line, status);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    /* Preserve scale + layer_mask + cursor across LRU re-create.
     * BSS-zero of `s.scale_idx` on cold boot is indistinguishable from
     * "user clamped to 100 m" — use s_first_create_done as the
     * disambiguator. */
    uint8_t  saved_scale  = s_first_create_done ? s.scale_idx  : SCALE_DEFAULT;
    uint8_t  saved_layer  = s_first_create_done ? s.layer_mask : LAYER_NODES;
    int8_t   saved_cursor = s_first_create_done ? s.cursor     : -1;
    s_first_create_done = true;
    memset(&s, 0, sizeof(s));
    s.scale_idx  = (saved_scale  < SCALE_COUNT) ? saved_scale  : SCALE_DEFAULT;
    s.layer_mask = (saved_layer  < LAYER_COUNT) ? saved_layer  : LAYER_NODES;
    s.cursor     = saved_cursor;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    /* Three rings: outer = disc edge, mid = 2/3, inner = 1/3. */
    s.ring_outer = make_ring(panel, DISC_CX, DISC_CY, DISC_R);
    s.ring_mid   = make_ring(panel, DISC_CX, DISC_CY, (DISC_R * 2) / 3);
    s.ring_inner = make_ring(panel, DISC_CX, DISC_CY, DISC_R / 3);

    /* North marker just above the outer ring. */
    s.cardinal_n = make_label(panel, DISC_CX - 4, DISC_CY - DISC_R - 16,
                              16, 16,
                              ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.cardinal_n, "N");

    /* ME crosshair at disc centre — single "+" in accent colour. */
    s.me_cross = make_label(panel, DISC_CX - 4, DISC_CY - 8, 16, 16,
                            ui_color(UI_COLOR_ACCENT_FOCUS));
    lv_label_set_text(s.me_cross, "+");

    /* Pre-allocated peer label cells; hidden by default, repositioned
     * on render. PHONEAPI_NODES_CAP = 32 matches the cache capacity so
     * we never need to evict. */
    for (uint32_t i = 0; i < PHONEAPI_NODES_CAP; ++i) {
        s.peer[i] = make_label(panel, 0, 0, PEER_LABEL_W, PEER_LABEL_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
        lv_obj_add_flag(s.peer[i], LV_OBJ_FLAG_HIDDEN);
    }

    s.status_line = make_label(panel, 4, STATUS_LINE_Y,
                               PANEL_W - 8, STATUS_LINE_H,
                               ui_color(UI_COLOR_TEXT_SECONDARY));

    TRACE("map", "create", "scale=%u layer=%u cursor=%d",
          (unsigned)s.scale_idx, (unsigned)s.layer_mask, (int)s.cursor);
    render();
}

static void destroy(void)
{
    TRACE_BARE("map", "destroy");
    s.header     = NULL;
    s.ring_inner = s.ring_mid = s.ring_outer = NULL;
    s.cardinal_n = NULL;
    s.me_cross   = NULL;
    s.status_line = NULL;
    for (uint32_t i = 0; i < PHONEAPI_NODES_CAP; ++i) s.peer[i] = NULL;
    /* scale_idx preserved across destroy → LRU re-create restores
     * the user's last zoom level. layer_mask / cursor are reset by
     * the BACK key handler in apply() (Option B sub-mode semantics),
     * not here, so a FUNC-out / FUNC-in round-trip keeps the user's
     * D-2 layer choice intact. */
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    switch (ev->keycode) {
        case MOKYA_KEY_LEFT:
            if (s.scale_idx > 0) {
                s.scale_idx--;
                render();
            }
            TRACE("map", "key", "kc=LEFT  scale=%u", (unsigned)s.scale_idx);
            break;
        case MOKYA_KEY_RIGHT:
            if (s.scale_idx + 1u < SCALE_COUNT) {
                s.scale_idx++;
                render();
            }
            TRACE("map", "key", "kc=RIGHT scale=%u", (unsigned)s.scale_idx);
            break;
        case MOKYA_KEY_SET:
            s.layer_mask = (uint8_t)((s.layer_mask + 1u) % LAYER_COUNT);
            render();
            TRACE("map", "key", "kc=SET   layer=%u", (unsigned)s.layer_mask);
            break;
        case MOKYA_KEY_UP:
            if (s.visible_count > 0u) {
                if (s.cursor <= 0) s.cursor = (int8_t)(s.visible_count - 1u);
                else               s.cursor--;
                render();
            }
            TRACE("map", "key", "kc=UP    cursor=%d", (int)s.cursor);
            break;
        case MOKYA_KEY_DOWN:
            if (s.visible_count > 0u) {
                if ((uint32_t)(s.cursor + 1) >= s.visible_count) s.cursor = 0;
                else                                              s.cursor++;
                render();
            }
            TRACE("map", "key", "kc=DOWN  cursor=%d", (int)s.cursor);
            break;
        case MOKYA_KEY_OK:
            /* Lock the cursor peer as the D-6 nav target. cursor < 0
             * (no visible peer) is a no-op so the user gets feedback
             * via TRACE only — D-6 with target=0 is reachable from
             * nodes_view OP_NAVIGATE for the "no D-1 peer" case. */
            if (s.cursor >= 0 && (uint32_t)s.cursor < s.visible_count) {
                s_nav_target_node_num = s.visible_node_id[s.cursor];
                TRACE("map", "key", "kc=OK    nav_target=0x%08lx",
                      (unsigned long)s_nav_target_node_num);
                view_router_navigate(VIEW_ID_MAP_NAV);
            } else {
                TRACE_BARE("map", "ok_no_cursor");
            }
            break;
        case MOKYA_KEY_BACK:
            /* BACK ends D-2 sub-modes: layer_mask + cursor reset so the
             * next D-1 entry comes up in the canonical "all peers,
             * no peer focused" state. scale_idx is the user's visual
             * zoom preference and stays sticky across BACK. */
            s.layer_mask = LAYER_NODES;
            s.cursor     = -1;
            TRACE_BARE("map", "back");
            view_router_navigate(VIEW_ID_BOOT_HOME);
            break;
        case MOKYA_KEY_TAB:
            /* TAB → D-3 waypoint list. Conceptually "next view" within
             * the Map app. Hint bar advertises this as "TAB 航點". */
            TRACE_BARE("map", "tab_to_d3");
            view_router_navigate(VIEW_ID_WAYPOINTS);
            break;
        default:
            break;
    }
}

static void refresh(void)
{
    if (s.header == NULL) return;
    /* Repaint when the cache has new data, otherwise once a second so
     * the GPS-fix toggle and beyond/no_pos counters stay current.
     * Mirrors telemetry_view's gating. */
    uint32_t cur = phoneapi_cache_change_seq();
    uint32_t t   = now_ms();
    if (cur == s.last_cache_seq && (t - s.last_render_ms) < 1000u) return;
    s.last_cache_seq  = cur;
    s.last_render_ms  = t;
    render();
}

static const view_descriptor_t MAP_DESC = {
    .id      = VIEW_ID_MAP,
    .name    = "map",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "<>縮放 TAB航點", "OK 鎖定", "BACK 返回" },
};

const view_descriptor_t *map_view_descriptor(void)
{
    return &MAP_DESC;
}

/* Phase C wiring point: nav target selected by the user's last OK on a
 * peer cursor. NULL-equivalent value is 0 (which is also reserved as
 * "broadcast" in Meshtastic, so callers should treat 0 as "no target"). */
uint32_t map_view_get_nav_target(void)
{
    return s_nav_target_node_num;
}
