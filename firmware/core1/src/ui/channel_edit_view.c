/* channel_edit_view.c — see channel_edit_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header   "B-2 編輯 ch3 LongFast"
 *   y  16..39   row 0    Name        :  LongFast
 *   y  40..63   row 1    Position    :  10 bits   (UP/DOWN ±1)
 *   y  64..87   row 2    Muted       :  [off]/[on] (OK toggles)
 *   y  88..111  row 3    PSK         :  default key  (read-only)
 *   y 112..135  row 4    Role        :  PRIMARY      (read-only)
 *   y 136..159  row 5    Channel ID  :  0xABCDEF01   (read-only)
 *   y 160..183  status (last SET ack/error/in-flight)
 *
 * Cursor walks rows 0..2 only (writable).  Read-only rows render at
 * dim colour so it's visually obvious they aren't selectable.
 *
 * Each writable change pushes settings_client_send_set + send_commit
 * (no reboot — IPC_CFG_CHANNEL_* are all soft-reload keys).  Reply
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
#define WRITABLE_ROWS    3                /* name / pos / muted */
#define READONLY_ROWS    3                /* psk / role / channel id */
#define TOTAL_ROWS      (WRITABLE_ROWS + READONLY_ROWS)
#define STATUS_TOP      (HEADER_H + TOTAL_ROWS * ROW_H + 4)
#define PANEL_W        320

typedef enum {
    ROW_NAME    = 0,
    ROW_POS     = 1,
    ROW_MUTED   = 2,
    ROW_PSK     = 3,
    ROW_ROLE    = 4,
    ROW_CHANID  = 5,
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
        snprintf(buf, sizeof(buf), "%sPosition   :  %u bits  (UP/DN ±1)",
                 s.cursor == ROW_POS ? ">" : " ",
                 (unsigned)ch.module_position_precision);
    } else {
        snprintf(buf, sizeof(buf), "%sPosition   :  --",
                 s.cursor == ROW_POS ? ">" : " ");
    }
    lv_label_set_text(s.rows[ROW_POS], buf);

    snprintf(buf, sizeof(buf), "%sMuted      :  [%s]",
             s.cursor == ROW_MUTED ? ">" : " ",
             have && ch.has_module_settings && ch.module_is_muted
                 ? "on" : "off");
    lv_label_set_text(s.rows[ROW_MUTED], buf);

    /* Apply colour by focus — writable rows */
    for (uint8_t i = 0; i < WRITABLE_ROWS; ++i) {
        lv_obj_set_style_text_color(s.rows[i],
            (i == s.cursor) ? ui_color(UI_COLOR_ACCENT_FOCUS)
                            : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }

    /* Read-only rows */
    if (have) {
        const char *enc;
        if      (ch.psk_len == 0u)  enc = "OPEN (no encryption)";
        else if (ch.psk_len == 1u)  enc = "default key";
        else                        enc = (ch.psk_len == 16u) ? "PSK 16 B" : "PSK 32 B";
        snprintf(buf, sizeof(buf), " PSK        :  %s", enc);
    } else {
        snprintf(buf, sizeof(buf), " PSK        :  --");
    }
    lv_label_set_text(s.rows[ROW_PSK], buf);

    snprintf(buf, sizeof(buf), " Role       :  %s",
             have ? role_full(ch.role) : "--");
    lv_label_set_text(s.rows[ROW_ROLE], buf);

    if (have) {
        snprintf(buf, sizeof(buf), " Channel ID :  0x%08lX",
                 (unsigned long)ch.channel_id);
    } else {
        snprintf(buf, sizeof(buf), " Channel ID :  --");
    }
    lv_label_set_text(s.rows[ROW_CHANID], buf);

    for (uint8_t i = WRITABLE_ROWS; i < TOTAL_ROWS; ++i) {
        lv_obj_set_style_text_color(s.rows[i],
            ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    }
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
            /* Numeric tweak axis on the position row — UP/DOWN stay
             * pure cursor moves so navigation is consistent across
             * row types. */
            if (s.cursor == ROW_POS) bump_position_precision(-1);
            break;
        case MOKYA_KEY_RIGHT:
            if (s.cursor == ROW_POS) bump_position_precision(+1);
            break;
        case MOKYA_KEY_OK:
            switch (s.cursor) {
                case ROW_NAME:  open_name_edit();  break;
                case ROW_MUTED: toggle_muted();    break;
                /* OK on POS row does nothing — adjustment is via
                 * UP/DOWN/LEFT/RIGHT. */
                default: break;
            }
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
    .hints   = { "UP/DN 移動", "OK 編輯", "BACK 列表" },
};

const view_descriptor_t *channel_edit_view_descriptor(void)
{
    return &CHANNEL_EDIT_DESC;
}
