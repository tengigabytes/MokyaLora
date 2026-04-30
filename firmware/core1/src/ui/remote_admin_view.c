/* remote_admin_view.c — see remote_admin_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header   "C-3 Remote Admin -> <target>"
 *   y  16..143  5 rows × 24 px + spacer (16 + 5*24 = 136, +8 pad = 144)
 *   y 152..183  status line (last action / arm hint)
 *   y 184..223  warning footer
 *
 * Two-step arm-then-confirm: first OK on a row sets s.armed_row to that
 * row + bumps a tick deadline; second OK fires the action.  UP/DOWN
 * disarm.  BACK in armed state disarms; BACK in idle state navigates
 * back to NODE_OPS.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "remote_admin_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "mokya_trace.h"

#include "phoneapi_cache.h"
#include "phoneapi_encode.h"
#include "node_alias.h"
#include "nodes_view.h"

#define HEADER_H        16
#define ROW_H           24
#define ROW_COUNT        5
#define LIST_TOP        HEADER_H
#define STATUS_TOP     (LIST_TOP + ROW_COUNT * ROW_H + 8)   /* 152 */
#define WARN_TOP       (STATUS_TOP + ROW_H + 8)              /* 184 */
#define PANEL_W        320

/* Arm timeout: second OK must land within this window or we disarm
 * automatically.  3 s = enough for human reaction time, short enough
 * that an accidental arm-then-pocket doesn't leave a destructive
 * action loaded indefinitely. */
#define ARM_TIMEOUT_MS  3000u

typedef enum {
    ACT_REBOOT             = 0,
    ACT_SHUTDOWN           = 1,
    ACT_FACTORY_RESET_CFG  = 2,
    ACT_FACTORY_RESET_DEV  = 3,
    ACT_NODEDB_RESET       = 4,
} action_id_t;

typedef struct {
    action_id_t id;
    const char *label;
    const char *short_warning;
} action_def_t;

static const action_def_t k_actions[ROW_COUNT] = {
    { ACT_REBOOT,            "Reboot 5s",                "節點將在 5 秒後重啟" },
    { ACT_SHUTDOWN,          "Shutdown 5s",              "節點將在 5 秒後關機" },
    { ACT_FACTORY_RESET_CFG, "Factory reset config",     "重置設定 (BLE 保留)" },
    { ACT_FACTORY_RESET_DEV, "Factory reset device",     "重置全部 (含 BLE)" },
    { ACT_NODEDB_RESET,      "NodeDB reset",             "清節點表 (保留 favorites)" },
};

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *status;
    lv_obj_t *warn;
    uint32_t  target_node_num;
    uint8_t   cursor;
    int8_t    armed_row;        /* -1 = idle */
    uint32_t  armed_deadline_ms;
} remadm_t;

static remadm_t s;

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

static void render_header(void)
{
    char buf[80];
    char nm[16] = {0};
    if (s.target_node_num != 0u) {
        phoneapi_node_t e;
        const char *short_name = NULL;
        if (phoneapi_cache_get_node_by_id(s.target_node_num, &e)) {
            short_name = e.short_name;
        }
        node_alias_format_display(s.target_node_num, short_name,
                                  nm, sizeof(nm));
        snprintf(buf, sizeof(buf), "C-3 Remote Admin -> %s", nm);
    } else {
        snprintf(buf, sizeof(buf), "C-3 Remote Admin (no target)");
    }
    lv_label_set_text(s.header, buf);
}

static void render_rows(void)
{
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        char buf[64];
        bool focused = (i == s.cursor);
        bool armed   = (s.armed_row == (int8_t)i);
        const char *prefix;
        if (armed)        prefix = "*";   /* armed (will fire on next OK) */
        else if (focused) prefix = ">";
        else              prefix = " ";
        snprintf(buf, sizeof(buf), "%s %s",
                 prefix, k_actions[i].label);
        lv_label_set_text(s.rows[i], buf);
        if (armed) {
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_WARN_YELLOW), 0);
        } else if (focused) {
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_ACCENT_FOCUS), 0);
        } else {
            lv_obj_set_style_text_color(s.rows[i],
                ui_color(UI_COLOR_TEXT_PRIMARY), 0);
        }
    }
}

static void render_status(const char *txt)
{
    if (s.status) lv_label_set_text(s.status, txt ? txt : "");
}

static void render(void)
{
    render_header();
    render_rows();
    if (s.armed_row >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ARM: OK 確認 / BACK 取消 (%s)",
                 k_actions[s.armed_row].short_warning);
        render_status(buf);
    } else {
        render_status("UP/DN 選動作  OK 解除武裝  BACK 返回");
    }
}

