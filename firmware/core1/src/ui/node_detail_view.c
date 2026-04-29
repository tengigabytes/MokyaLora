/* node_detail_view.c — see node_detail_view.h.
 *
 * C-2 detail layout (panel 320 × 224):
 *   y   0..15  header   "<short_name> / !<id>"
 *   y  16..207 body     full info block (name / hw / role / SNR /
 *                        hops / batt / volt / util / uptime /
 *                        licensed / pubkey hex)
 *   y 208..223 footer hint (managed by hint_bar)
 *
 * BACK returns to C-1 (VIEW_ID_NODES). OK enters C-3 ops menu.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "node_detail_view.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "phoneapi_cache.h"
#include "global/ui_theme.h"
#include "global/hint_bar.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "nodes_view.h"
#include "node_alias.h"

/* ── State ──────────────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *header;
    lv_obj_t *body;
    uint32_t  active_num;
    uint32_t  last_change_seq;
} node_detail_t;

/* PSRAM-resident — single-core access, low frequency, no SWD probe
 * target. Saves 16 B of the tight Core 1 SRAM budget. */
static node_detail_t s __attribute__((section(".psram_bss")));

/* ── Field helpers ──────────────────────────────────────────────────── */

/* Map Meshtastic Config.DeviceConfig.Role enum (subset) to a short
 * name. Falls through to "(role N)" for unknown values. */
static const char *role_label(uint8_t role)
{
    switch (role) {
        case 0:  return "Client";
        case 1:  return "Client_Mute";
        case 2:  return "Router";
        case 3:  return "Router_Client";
        case 4:  return "Repeater";
        case 5:  return "Tracker";
        case 6:  return "Sensor";
        case 7:  return "TAK";
        case 8:  return "Client_Hidden";
        case 9:  return "Lost_Found";
        case 10: return "TAK_Tracker";
        default: return "(role?)";
    }
}

/* Compact age formatter: seconds → "12s" / "5m" / "2h" / "3d". */
static void format_age(char *buf, size_t cap, uint32_t epoch_s)
{
    if (epoch_s == 0u) { snprintf(buf, cap, "--"); return; }
    /* phoneapi_cache last_heard is a wall-time epoch from Core 0. We
     * don't have a stable local "now" mapping here, so just show the
     * raw value the same way nodes_view used to. The user can compare
     * across rows. */
    snprintf(buf, cap, "%lu", (unsigned long)epoch_s);
}

static void format_pubkey(char *buf, size_t cap,
                          const uint8_t *bytes, uint8_t len)
{
    if (len == 0u) { snprintf(buf, cap, "(none)"); return; }
    /* Show 8 leading hex bytes + ".." for readability. Full key is
     * 32 B Curve25519, too wide for the panel. */
    size_t off = 0;
    uint8_t show = (len < 8u) ? len : 8u;
    for (uint8_t i = 0; i < show && off + 3u < cap; ++i) {
        off += (size_t)snprintf(buf + off, cap - off, "%02X", bytes[i]);
    }
    if (len > show && off + 3u < cap) {
        snprintf(buf + off, cap - off, "..");
    }
}

/* ── Render ─────────────────────────────────────────────────────────── */

static void render(void)
{
    phoneapi_node_t e;
    bool found = false;

    /* Linear lookup by node num — phoneapi_cache exposes _by_id but we
     * also want the offset-style ordering preserved by the cache. */
    if (s.active_num != 0u &&
        phoneapi_cache_get_node_by_id(s.active_num, &e)) {
        found = true;
    }

    if (!found) {
        lv_label_set_text(s.header, "(no node selected)");
        lv_label_set_text(s.body, "Pick a node from the list (BACK).");
        return;
    }

    char hdr[80];
    char nm[24];
    node_alias_format_display(e.num, e.short_name, nm, sizeof(nm));
    snprintf(hdr, sizeof(hdr), "%s / !%08lx",
             nm, (unsigned long)e.num);
    lv_label_set_text(s.header, hdr);

    char snr[16];
    if (e.snr_x100 == INT32_MIN) snprintf(snr, sizeof(snr), "--");
    else snprintf(snr, sizeof(snr), "%+.1f dB", e.snr_x100 / 100.0);

    char hops[8];
    if (e.hops_away == 0xFFu) snprintf(hops, sizeof(hops), "?");
    else snprintf(hops, sizeof(hops), "%u", (unsigned)e.hops_away);

    char batt[16];
    if (e.battery_level == 0xFFu) snprintf(batt, sizeof(batt), "--");
    else snprintf(batt, sizeof(batt), "%u%%", (unsigned)e.battery_level);

    char volt[16];
    if (e.voltage_mv == INT16_MIN || e.voltage_mv == 0)
        snprintf(volt, sizeof(volt), "--");
    else snprintf(volt, sizeof(volt), "%d.%02dV",
                  e.voltage_mv / 1000, (e.voltage_mv % 1000) / 10);

    char util[24];
    snprintf(util, sizeof(util), "ch %u%% / tx %u%%",
             (unsigned)e.channel_util_pct,
             (unsigned)e.air_util_tx_pct);

    char uptime[16];
    if (e.uptime_seconds == 0u) snprintf(uptime, sizeof(uptime), "--");
    else snprintf(uptime, sizeof(uptime), "%lus",
                  (unsigned long)e.uptime_seconds);

    char age[16];
    format_age(age, sizeof(age), e.last_heard);

    char pubkey[24];
    format_pubkey(pubkey, sizeof(pubkey), e.public_key, e.public_key_len);

    char body[480];
    snprintf(body, sizeof(body),
             "Long  : %s\n"
             "HW    : %u   Role : %s\n"
             "Channel : %u   Hops : %s\n"
             "SNR   : %s\n"
             "Batt  : %s    %s\n"
             "Util  : %s\n"
             "Uptime: %s    Heard: %s\n"
             "Lic   : %s\n"
             "PubKey: %s",
             e.long_name[0] ? e.long_name : "(no name)",
             (unsigned)e.hw_model,
             role_label(e.role),
             (unsigned)e.channel,
             hops,
             snr,
             batt, volt,
             util,
             uptime, age,
             e.is_licensed ? "yes" : "no",
             pubkey);
    lv_label_set_text(s.body, body);
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));
    s.active_num = nodes_view_get_active_node();

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

    render();
    hint_bar_set("", "OK ops", "BACK list");
}

static void destroy(void)
{
    s.header = s.body = NULL;
    hint_bar_clear();
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_OK:
            view_router_navigate(VIEW_ID_NODE_OPS);
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_NODES);
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

static const view_descriptor_t NODE_DETAIL_DESC = {
    .id      = VIEW_ID_NODE_DETAIL,
    .name    = "node_detail",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *node_detail_view_descriptor(void)
{
    return &NODE_DETAIL_DESC;
}
