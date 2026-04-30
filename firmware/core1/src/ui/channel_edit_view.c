/* channel_edit_view.c — see channel_edit_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header   "B-2 編輯 ch3 LongFast"
 *   y  16..39   row 0    Name        :  LongFast
 *   y  40..63   row 1    Position    :  10 bits   (LEFT/RIGHT ±1)
 *   y  64..87   row 2    Muted       :  [off]/[on]  (OK toggles)
 *   y  88..111  row 3    Role        :  PRIMARY     (LEFT/RIGHT cycle 3)
 *   y 112..135  row 4    Uplink      :  [on]/[off]  (OK toggles)
 *   y 136..159  row 5    Downlink    :  [on]/[off]  (OK toggles)
 *   y 160..183  row 6    PSK 32B  id 0xABCDEF01  (read-only combined info)
 *   y 184..207  status (last SET ack/error/in-flight)
 *
 * Cursor walks rows 0..5 (writable). The read-only info row at the
 * bottom renders dim so it's visually clear it's not selectable.
 *
 * Each writable change pushes settings_client_send_set + send_commit
 * (no reboot — IPC_CFG_CHANNEL_* are all soft-reload keys). Reply
 * intake from settings_client_dispatch_reply lands in the in-process
 * queue but we don't poll it here — the cascade replays Channel after
 * commit and phoneapi_cache_change_seq bumping is the success signal.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "channel_edit_view.h"
#include "channels_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "settings_client.h"
#include "ipc_protocol.h"
#include "ime_task.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define ROW_H           24
#define WRITABLE_ROWS    6                /* name/pos/muted/role/uplink/downlink */
#define READONLY_ROWS    1                /* combined psk+id info */
#define TOTAL_ROWS      (WRITABLE_ROWS + READONLY_ROWS)
#define STATUS_TOP      (HEADER_H + TOTAL_ROWS * ROW_H + 0)
#define PANEL_W        320

typedef enum {
    ROW_NAME     = 0,
    ROW_POS      = 1,
    ROW_MUTED    = 2,
    ROW_ROLE     = 3,
    ROW_UPLINK   = 4,
    ROW_DOWNLINK = 5,
    ROW_INFO     = 6,    /* combined PSK summary + channel id, read-only */
} row_id_t;

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[TOTAL_ROWS];
    lv_obj_t *status;
    uint8_t   active_idx;       /* channel index currently being edited */
    uint8_t   cursor;            /* 0..WRITABLE_ROWS-1 */
    uint32_t  last_change_seq;
} cedit_t;

static cedit_t s;

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
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}

static const char *role_full(uint8_t role)
{
    switch (role) {
        case PHONEAPI_CHAN_ROLE_DISABLED:  return "DISABLED";
        case PHONEAPI_CHAN_ROLE_PRIMARY:   return "PRIMARY";
        case PHONEAPI_CHAN_ROLE_SECONDARY: return "SECONDARY";
        default:                           return "?";
    }
}

