/* waypoints_view.c — see waypoints_view.h.
 *
 * D-3 list: pulls waypoints from phoneapi_waypoints_take_at() (newest
 * first). 8-slot cap so list never scrolls — fits in 8 visible rows.
 *
 * Row format: "[focus] name  lat,lon  source"
 *   - cursor row prefixed with ">"
 *   - source = "@<sender_short>" for received, "*me" for self-created
 *   - lat/lon shown to 4 decimals (≈ 11 m precision; full e7 is in D-4)
 *
 * Persisted across reboots since Phase 4 (waypoint_persist on c1_storage
 * LittleFS). Header line shows count only (no v1 caveat any more).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "waypoints_view.h"

#include <stdio.h>
#include <string.h>

#include "phoneapi_cache.h"
#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"
#include "node_alias.h"

/* ── Layout ─────────────────────────────────────────────────────────── */

#define ROW_H         24
#define MAX_VISIBLE   PHONEAPI_WAYPOINTS_CAP
#define HEADER_H      16

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_VISIBLE];
    uint32_t  cursor;            /* 0..count-1 */
    uint32_t  last_change_seq;
} waypoints_t;

/* Push state into PSRAM .bss to keep tight Core 1 SRAM budget — same
 * pattern as rhw_pin_edit_view. LVGL pointers + cursor are read in
 * apply()/refresh() context only; PSRAM is write-back cached and
 * sequential reads are essentially free. */
static waypoints_t s __attribute__((section(".psram_bss")));

/* Cross-view stash for D-4 hand-off. */
static uint32_t s_active_id __attribute__((section(".psram_bss")));

void     waypoints_view_set_active_id(uint32_t id) { s_active_id = id; }
uint32_t waypoints_view_get_active_id(void)        { return s_active_id; }

/* ── Helpers ────────────────────────────────────────────────────────── */

static lv_obj_t *make_row(lv_obj_t *parent, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, 4, y);
    lv_obj_set_size(l, 320 - 8, ROW_H);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static void format_source(char *buf, size_t cap, const phoneapi_waypoint_t *e)
{
    if (e->is_local) {
        snprintf(buf, cap, "*me");
        return;
    }
    /* Look up sender short_name for received waypoints. */
    phoneapi_node_t n;
    if (phoneapi_cache_get_node_by_id(e->sender_node_id, &n) &&
        n.short_name[0] != '\0') {
        snprintf(buf, cap, "@%s", n.short_name);
    } else {
        snprintf(buf, cap, "@!%08lx",
                 (unsigned long)e->sender_node_id);
    }
}

static void format_row(char *buf, size_t cap, bool focused,
                       const phoneapi_waypoint_t *e)
{
    char src[12];
    format_source(src, sizeof(src), e);

    /* lat/lon to 4 decimals: ~11 m precision, fits the row. */
    double lat = (double)e->lat_e7 * 1e-7;
    double lon = (double)e->lon_e7 * 1e-7;

    snprintf(buf, cap, "%s%-12s %.4f,%.4f %s",
             focused ? ">" : " ",
             e->name[0] ? e->name : "(unnamed)",
             lat, lon, src);
}

static void clamp_cursor(uint32_t total)
{
    if (total == 0u) {
        s.cursor = 0u;
        return;
    }
    if (s.cursor >= total) s.cursor = total - 1u;
}

static void render(void)
{
    uint32_t total = phoneapi_waypoints_count();

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Waypts %lu/%u",
             (unsigned long)total, (unsigned)PHONEAPI_WAYPOINTS_CAP);
    lv_label_set_text(s.header, hdr);

    clamp_cursor(total);

    char buf[96];
    for (uint32_t i = 0; i < MAX_VISIBLE; ++i) {
        if (i >= total) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        phoneapi_waypoint_t e;
        if (!phoneapi_waypoints_take_at(i, &e)) {
            lv_label_set_text(s.rows[i], "");
            continue;
        }
        bool focused = (i == s.cursor);
        format_row(buf, sizeof(buf), focused, &e);
        lv_label_set_text(s.rows[i], buf);
        lv_obj_set_style_text_color(s.rows[i],
            focused ? ui_color(UI_COLOR_ACCENT_FOCUS)
                    : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, HEADER_H);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "Waypts");

    for (int i = 0; i < MAX_VISIBLE; ++i) {
        s.rows[i] = make_row(panel, HEADER_H + i * ROW_H);
    }

    TRACE("wpts", "create", "total=%u",
          (unsigned)phoneapi_waypoints_count());
    render();
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < MAX_VISIBLE; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    uint32_t total = phoneapi_waypoints_count();

    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (total > 0u && s.cursor > 0u) {
                s.cursor--;
                render();
            }
            break;
        case MOKYA_KEY_DOWN:
            if (total > 0u && s.cursor + 1u < total) {
                s.cursor++;
                render();
            }
            break;
        case MOKYA_KEY_OK: {
            if (total == 0u) break;
            phoneapi_waypoint_t e;
            if (phoneapi_waypoints_take_at(s.cursor, &e)) {
                waypoints_view_set_active_id(e.id);
                TRACE("wpts", "ok", "id=0x%08lx", (unsigned long)e.id);
                /* D-4 detail view lands in Phase 3 — navigation
                 * wired then. Phase 2 stops at id stash + TRACE. */
                if (g_view_registry[VIEW_ID_WAYPOINT_DETAIL] != NULL) {
                    view_router_navigate(VIEW_ID_WAYPOINT_DETAIL);
                }
            }
            break;
        }
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_MAP);
            break;
        case MOKYA_KEY_LEFT:
            /* LEFT → D-5 add waypoint (GNSS-only mode in v1). */
            TRACE_BARE("wpts", "left_to_d5");
            view_router_navigate(VIEW_ID_WAYPOINT_EDIT);
            break;
        default: break;
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

static const view_descriptor_t WAYPOINTS_DESC = {
    .id      = VIEW_ID_WAYPOINTS,
    .name    = "wpts",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "上下走訪 LEFT加", "OK 詳情", "BACK 地圖" },
};

const view_descriptor_t *waypoints_view_descriptor(void)
{
    return &WAYPOINTS_DESC;
}
