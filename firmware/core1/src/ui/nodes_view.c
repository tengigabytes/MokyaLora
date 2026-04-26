/* nodes_view.c — see nodes_view.h. */

#include "nodes_view.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "nodes_db.h"
#include "mie_font.h"
#include "mie/keycode.h"
#include "mokya_trace.h"

static lv_obj_t *s_panel;
static lv_obj_t *s_header;
static lv_obj_t *s_body;
static lv_obj_t *s_footer;

static uint32_t s_offset;            /* 0 = most recently touched */
static uint32_t s_last_change_seq;
static bool     s_sticky_to_newest = true;

static void render_offset(uint32_t offset)
{
    nodes_db_entry_t e;
    if (!nodes_db_take_at(offset, &e)) {
        lv_label_set_text(s_header, "(no nodes seen yet)");
        lv_label_set_text(s_body, "");
        lv_label_set_text(s_footer, "node 0/0");
        return;
    }

    char alias_buf[NODES_DB_ALIAS_MAX + 1];
    uint8_t alias_len = e.alias_len;
    if (alias_len > NODES_DB_ALIAS_MAX) alias_len = NODES_DB_ALIAS_MAX;
    memcpy(alias_buf, e.alias, alias_len);
    alias_buf[alias_len] = '\0';

    char hdr[80];
    if (alias_len > 0) {
        snprintf(hdr, sizeof(hdr), "%s  /  0x%08lx",
                 alias_buf, (unsigned long)e.node_id);
    } else {
        snprintf(hdr, sizeof(hdr), "0x%08lx",
                 (unsigned long)e.node_id);
    }
    lv_label_set_text(s_header, hdr);

    /* Body — multiline. Use printf-ish layout; the MIEF 16 px font
     * fits ~20 chars/line at 320 px so this stays inside the panel. */
    char body[256];
    char snr_str[16];
    if (e.snr_x4 == (int8_t)INT8_MIN) {
        snprintf(snr_str, sizeof(snr_str), "--");
    } else {
        snprintf(snr_str, sizeof(snr_str), "%+.1f dB",
                 e.snr_x4 / 4.0);
    }
    char hops_str[8];
    if (e.hops_away == 0xFFu) {
        snprintf(hops_str, sizeof(hops_str), "?");
    } else {
        snprintf(hops_str, sizeof(hops_str), "%u", (unsigned)e.hops_away);
    }
    char batt_str[16];
    if (e.battery_mv == 0u) {
        snprintf(batt_str, sizeof(batt_str), "--");
    } else {
        snprintf(batt_str, sizeof(batt_str), "%u.%02u V",
                 (unsigned)(e.battery_mv / 1000u),
                 (unsigned)((e.battery_mv % 1000u) / 10u));
    }

    char pos_str[64];
    if (e.lat_e7 == INT32_MIN || e.lon_e7 == INT32_MIN) {
        snprintf(pos_str, sizeof(pos_str), "no GPS");
    } else {
        /* Render as decimal degrees with 5 d.p. (~1.1 m). */
        double lat = e.lat_e7 / 1.0e7;
        double lon = e.lon_e7 / 1.0e7;
        snprintf(pos_str, sizeof(pos_str), "%.5f, %.5f", lat, lon);
    }

    snprintf(body, sizeof(body),
             "SNR  : %s\n"
             "hops : %s\n"
             "batt : %s\n"
             "pos  : %s",
             snr_str, hops_str, batt_str, pos_str);
    lv_label_set_text(s_body, body);

    uint32_t total = nodes_db_count();
    uint32_t shown = (offset < total) ? (total - offset) : 0u;
    char foot[32];
    snprintf(foot, sizeof(foot), "node %lu/%lu",
             (unsigned long)shown,
             (unsigned long)total);
    lv_label_set_text(s_footer, foot);
}

void nodes_view_init(lv_obj_t *panel)
{
    const lv_font_t *f16 = mie_font_unifont_sm_16();
    s_panel = panel;

    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);

    s_header = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_header, f16, 0);
    lv_obj_set_style_text_color(s_header, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(s_header, "(no nodes seen yet)");
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_width(s_header, 312);

    s_body = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_body, f16, 0);
    lv_obj_set_style_text_color(s_body, lv_color_white(), 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_body, 4, 0);
    lv_label_set_text(s_body, "");
    lv_obj_set_pos(s_body, 0, 24);
    lv_obj_set_size(s_body, 312, 240 - 24 - 24);

    s_footer = lv_label_create(panel);
    if (f16) lv_obj_set_style_text_font(s_footer, f16, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(0x808080), 0);
    lv_label_set_text(s_footer, "node 0/0");
    lv_obj_set_pos(s_footer, 0, 240 - 24);
    lv_obj_set_width(s_footer, 312);
}

void nodes_view_apply(const key_event_t *ev)
{
    if (!ev || !ev->pressed) return;

    uint32_t total = nodes_db_count();
    if (total == 0) return;

    if (ev->keycode == MOKYA_KEY_UP) {
        if (s_offset + 1u < total) {
            s_offset++;
        }
        s_sticky_to_newest = false;
        render_offset(s_offset);
    } else if (ev->keycode == MOKYA_KEY_DOWN) {
        if (s_offset > 0u) {
            s_offset--;
        }
        if (s_offset == 0u) {
            s_sticky_to_newest = true;
        }
        render_offset(s_offset);
    }
}

void nodes_view_refresh(void)
{
    /* Skip the heavy snprintf-and-set_text path while the panel is
     * hidden — view_router_tick calls every view's refresh each LVGL
     * tick to "stay current", but with NodeDB observer firing at a few
     * Hz this stole noticeable lvgl_task budget from ime_view's render
     * path. Track change_seq so a single render at activation paints
     * the latest state. */
    uint32_t cur = nodes_db_change_seq();

    if (s_panel != NULL && lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        s_last_change_seq = cur;
        return;
    }

    if (cur == s_last_change_seq) return;
    s_last_change_seq = cur;

    /* Sticky-to-newest re-renders at offset 0 on every fresh upsert.
     * If the user has navigated away with UP, leave them at their
     * offset until they DOWN-back to 0. */
    if (s_sticky_to_newest) s_offset = 0u;
    render_offset(s_offset);
}