static void render(void)
{
    phoneapi_channel_t ch;
    bool have = phoneapi_cache_get_channel(s.active_idx, &ch);

    char buf[80];
    /* Header */
    if (have) {
        snprintf(buf, sizeof(buf), "B-2 編輯 ch%u  %s",
                 (unsigned)s.active_idx,
                 ch.name[0] ? ch.name : "(no name)");
    } else {
        snprintf(buf, sizeof(buf), "B-2 編輯 ch%u  (cache empty)",
                 (unsigned)s.active_idx);
    }
    lv_label_set_text(s.header, buf);

    /* Writable rows */
    snprintf(buf, sizeof(buf), "%sName       :  %s",
             s.cursor == ROW_NAME ? ">" : " ",
             have && ch.name[0] ? ch.name : "(empty)");
    lv_label_set_text(s.rows[ROW_NAME], buf);

    if (have && ch.has_module_settings) {
        snprintf(buf, sizeof(buf), "%sPosition   :  %u bits  (L/R ±1)",
                 s.cursor == ROW_POS ? ">" : " ",
                 (unsigned)ch.module_position_precision);
    } else {
        snprintf(buf, sizeof(buf), "%sPosition   :  --",
                 s.cursor == ROW_POS ? ">" : " ");
    }
    lv_label_set_text(s.rows[ROW_POS], buf);

    snprintf(buf, sizeof(buf), "%sMuted      :  [%s]  (OK toggle)",
             s.cursor == ROW_MUTED ? ">" : " ",
             have && ch.has_module_settings && ch.module_is_muted
                 ? "on" : "off");
    lv_label_set_text(s.rows[ROW_MUTED], buf);

    snprintf(buf, sizeof(buf), "%sRole       :  %s  (L/R cycle)",
             s.cursor == ROW_ROLE ? ">" : " ",
             have ? role_full(ch.role) : "--");
    lv_label_set_text(s.rows[ROW_ROLE], buf);

    snprintf(buf, sizeof(buf), "%sUplink     :  [%s]  (OK toggle)",
             s.cursor == ROW_UPLINK ? ">" : " ",
             have && ch.uplink_enabled ? "on" : "off");
    lv_label_set_text(s.rows[ROW_UPLINK], buf);

    snprintf(buf, sizeof(buf), "%sDownlink   :  [%s]  (OK toggle)",
             s.cursor == ROW_DOWNLINK ? ">" : " ",
             have && ch.downlink_enabled ? "on" : "off");
    lv_label_set_text(s.rows[ROW_DOWNLINK], buf);

    /* Apply colour by focus — writable rows */
    for (uint8_t i = 0; i < WRITABLE_ROWS; ++i) {
        lv_obj_set_style_text_color(s.rows[i],
            (i == s.cursor) ? ui_color(UI_COLOR_ACCENT_FOCUS)
                            : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }

    /* Combined read-only info row (PSK summary + channel id) */
    if (have) {
        const char *enc;
        if      (ch.psk_len == 0u)  enc = "OPEN";
        else if (ch.psk_len == 1u)  enc = "default";
        else                        enc = (ch.psk_len == 16u) ? "PSK16" : "PSK32";
        snprintf(buf, sizeof(buf), " %s  id 0x%08lX",
                 enc, (unsigned long)ch.channel_id);
    } else {
        snprintf(buf, sizeof(buf), " --  id --");
    }
    lv_label_set_text(s.rows[ROW_INFO], buf);
    lv_obj_set_style_text_color(s.rows[ROW_INFO],
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
}

/* ── IPC SET helpers ────────────────────────────────────────────────── */

static void set_status(const char *msg)
{
    if (s.status) {
        lv_label_set_text(s.status, msg);
    }
}

static void send_set_and_commit(uint16_t key, const void *val, uint16_t vlen)
{
    bool ok_set = settings_client_send_set(key, s.active_idx, val, vlen);
    bool ok_commit = ok_set && settings_client_send_commit(/*reboot=*/false);
    if (ok_set && ok_commit) {
        char buf[64];
        snprintf(buf, sizeof(buf), "SET 0x%04X ch%u  ok (commit pending)",
                 (unsigned)key, (unsigned)s.active_idx);
        set_status(buf);
        TRACE("cedit", "set",
              "k=0x%04x ch=%u vlen=%u",
              (unsigned)key, (unsigned)s.active_idx, (unsigned)vlen);
    } else {
        set_status("SET push failed (ring full?)");
        TRACE("cedit", "set_fail",
              "k=0x%04x ch=%u set=%u commit=%u",
              (unsigned)key, (unsigned)s.active_idx,
              (unsigned)ok_set, (unsigned)ok_commit);
    }
}

static void on_name_done(bool committed, const char *utf8,
                         uint16_t byte_len, void *ctx)
{
    (void)ctx;
    if (!committed) {
        set_status("Name edit cancelled");
        return;
    }
    if (byte_len == 0u) {
        /* Allow empty name (Meshtastic treats it as "use default" for
         * primary channels).  Send an empty SET — Core 0 clamps. */
    }
    send_set_and_commit(IPC_CFG_CHANNEL_NAME,
                        (const uint8_t *)utf8, byte_len);
}

static void open_name_edit(void)
{
    if (ime_request_text_active()) return;
    phoneapi_channel_t ch;
    bool have = phoneapi_cache_get_channel(s.active_idx, &ch);
    /* IME's `initial` field is documented but the ime_request_text
     * trampoline pre-fills g_text from it — see template_text.c
     * comment.  Pass current name so user edits-in-place. */
    ime_text_request_t req = {
        .prompt    = "Channel name",
        .initial   = (have && ch.name[0]) ? ch.name : NULL,
        .max_bytes = 11,             /* proto cap */
        .mode_hint = IME_TEXT_MODE_DEFAULT,
        .flags     = IME_TEXT_FLAG_ALLOW_EMPTY,
        .layout    = IME_TEXT_LAYOUT_FULLSCREEN,
        .draft_id  = 0u,             /* no draft persistence — short edit */
    };
    if (!ime_request_text(&req, on_name_done, NULL)) {
        set_status("IME busy — try again");
    }
}

static void bump_position_precision(int delta)
{
    phoneapi_channel_t ch;
    if (!phoneapi_cache_get_channel(s.active_idx, &ch) ||
        !ch.has_module_settings) {
        set_status("Position precision unavailable for this channel");
        return;
    }
    int v = (int)ch.module_position_precision + delta;
    if (v < 0)  v = 0;
    if (v > 32) v = 32;
    if ((uint32_t)v == ch.module_position_precision) {
        return;       /* clamp hit, no change */
    }
    uint32_t out = (uint32_t)v;
    /* IpcPayloadConfigValue carries little-endian raw bytes; uint32 is
     * 4 bytes LSB first. RP2350 is little-endian so direct memcpy
     * matches the wire format. */
    send_set_and_commit(IPC_CFG_CHANNEL_MODULE_POSITION_PRECISION,
                        &out, sizeof(out));
}

static void toggle_muted(void)
{
    phoneapi_channel_t ch;
    if (!phoneapi_cache_get_channel(s.active_idx, &ch) ||
        !ch.has_module_settings) {
        set_status("Mute toggle unavailable for this channel");
        return;
    }
    uint8_t new_val = ch.module_is_muted ? 0u : 1u;
    send_set_and_commit(IPC_CFG_CHANNEL_MODULE_IS_MUTED,
                        &new_val, sizeof(new_val));
}

static void cycle_role(int delta)
{
    /* DISABLED(0) → PRIMARY(1) → SECONDARY(2) → DISABLED(0) → ... */
    phoneapi_channel_t ch;
    uint8_t cur = 0u;
    if (phoneapi_cache_get_channel(s.active_idx, &ch)) cur = ch.role;
    int v = (int)cur + delta;
    if (v < 0) v = 2;
    if (v > 2) v = 0;
    uint8_t new_val = (uint8_t)v;
    send_set_and_commit(IPC_CFG_CHANNEL_ROLE, &new_val, sizeof(new_val));
}

static void toggle_uplink(void)
{
    phoneapi_channel_t ch;
    bool cur = phoneapi_cache_get_channel(s.active_idx, &ch) ? ch.uplink_enabled : false;
    uint8_t new_val = cur ? 0u : 1u;
    send_set_and_commit(IPC_CFG_CHANNEL_UPLINK_ENABLED, &new_val, sizeof(new_val));
}

static void toggle_downlink(void)
{
    phoneapi_channel_t ch;
    bool cur = phoneapi_cache_get_channel(s.active_idx, &ch) ? ch.downlink_enabled : false;
    uint8_t new_val = cur ? 0u : 1u;
    send_set_and_commit(IPC_CFG_CHANNEL_DOWNLINK_ENABLED, &new_val, sizeof(new_val));
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    /* Pull the requested index from the B-1 stash; preserve cursor on
     * LRU re-create. */
    uint8_t target = channels_view_get_active_index();
    if (target == 0xFFu) target = 0u;
    uint8_t saved_cursor = s.cursor;
    memset(&s, 0, sizeof(s));
    s.active_idx = target;
    s.cursor     = (saved_cursor < WRITABLE_ROWS) ? saved_cursor : 0u;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    for (uint8_t i = 0; i < TOTAL_ROWS; ++i) {
        s.rows[i] = make_label(panel, 4, HEADER_H + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }

    s.status = make_label(panel, 4, STATUS_TOP, PANEL_W - 8, ROW_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.status = NULL;
    for (int i = 0; i < TOTAL_ROWS; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cursor > 0u) {
                s.cursor--;
                render();
            }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cursor + 1u < WRITABLE_ROWS) {
                s.cursor++;
                render();
            }
            break;
        case MOKYA_KEY_LEFT:
            /* Numeric / cycle tweak axis. UP/DOWN stay pure cursor moves
             * so navigation is consistent across row types. */
            if      (s.cursor == ROW_POS)  bump_position_precision(-1);
            else if (s.cursor == ROW_ROLE) cycle_role(-1);
            break;
        case MOKYA_KEY_RIGHT:
            if      (s.cursor == ROW_POS)  bump_position_precision(+1);
            else if (s.cursor == ROW_ROLE) cycle_role(+1);
            break;
        case MOKYA_KEY_OK:
            switch (s.cursor) {
                case ROW_NAME:     open_name_edit();  break;
                case ROW_MUTED:    toggle_muted();    break;
                case ROW_UPLINK:   toggle_uplink();   break;
                case ROW_DOWNLINK: toggle_downlink(); break;
                /* OK on POS / ROLE rows does nothing — adjustment is
                 * via LEFT/RIGHT. */
                default: break;
            }
            break;
        case MOKYA_KEY_SET:
            /* B-4: open share-URL view for the channel currently
             * being edited. Active index is already set on
             * channels_view_set_active_index by the B-1 entry path. */
            view_router_navigate(VIEW_ID_CHANNEL_SHARE);
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_CHANNELS);
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

static const view_descriptor_t CHANNEL_EDIT_DESC = {
    .id      = VIEW_ID_CHANNEL_EDIT,
    .name    = "ch_edit",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN 移動", "OK 編輯", "SET 分享 / BACK" },
};

const view_descriptor_t *channel_edit_view_descriptor(void)
{
    return &CHANNEL_EDIT_DESC;
}
