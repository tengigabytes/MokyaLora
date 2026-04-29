/* my_node_view.c — see my_node_view.h.
 *
 * Read-only display, sourced from phoneapi_cache:
 *   - my_info  : my_node_num, reboot_count, firmware_edition, pio_env
 *   - metadata : role, can_shutdown, has_wifi/ble/ethernet, fw version
 *   - node[my] : long_name, short_name, hw_model (cascade upserts our
 *                own node when Core 0 broadcasts NodeInfo)
 *
 * Edits live in the Settings App (S-2 Owner / S-1 LoRa region etc).
 * BACK returns to L-0 home (this view is reachable from launcher's
 * "Admin" tile once that lands; until then, useful as a diagnostic).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "my_node_view.h"

#include <stdio.h>
#include <string.h>

#include "phoneapi_cache.h"
#include "global/ui_theme.h"
#include "global/hint_bar.h"
#include "key_event.h"
#include "mie/keycode.h"

typedef struct {
    lv_obj_t *header;
    lv_obj_t *body;
    uint32_t  last_change_seq;
} my_node_t;

/* PSRAM-resident (single-core access, low frequency) — saves ~12 B
 * of the tight Core 1 SRAM budget. */
static my_node_t s __attribute__((section(".psram_bss")));

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

static void render(void)
{
    phoneapi_my_info_t  mi  = {0};
    phoneapi_metadata_t md  = {0};
    phoneapi_node_t     me  = {0};

    bool have_mi = phoneapi_cache_get_my_info(&mi);
    bool have_md = phoneapi_cache_get_metadata(&md);
    bool have_me = have_mi && phoneapi_cache_get_node_by_id(mi.my_node_num, &me);

    if (!have_mi) {
        lv_label_set_text(s.header, "(my node — waiting for cascade)");
        lv_label_set_text(s.body, "");
        return;
    }

    char hdr[80];
    if (have_me && me.short_name[0]) {
        snprintf(hdr, sizeof(hdr), "%s !%08lx",
                 me.short_name, (unsigned long)mi.my_node_num);
    } else {
        snprintf(hdr, sizeof(hdr), "Me  !%08lx",
                 (unsigned long)mi.my_node_num);
    }
    lv_label_set_text(s.header, hdr);

    char body[480];
    snprintf(body, sizeof(body),
             "Long  : %s\n"
             "Role  : %s\n"
             "HW    : %u\n"
             "Reboot: %lu\n"
             "FW    : %s\n"
             "PIO   : %s\n"
             "NodeDB: %lu peers\n"
             "Caps  : wifi=%c ble=%c eth=%c shutdown=%c",
             have_me && me.long_name[0] ? me.long_name : "(no name)",
             role_label(have_md ? md.role : 0xFF),
             have_me ? (unsigned)me.hw_model : 0u,
             (unsigned long)mi.reboot_count,
             have_md && md.firmware_version[0]
                 ? md.firmware_version : "(unknown)",
             mi.pio_env[0] ? mi.pio_env : "(unknown)",
             (unsigned long)mi.nodedb_count,
             have_md && md.has_wifi      ? 'Y' : 'n',
             have_md && md.has_bluetooth ? 'Y' : 'n',
             have_md && md.has_ethernet  ? 'Y' : 'n',
             have_md && md.can_shutdown  ? 'Y' : 'n');
    lv_label_set_text(s.body, body);
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

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
    hint_bar_set("", "(edit in Settings)", "BACK home");
}

static void destroy(void)
{
    s.header = s.body = NULL;
    hint_bar_clear();
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    if (ev->keycode == MOKYA_KEY_BACK) {
        view_router_navigate(VIEW_ID_BOOT_HOME);
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

static const view_descriptor_t MY_NODE_DESC = {
    .id      = VIEW_ID_MY_NODE,
    .name    = "my_node",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
};

const view_descriptor_t *my_node_view_descriptor(void)
{
    return &MY_NODE_DESC;
}