/* Fire the armed action. */
static bool fire(action_id_t a)
{
    uint32_t pid = 0u;
    bool ok = false;
    switch (a) {
        case ACT_REBOOT:
            ok = phoneapi_encode_admin_reboot(s.target_node_num,
                                              /*channel=*/0u,
                                              /*seconds=*/5,
                                              &pid);
            break;
        case ACT_SHUTDOWN:
            ok = phoneapi_encode_admin_shutdown(s.target_node_num,
                                                /*channel=*/0u,
                                                /*seconds=*/5,
                                                &pid);
            break;
        case ACT_FACTORY_RESET_CFG:
            ok = phoneapi_encode_admin_factory_reset_config(s.target_node_num,
                                                            /*channel=*/0u,
                                                            &pid);
            break;
        case ACT_FACTORY_RESET_DEV:
            ok = phoneapi_encode_admin_factory_reset_device(s.target_node_num,
                                                            /*channel=*/0u,
                                                            &pid);
            break;
        case ACT_NODEDB_RESET:
            ok = phoneapi_encode_admin_nodedb_reset(s.target_node_num,
                                                    /*channel=*/0u,
                                                    &pid);
            break;
    }
    char buf[80];
    if (ok) {
        snprintf(buf, sizeof(buf), "%s sent (pid=%#lx)",
                 k_actions[a].label, (unsigned long)pid);
        TRACE("radmin", "fire",
              "act=%u target=%lu pid=%#lx",
              (unsigned)a, (unsigned long)s.target_node_num,
              (unsigned long)pid);
    } else {
        snprintf(buf, sizeof(buf), "%s push failed",
                 k_actions[a].label);
        TRACE("radmin", "fire_fail",
              "act=%u target=%lu",
              (unsigned)a, (unsigned long)s.target_node_num);
    }
    render_status(buf);
    return ok;
}

static void disarm(void)
{
    s.armed_row = -1;
    s.armed_deadline_ms = 0u;
    render();
}

static void create(lv_obj_t *panel)
{
    /* Capture target on entry; UX state (cursor) optionally preserved
     * across LRU re-creates so user comes back to same row. armed_row
     * is intentionally NOT preserved — re-entering must re-arm. */
    uint8_t saved_cursor = s.cursor;
    memset(&s, 0, sizeof(s));
    s.cursor          = (saved_cursor < ROW_COUNT) ? saved_cursor : 0u;
    s.armed_row       = -1;
    s.target_node_num = nodes_view_get_active_node();

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    for (int i = 0; i < ROW_COUNT; ++i) {
        s.rows[i] = make_label(panel, 4, LIST_TOP + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.status = make_label(panel, 4, STATUS_TOP, PANEL_W - 8, ROW_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    s.warn   = make_label(panel, 4, WARN_TOP, PANEL_W - 8, ROW_H,
                          ui_color(UI_COLOR_WARN_YELLOW));
    lv_label_set_text(s.warn,
        "需 admin channel 啟用或 target admin_key 含本機 pubkey");

    render();
}

static void destroy(void)
{
    s.header = s.status = s.warn = NULL;
    for (int i = 0; i < ROW_COUNT; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;

    /* Auto-disarm on timeout (caller's hand left the keypad). */
    if (s.armed_row >= 0 && now_ms() > s.armed_deadline_ms) {
        TRACE("radmin", "arm_timeout", "row=%d", (int)s.armed_row);
        disarm();
    }

    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            /* Any directional press disarms — even at a boundary where
             * the cursor doesn't move — because intent is "back away
             * from this action".  Clamp on cursor; disarm always. */
            if (s.armed_row >= 0) {
                disarm();
            } else if (s.cursor > 0u) {
                s.cursor--;
                render();
            }
            break;
        case MOKYA_KEY_DOWN:
            if (s.armed_row >= 0) {
                disarm();
            } else if (s.cursor + 1u < ROW_COUNT) {
                s.cursor++;
                render();
            }
            break;
        case MOKYA_KEY_OK:
            if (s.target_node_num == 0u) {
                render_status("No target node — go back to C-1 first");
                break;
            }
            if (s.armed_row == (int8_t)s.cursor) {
                /* Confirmed — fire. */
                action_id_t a = (action_id_t)s.cursor;
                disarm();
                fire(a);
            } else {
                /* First OK arms this row. */
                s.armed_row = (int8_t)s.cursor;
                s.armed_deadline_ms = now_ms() + ARM_TIMEOUT_MS;
                TRACE("radmin", "arm",
                      "row=%u target=%lu",
                      (unsigned)s.cursor,
                      (unsigned long)s.target_node_num);
                render();
            }
            break;
        case MOKYA_KEY_BACK:
            if (s.armed_row >= 0) {
                disarm();
            } else {
                view_router_navigate(VIEW_ID_NODE_OPS);
            }
            break;
        default: break;
    }
}

static void refresh(void)
{
    /* Auto-disarm on timeout even if no key arrives. */
    if (s.armed_row >= 0 && now_ms() > s.armed_deadline_ms) {
        TRACE("radmin", "arm_timeout_idle", "row=%d", (int)s.armed_row);
        disarm();
    }
}

static const view_descriptor_t REMOTE_ADMIN_DESC = {
    .id      = VIEW_ID_REMOTE_ADMIN,
    .name    = "radmin",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN 選", "OK 武裝/確認", "BACK 取消/返回" },
};

const view_descriptor_t *remote_admin_view_descriptor(void)
{
    return &REMOTE_ADMIN_DESC;
}
