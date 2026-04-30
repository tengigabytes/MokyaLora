/* waypoint_detail_view.c — see waypoint_detail_view.h.
 *
 * Reads the active waypoint id stashed via _set_target() (or via
 * waypoints_view_get_active_id() as fallback when entered straight
 * from D-3 without an explicit set).
 *
 * Locked-to resolution: if locked_to != 0, look up the owner node
 * in phoneapi_cache to render the short_name; otherwise show "open".
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "waypoint_detail_view.h"

#include <stdio.h>
#include <string.h>

#include "phoneapi_cache.h"
#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"
#include "node_alias.h"
#include "waypoints_view.h"

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *header;
    lv_obj_t *body;
    uint32_t  target_id;
    uint32_t  last_change_seq;
} wp_detail_t;

static wp_detail_t s __attribute__((section(".psram_bss")));
static uint32_t    s_pending_target __attribute__((section(".psram_bss")));

void waypoint_detail_view_set_target(uint32_t id) { s_pending_target = id; }
uint32_t waypoint_detail_view_get_target(void)    { return s.target_id; }

/* ── Helpers ────────────────────────────────────────────────────────── */

static void format_locked(char *buf, size_t cap, uint32_t locked_to)
{
    if (locked_to == 0u) { snprintf(buf, cap, "open"); return; }
    phoneapi_node_t n;
    if (phoneapi_cache_get_node_by_id(locked_to, &n) &&
        n.short_name[0] != '\0') {
        snprintf(buf, cap, "@%s (!%08lx)",
                 n.short_name, (unsigned long)locked_to);
    } else {
        snprintf(buf, cap, "!%08lx", (unsigned long)locked_to);
    }
}

static void format_source(char *buf, size_t cap,
                          const phoneapi_waypoint_t *e)
{
    if (e->is_local) { snprintf(buf, cap, "*me (local)"); return; }
    phoneapi_node_t n;
    if (phoneapi_cache_get_node_by_id(e->sender_node_id, &n) &&
        n.short_name[0] != '\0') {
        snprintf(buf, cap, "@%s (!%08lx)",
                 n.short_name, (unsigned long)e->sender_node_id);
    } else {
        snprintf(buf, cap, "@!%08lx", (unsigned long)e->sender_node_id);
    }
}

static void format_icon(char *buf, size_t cap, uint32_t icon)
{
    if (icon == 0u) { snprintf(buf, cap, "(none)"); return; }
    /* Render Unicode codepoint as both U+XXXX hex and decimal so the
     * user can confirm the broadcaster's choice without depending on
     * font glyph coverage (Unifont sm 16 lacks emoji). */
    snprintf(buf, cap, "U+%04lX (%lu)",
             (unsigned long)icon, (unsigned long)icon);
}

/* ── Render ─────────────────────────────────────────────────────────── */

static void render(void)
{
    phoneapi_waypoint_t e;
    bool found = false;

    if (s.target_id != 0u && phoneapi_waypoints_get_by_id(s.target_id, &e)) {
        found = true;
    }

    if (!found) {
        lv_label_set_text(s.header, "(no waypoint)");
        lv_label_set_text(s.body, "Pick a waypoint from the list (BACK).");
        return;
    }

    char hdr[80];
    snprintf(hdr, sizeof(hdr), "%s / id 0x%08lX",
             e.name[0] ? e.name : "(unnamed)",
             (unsigned long)e.id);
    lv_label_set_text(s.header, hdr);

    char locked[40];
    format_locked(locked, sizeof(locked), e.locked_to);

    char source[40];
    format_source(source, sizeof(source), &e);

    char icon[24];
    format_icon(icon, sizeof(icon), e.icon);

    char expire[24];
    if (e.expire == 0u) snprintf(expire, sizeof(expire), "never");
    else                snprintf(expire, sizeof(expire), "epoch %lu",
                                  (unsigned long)e.expire);

    char body[480];
    snprintf(body, sizeof(body),
             "Lat   : %.7f\n"
             "Lon   : %.7f\n"
             "Expire: %s\n"
             "Locked: %s\n"
             "Icon  : %s\n"
             "Source: %s\n"
             "Seen  : epoch %lu\n"
             "Desc  : %s",
             (double)e.lat_e7 * 1e-7,
             (double)e.lon_e7 * 1e-7,
             expire,
             locked,
             icon,
             source,
             (unsigned long)e.epoch_seen,
             e.description[0] ? e.description : "(none)");
    lv_label_set_text(s.body, body);
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));
    /* Pick the most-recently-set target. _set_target() takes precedence
     * over the D-3 cursor stash so D-2 layer hand-offs work too. */
    if (s_pending_target != 0u) {
        s.target_id = s_pending_target;
        s_pending_target = 0u;
    } else {
        s.target_id = waypoints_view_get_active_id();
    }

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = lv_label_create(panel);
    lv_obj_set_pos(s.header, 4, 0);
    lv_obj_set_size(s.header, 320 - 8, 16);
    lv_obj_set_style_text_font(s.header, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.header,
        ui_color(UI_COLOR_ACCENT_FOCUS), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "");

    s.body = lv_label_create(panel);
    lv_obj_set_pos(s.body, 4, 18);
    lv_obj_set_size(s.body, 320 - 8, 224 - 18 - 4);
    lv_obj_set_style_text_font(s.body, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(s.body,
        ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_line_space(s.body, 2, 0);
    lv_obj_set_style_pad_all(s.body, 0, 0);
    lv_label_set_long_mode(s.body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s.body, "");

    TRACE("wpd", "create", "id=0x%08lx", (unsigned long)s.target_id);
    render();
}

static void destroy(void)
{
    s.header = s.body = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_WAYPOINTS);
            break;
        default: break;
    }
}

static void refresh(void)
{
    if (s.body == NULL) return;
    uint32_t cur = phoneapi_cache_change_seq();
    if (cur == s.last_change_seq) return;
    s.last_change_seq = cur;
    render();
}

static const view_descriptor_t WP_DETAIL_DESC = {
    .id      = VIEW_ID_WAYPOINT_DETAIL,
    .name    = "wp_detail",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK list" },
};

const view_descriptor_t *waypoint_detail_view_descriptor(void)
{
    return &WP_DETAIL_DESC;
}
