/* waypoint_edit_view.c — see waypoint_edit_view.h.
 *
 * D-5 v1: GNSS-only path.
 *   - Row 0 Name: OK opens IME (fullscreen, max 30 bytes — matches
 *     proto Waypoint.name max). Empty allowed; row falls back to
 *     "(unnamed)" placeholder.
 *   - Row 1 Save: OK snapshots teseo_state at press time and creates
 *     a waypoint with id = lower 32 bits of time_us_64() (same trick
 *     channel_add_view uses for entropy seeding). is_local=true,
 *     sender_node_id = my_node_num.  Disabled when fix_valid == false
 *     — toast "需 GNSS 3D fix" instead of writing junk coordinates.
 *
 * Manual lat/lon editor deferred to v2 (would need two number editors
 * + sign + 7-decimal handling — non-trivial, defers cleanly).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "waypoint_edit_view.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "teseo_liv3fl.h"
#include "ime_task.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define ROW_H           24
#define ROW_COUNT        2
#define STATUS_TOP      (HEADER_H + ROW_COUNT * ROW_H + 4)
#define PANEL_W        320

typedef enum {
    WPE_ROW_NAME = 0,
    WPE_ROW_SAVE = 1,
} wpe_row_t;

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *status;
    uint8_t   cursor;
    char      name[PHONEAPI_WAYPOINT_NAME_MAX];   /* 30 + NUL */
    uint8_t   name_len;
} wpe_t;

static wpe_t s __attribute__((section(".psram_bss")));

/* ── Helpers ────────────────────────────────────────────────────────── */

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

static void set_status(const char *msg)
{
    if (s.status) lv_label_set_text(s.status, msg);
}

/* ── Render ─────────────────────────────────────────────────────────── */

