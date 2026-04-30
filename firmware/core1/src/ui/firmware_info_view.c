/* firmware_info_view.c — see firmware_info_view.h.
 *
 * Single-page read-only summary.  No periodic refresh: cache values are
 * stable once the cascade has replayed Config (set on first cascade
 * commit), and the Core 1 git hash is compile-time constant.  We do
 * re-render when phoneapi cache change_seq bumps in case the host
 * pushes a metadata update mid-session.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "firmware_info_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"

#ifndef MOKYA_CORE1_GIT_HASH
#define MOKYA_CORE1_GIT_HASH "unknown"
#endif

#define ROW_H        20
#define HEADER_H     16
#define MAX_ROWS     10
#define PANEL_W     320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_ROWS];
    uint32_t  last_change_seq;
} fwinfo_t;

static fwinfo_t s;

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

static const char *role_name(uint8_t r)
{
    /* Mirrors meshtastic_Config_DeviceConfig_Role enum (subset of the
     * common cases).  Unknown values render as "?N" — the cache holds
     * a uint8 so wider values still print legibly. */
    switch (r) {
        case 0:  return "CLIENT";
        case 1:  return "CLIENT_MUTE";
        case 2:  return "ROUTER";
        case 4:  return "REPEATER";
        case 5:  return "TRACKER";
        case 6:  return "SENSOR";
        case 7:  return "TAK";
        case 8:  return "CLIENT_HIDDEN";
        case 9:  return "LOST_AND_FOUND";
        case 10: return "TAK_TRACKER";
        case 11: return "ROUTER_LATE";
        default: {
            static char buf[8];
            snprintf(buf, sizeof(buf), "?%u", (unsigned)r);
            return buf;
        }
    }
}

static void render(void)
{
    char buf[96];

    /* Row 0: Core 1 git hash (compile-time constant, always present). */
    snprintf(buf, sizeof(buf), "Core 1   git=%s", MOKYA_CORE1_GIT_HASH);
    lv_label_set_text(s.rows[0], buf);

    /* Row 1: Core 0 firmware version + pio_env (cached from cascade). */
    phoneapi_metadata_t md;
    phoneapi_my_info_t  mi;
    bool have_md = phoneapi_cache_get_metadata(&md);
    bool have_mi = phoneapi_cache_get_my_info(&mi);
    if (have_md && md.firmware_version[0]) {
        snprintf(buf, sizeof(buf), "Core 0   meshtastic %s",
                 md.firmware_version);
    } else {
        snprintf(buf, sizeof(buf), "Core 0   (waiting for cascade)");
    }
    lv_label_set_text(s.rows[1], buf);

    /* Row 2: PIO environment (rp2350b-mokya). */
    if (have_mi && mi.pio_env[0]) {
        snprintf(buf, sizeof(buf), "Variant  %s", mi.pio_env);
    } else {
        snprintf(buf, sizeof(buf), "Variant  ?");
    }
    lv_label_set_text(s.rows[2], buf);

    /* Row 3: own node id. */
    if (have_mi && mi.my_node_num != 0u) {
        snprintf(buf, sizeof(buf), "Node     !%08lx",
                 (unsigned long)mi.my_node_num);
    } else {
        snprintf(buf, sizeof(buf), "Node     ?");
    }
    lv_label_set_text(s.rows[3], buf);

    /* Row 4: device state version + role.  device_state_version bumps
     * on any deviceState mutation — useful for catching unintended
     * settings drift between flashes. */
    if (have_md) {
        snprintf(buf, sizeof(buf), "Role     %s  (devstate v%lu)",
                 role_name(md.role),
                 (unsigned long)md.device_state_version);
    } else {
        snprintf(buf, sizeof(buf), "Role     ?");
    }
    lv_label_set_text(s.rows[4], buf);

    /* Row 5: capability summary.  Core 0 reports has_wifi / bluetooth /
     * ethernet — all expected to be false on RP2350B Mokya, but a
     * regression in metadata decoder would flip them. */
    if (have_md) {
        snprintf(buf, sizeof(buf), "Caps     wifi=%d bt=%d eth=%d shut=%d",
                 (int)md.has_wifi, (int)md.has_bluetooth,
                 (int)md.has_ethernet, (int)md.can_shutdown);
    } else {
        snprintf(buf, sizeof(buf), "Caps     ?");
    }
    lv_label_set_text(s.rows[5], buf);

    /* Row 6: HW model from cached node entry (own node).  Captured
     * via cascade NodeInfo.user.hw_model.  RPI_PICO2 = 75 typically. */
    uint8_t hw = 0;
    if (have_mi && mi.my_node_num != 0u) {
        phoneapi_node_t e;
        if (phoneapi_cache_get_node_by_id(mi.my_node_num, &e)) {
            hw = e.hw_model;
        }
    }
    snprintf(buf, sizeof(buf), "HW       model=%u (RPI_PICO2 expected)",
             (unsigned)hw);
    lv_label_set_text(s.rows[6], buf);

    /* Row 7: cache freshness — node count + change seq give a quick
     * sanity check that the cascade is still alive. */
    snprintf(buf, sizeof(buf), "Cache    nodes=%lu  seq=%lu",
             (unsigned long)phoneapi_cache_node_count(),
             (unsigned long)phoneapi_cache_change_seq());
    lv_label_set_text(s.rows[7], buf);

    /* Row 8: blank divider */

    /* Row 9: hint about update path — Mokya doesn't yet support OTA;
     * spell that out so the user doesn't expect a prompt. */
    lv_label_set_text(s.rows[9], "更新   J-Link reflash (尚無 OTA)");
    lv_obj_set_style_text_color(s.rows[9],
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
}

static void create(lv_obj_t *panel)
{
    memset(&s, 0, sizeof(s));

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.header, "T-8 韌體資訊");

    for (int i = 0; i < MAX_ROWS; ++i) {
        s.rows[i] = make_label(panel, 4, HEADER_H + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    render();
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < MAX_ROWS; ++i) s.rows[i] = NULL;
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
    uint32_t cur = phoneapi_cache_change_seq();
    if (cur == s.last_change_seq) return;
    s.last_change_seq = cur;
    render();
}

static const view_descriptor_t FIRMWARE_INFO_DESC = {
    .id      = VIEW_ID_FIRMWARE_INFO,
    .name    = "fwinfo",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { NULL, NULL, "BACK 工具" },
};

const view_descriptor_t *firmware_info_view_descriptor(void)
{
    return &FIRMWARE_INFO_DESC;
}
