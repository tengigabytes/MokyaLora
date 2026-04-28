/* nodes_view.c — see nodes_view.h.
 *
 * M5F.2 (2026-04-28): switched data source from nodes_db (fed by the
 * retired IPC_MSG_NODE_UPDATE producer) to phoneapi_cache (cascade
 * decoder). Layout unchanged — the on-screen fields the cascade can
 * supply (long_name / short_name, SNR, hops, last_heard, battery) are
 * a strict subset of what nodes_db exposed; lat/lon are not currently
 * decoded by cascade so the GPS line is replaced with last-heard.
 */

#include "nodes_view.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "phoneapi_cache.h"
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
    phoneapi_node_t e;
    if (!phoneapi_cache_take_node_at(offset, &e)) {
        lv_label_set_text(s_header, "(no nodes seen yet)");
        lv_label_set_text(s_body, "");
        lv_label_set_text(s_footer, "node 0/0");
        return;
    }

    char hdr[80];
    if (e.short_name[0] != '\0' || e.long_name[0] != '\0') {
        snprintf(hdr, sizeof(hdr), "%s (%s)  /  0x%08lx",
                 e.long_name[0] ? e.long_name : "?",
                 e.short_name[0] ? e.short_name : "??",
                 (unsigned long)e.num);
    } else {
        snprintf(hdr, sizeof(hdr), "0x%08lx", (unsigned long)e.num);
    }
    lv_label_set_text(s_header, hdr);

    char snr_str[16];
    if (e.snr_x100 == INT32_MIN) {
        snprintf(snr_str, sizeof(snr_str), "--");
    } else {
        snprintf(snr_str, sizeof(snr_str), "%+.1f dB", e.snr_x100 / 100.0);
    }
    char hops_str[8];
    if (e.hops_away == 0xFFu) {
        snprintf(hops_str, sizeof(hops_str), "?");
    } else {
        snprintf(hops_str, sizeof(hops_str), "%u", (unsigned)e.hops_away);
    }
    char batt_str[16];
    if (e.battery_level == 0xFFu) {
        snprintf(batt_str, sizeof(batt_str), "--");
    } else {
        snprintf(batt_str, sizeof(batt_str), "%u%%",
                 (unsigned)e.battery_level);
    }
    char heard_str[24];
    if (e.last_heard == 0u) {
        snprintf(heard_str, sizeof(heard_str), "--");
    } else {
        snprintf(heard_str, sizeof(heard_str), "%lu",
                 (unsigned long)e.last_heard);
    }

    char body[256];
    snprintf(body, sizeof(body),
             "SNR  : %s\n"
             "hops : %s\n"
             "batt : %s\n"
             "heard: %s",
             snr_str, hops_str, batt_str, heard_str);
    lv_label_set_text(s_body, body);

    uint32_t total = phoneapi_cache_node_count();
    uint32_t shown = (offset < total) ? (total - offset) : 0u;
    char foot[32];
    snprintf(foot, sizeof(foot), "node %lu/%lu",
             (unsigned long)shown,
             (unsigned long)total);
    lv_label_set_text(s_footer, foot);
}

static void create(lv_obj_t *panel)
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

static void apply(const key_event_t *ev)
{
    if (!ev || !ev->pressed) return;

    uint32_t total = phoneapi_cache_node_count();
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

static void refresh(void)
{
    /* Active-only refresh: router no longer calls us when hidden. */
    if (s_panel == NULL) return;

    uint32_t cur = phoneapi_cache_change_seq();
    if (cur == s_last_change_seq) return;
    s_last_change_seq = cur;

    /* Sticky-to-newest re-renders at offset 0 on every fresh upsert.
     * If the user has navigated away with UP, leave them at their
     * offset until they DOWN-back to 0. */
    if (s_sticky_to_newest) s_offset = 0u;
    render_offset(s_offset);
}

static void destroy(void)
{
    s_panel = s_header = s_body = s_footer = NULL;
    s_last_change_seq = 0u;        /* force re-render on next activation */
    /* s_offset / s_sticky_to_newest persist across destroy. */
}

static const view_descriptor_t NODES_DESC = {
    .id      = VIEW_ID_NODES,
    .name    = "nodes",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *nodes_view_descriptor(void)
{
    return &NODES_DESC;
}