static void render(void)
{
    char buf[80];

    snprintf(buf, sizeof(buf), "D-5 加航點 (v1 GNSS only — manual: v2)");
    lv_label_set_text(s.header, buf);

    snprintf(buf, sizeof(buf), "%sName : %s",
             s.cursor == WPE_ROW_NAME ? ">" : " ",
             s.name_len > 0u ? s.name : "(tap OK to enter)");
    lv_label_set_text(s.rows[WPE_ROW_NAME], buf);

    const teseo_state_t *t = teseo_get_state();
    bool fix = (t != NULL) && t->fix_valid;
    snprintf(buf, sizeof(buf), "%sSave (use GNSS now)%s",
             s.cursor == WPE_ROW_SAVE ? ">" : " ",
             fix ? "" : "  [no fix]");
    lv_label_set_text(s.rows[WPE_ROW_SAVE], buf);

    /* Colours: focus on cursor, dim Save when no fix (still
     * navigable, just visually disabled). */
    lv_obj_set_style_text_color(s.rows[WPE_ROW_NAME],
        (s.cursor == WPE_ROW_NAME)
            ? ui_color(UI_COLOR_ACCENT_FOCUS)
            : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_color(s.rows[WPE_ROW_SAVE],
        (s.cursor == WPE_ROW_SAVE && fix)
            ? ui_color(UI_COLOR_ACCENT_FOCUS)
            : (fix ? ui_color(UI_COLOR_TEXT_PRIMARY)
                   : ui_color(UI_COLOR_TEXT_SECONDARY)), 0);

    /* Status block: live GNSS readout. */
    char status[160];
    if (!fix) {
        snprintf(status, sizeof(status),
                 "GNSS: searching...\n"
                 "(need 3D fix before Save)");
    } else {
        const char *q = "?";
        switch (t->fix_quality) {
            case 0: q = "none"; break;
            case 1: q = "GPS";  break;
            case 2: q = "DGPS"; break;
            default: q = "?";   break;
        }
        snprintf(status, sizeof(status),
                 "GNSS fix: %s  sats=%u\n"
                 "Lat:  %.7f\n"
                 "Lon:  %.7f\n"
                 "HDOP: %.1f",
                 q, (unsigned)t->num_sats,
                 (double)t->lat_e7 * 1e-7,
                 (double)t->lon_e7 * 1e-7,
                 (double)t->hdop_x10 * 0.1);
    }
    set_status(status);
}

/* ── Actions ────────────────────────────────────────────────────────── */

static void on_name_done(bool committed, const char *utf8,
                         uint16_t byte_len, void *ctx)
{
    (void)ctx;
    if (!committed) {
        TRACE_BARE("wpe", "name_cancel");
        render();
        return;
    }
    if (byte_len >= PHONEAPI_WAYPOINT_NAME_MAX) {
        byte_len = PHONEAPI_WAYPOINT_NAME_MAX - 1u;
    }
    memcpy(s.name, utf8, byte_len);
    s.name[byte_len] = '\0';
    s.name_len = (uint8_t)byte_len;
    TRACE("wpe", "name_set", "len=%u", (unsigned)byte_len);
    render();
}

static void open_name_edit(void)
{
    if (ime_request_text_active()) {
        set_status("IME busy — try again");
        return;
    }
    ime_text_request_t req = {
        .prompt    = "Waypoint name",
        .initial   = (s.name_len > 0u) ? s.name : NULL,
        .max_bytes = PHONEAPI_WAYPOINT_NAME_MAX - 1u,   /* 30 chars */
        .mode_hint = IME_TEXT_MODE_DEFAULT,
        .flags     = IME_TEXT_FLAG_ALLOW_EMPTY,
        .layout    = IME_TEXT_LAYOUT_FULLSCREEN,
        .draft_id  = 0u,
    };
    if (!ime_request_text(&req, on_name_done, NULL)) {
        set_status("IME request failed");
    }
}

static void fire_save(void)
{
    const teseo_state_t *t = teseo_get_state();
    if (t == NULL || !t->fix_valid) {
        set_status("需 GNSS 3D fix (move to open sky)");
        TRACE_BARE("wpe", "save_no_fix");
        return;
    }

    phoneapi_my_info_t mi;
    uint32_t my_num = 0u;
    if (phoneapi_cache_get_my_info(&mi)) my_num = mi.my_node_num;

    phoneapi_waypoint_t w;
    memset(&w, 0, sizeof(w));
    /* Lower 32 bits of the 1 MHz monotonic — wraps every ~71 min, but
     * collisions inside the 8-slot cache are detected by upsert anyway.
     * Avoid id == 0 (decoder reserves that as malformed). */
    uint32_t id = (uint32_t)(time_us_64() & 0xFFFFFFFFu);
    if (id == 0u) id = 1u;
    w.id              = id;
    w.lat_e7          = t->lat_e7;
    w.lon_e7          = t->lon_e7;
    w.expire          = 0u;             /* never */
    w.locked_to       = my_num;         /* only we can edit */
    w.icon            = 0u;
    if (s.name_len > 0u) {
        memcpy(w.name, s.name, s.name_len);
        w.name[s.name_len] = '\0';
    }
    w.description[0]  = '\0';
    w.sender_node_id  = my_num;
    w.epoch_seen      = (uint32_t)(time_us_64() / 1000000u);
    w.is_local        = true;

    phoneapi_waypoints_upsert(&w);

    char msg[80];
    snprintf(msg, sizeof(msg),
             "Saved id=0x%08lx (%.4f,%.4f)",
             (unsigned long)id,
             (double)t->lat_e7 * 1e-7,
             (double)t->lon_e7 * 1e-7);
    set_status(msg);
    TRACE("wpe", "saved",
          "id=0x%08lx,lat=%d,lon=%d",
          (unsigned long)id, (int)t->lat_e7, (int)t->lon_e7);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    /* Preserve typed name across LRU re-create — typing "Taipei101" then
     * leaving the view should not lose the partial work. */
    char     saved_name[PHONEAPI_WAYPOINT_NAME_MAX];
    uint8_t  saved_len = s.name_len;
    memcpy(saved_name, s.name, sizeof(saved_name));

    memset(&s, 0, sizeof(s));
    if (saved_len > 0u && saved_len < sizeof(s.name)) {
        memcpy(s.name, saved_name, saved_len);
        s.name[saved_len] = '\0';
        s.name_len = saved_len;
    }

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        s.rows[i] = make_label(panel, 4, HEADER_H + (int)i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.status = make_label(panel, 4, STATUS_TOP, PANEL_W - 8,
                          224 - STATUS_TOP - 16,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    TRACE_BARE("wpe", "create");
    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.status = NULL;
    for (uint8_t i = 0; i < ROW_COUNT; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cursor > 0u) { s.cursor--; render(); }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cursor + 1u < ROW_COUNT) { s.cursor++; render(); }
            break;
        case MOKYA_KEY_OK:
            switch (s.cursor) {
                case WPE_ROW_NAME: open_name_edit(); break;
                case WPE_ROW_SAVE: fire_save();      break;
                default: break;
            }
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_WAYPOINTS);
            break;
        default: break;
    }
}

static uint32_t s_last_render_ms __attribute__((section(".psram_bss")));

static void refresh(void)
{
    /* GNSS state changes outside our key flow — repaint the status
     * block once a second so the lat/lon block stays current. */
    if (s.status == NULL) return;
    uint32_t now = (uint32_t)(time_us_64() / 1000u);
    if (now - s_last_render_ms < 1000u) return;
    s_last_render_ms = now;
    render();
}

static const view_descriptor_t WP_EDIT_DESC = {
    .id      = VIEW_ID_WAYPOINT_EDIT,
    .name    = "wp_edit",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN", "OK 編輯/存", "BACK 返回" },
};

const view_descriptor_t *waypoint_edit_view_descriptor(void)
{
    return &WP_EDIT_DESC;
}
