/* channel_add_view.c — see channel_add_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header "B-3 加入頻道  slot=N"
 *   y  16..39   row 0  "Name :  <text>"
 *   y  40..63   row 1  "Role :  PRIMARY/SECONDARY  (←/→)"
 *   y  64..87   row 2  "PSK  :  random 32 B"   (read-only)
 *   y  88..111  row 3  ">> Save & broadcast <<"
 *   y 112..207  status (cascade SET ack / encoder errors)
 *
 * Random PSK source: Pico SDK `get_rand_32` (ROSC-derived).
 * Save path: phoneapi_encode_admin_set_channel — single
 *   AdminMessage.set_channel sub-message with {index, settings{psk,
 *   name}, role}, sent locally (mp.from = mp.to = self) so
 *   AdminModule on Core 0 picks it up + persists to channelFile.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "channel_add_view.h"
#include "channels_view.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "phoneapi_cache.h"
#include "phoneapi_encode.h"
#include "ime_task.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define ROW_H           24
#define ROW_COUNT        4
#define STATUS_TOP      (HEADER_H + ROW_COUNT * ROW_H + 4)
#define PANEL_W        320

typedef enum {
    CADD_ROW_NAME = 0,
    CADD_ROW_ROLE = 1,
    CADD_ROW_PSK  = 2,
    CADD_ROW_SAVE = 3,
} cadd_row_t;

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *status;
    uint8_t   active_idx;
    uint8_t   cursor;
    char      name[12];        /* 11 chars + NUL */
    uint8_t   name_len;
    uint8_t   role;            /* phoneapi_channel_role_t */
    uint8_t   psk[32];
    bool      psk_generated;
} cadd_t;

static cadd_t s;

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

/* Timer-seeded xorshift32 — non-crypto but deterministic enough as
 * channel-key separator. The seed mixes time_us_64 (free-running 1 MHz
 * counter, low bits change every microsecond) with the previous PSK
 * state in case generate_psk() is called twice in the same boot. v2
 * should swap this for pico_rand or MbedTLS DRBG. */
static uint32_t s_rng_state;

static uint32_t rng_next(void)
{
    if (s_rng_state == 0u) {
        uint64_t t = time_us_64();
        s_rng_state = (uint32_t)(t ^ (t >> 32));
        if (s_rng_state == 0u) s_rng_state = 0xA5DEADBEu;
    }
    uint32_t x = s_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x <<  5;
    s_rng_state = x;
    return x;
}

static void generate_psk(void)
{
    for (uint32_t i = 0; i < 8u; ++i) {
        uint32_t v = rng_next();
        s.psk[i * 4 + 0] = (uint8_t)(v >> 0);
        s.psk[i * 4 + 1] = (uint8_t)(v >> 8);
        s.psk[i * 4 + 2] = (uint8_t)(v >> 16);
        s.psk[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    s.psk_generated = true;
}

static const char *role_str(uint8_t role)
{
    switch (role) {
        case PHONEAPI_CHAN_ROLE_PRIMARY:   return "PRIMARY";
        case PHONEAPI_CHAN_ROLE_SECONDARY: return "SECONDARY";
        case PHONEAPI_CHAN_ROLE_DISABLED:  return "DISABLED";
        default:                           return "?";
    }
}

/* ── Render ─────────────────────────────────────────────────────────── */

static void render(void)
{
    char buf[80];

    snprintf(buf, sizeof(buf), "B-3 加入頻道  slot=%u", (unsigned)s.active_idx);
    lv_label_set_text(s.header, buf);

    snprintf(buf, sizeof(buf), "%sName :  %s",
             s.cursor == CADD_ROW_NAME ? ">" : " ",
             s.name_len > 0u ? s.name : "(tap OK to enter)");
    lv_label_set_text(s.rows[CADD_ROW_NAME], buf);

    snprintf(buf, sizeof(buf), "%sRole :  %s  (LEFT/RIGHT)",
             s.cursor == CADD_ROW_ROLE ? ">" : " ",
             role_str(s.role));
    lv_label_set_text(s.rows[CADD_ROW_ROLE], buf);

    /* PSK row stays dim — read-only. */
    snprintf(buf, sizeof(buf), " PSK  :  %s",
             s.psk_generated ? "random 32 B" : "(generating...)");
    lv_label_set_text(s.rows[CADD_ROW_PSK], buf);

    snprintf(buf, sizeof(buf), "%s>> Save & broadcast <<",
             s.cursor == CADD_ROW_SAVE ? ">" : " ");
    lv_label_set_text(s.rows[CADD_ROW_SAVE], buf);

    /* Colours: focus on cursor, dim PSK row, normal otherwise. */
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        lv_obj_set_style_text_color(s.rows[i],
            (i == s.cursor)
                ? ui_color(UI_COLOR_ACCENT_FOCUS)
                : (i == CADD_ROW_PSK)
                    ? ui_color(UI_COLOR_TEXT_SECONDARY)
                    : ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    }
}

/* ── Actions ────────────────────────────────────────────────────────── */

static void set_status(const char *msg)
{
    if (s.status) lv_label_set_text(s.status, msg);
}

static void on_name_done(bool committed, const char *utf8,
                         uint16_t byte_len, void *ctx)
{
    (void)ctx;
    if (!committed) {
        set_status("Name edit cancelled");
        return;
    }
    if (byte_len > 11u) byte_len = 11u;
    memcpy(s.name, utf8, byte_len);
    s.name[byte_len] = '\0';
    s.name_len = (uint8_t)byte_len;
    render();
}

static void open_name_edit(void)
{
    if (ime_request_text_active()) {
        set_status("IME busy — try again");
        return;
    }
    ime_text_request_t req = {
        .prompt    = "Channel name",
        .initial   = (s.name_len > 0u) ? s.name : NULL,
        .max_bytes = 11,
        .mode_hint = IME_TEXT_MODE_DEFAULT,
        .flags     = IME_TEXT_FLAG_ALLOW_EMPTY,
        .layout    = IME_TEXT_LAYOUT_FULLSCREEN,
        .draft_id  = 0u,
    };
    if (!ime_request_text(&req, on_name_done, NULL)) {
        set_status("IME request failed");
    }
}

static void cycle_role(int delta)
{
    /* Cycle through PRIMARY ↔ SECONDARY. v1 doesn't expose DISABLED via
     * the create flow — picking DISABLED defeats the purpose of B-3. */
    if (delta > 0) {
        s.role = (s.role == PHONEAPI_CHAN_ROLE_SECONDARY)
                     ? PHONEAPI_CHAN_ROLE_PRIMARY
                     : PHONEAPI_CHAN_ROLE_SECONDARY;
    } else {
        s.role = (s.role == PHONEAPI_CHAN_ROLE_PRIMARY)
                     ? PHONEAPI_CHAN_ROLE_SECONDARY
                     : PHONEAPI_CHAN_ROLE_PRIMARY;
    }
    render();
}

static void fire_save(void)
{
    if (s.name_len == 0u) {
        set_status("Set a name first (cursor->Name, OK)");
        return;
    }
    uint32_t pid = 0u;
    bool ok = phoneapi_encode_admin_set_channel(
        s.active_idx,
        s.name, s.name_len,
        s.psk, 32u,
        s.role,
        &pid);
    char buf[80];
    if (ok) {
        snprintf(buf, sizeof(buf),
                 "set_channel sent pid=%#lx (apply pending)",
                 (unsigned long)pid);
        TRACE("cadd", "saved",
              "idx=%u name_len=%u role=%u pid=%lu",
              (unsigned)s.active_idx, (unsigned)s.name_len,
              (unsigned)s.role, (unsigned long)pid);
    } else {
        snprintf(buf, sizeof(buf), "Encode failed (no my_node? overflow?)");
        TRACE_BARE("cadd", "save_fail");
    }
    set_status(buf);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    uint8_t target = channels_view_get_active_index();
    if (target >= PHONEAPI_CHANNEL_COUNT) target = 0u;
    memset(&s, 0, sizeof(s));
    s.active_idx = target;
    s.role       = PHONEAPI_CHAN_ROLE_SECONDARY;   /* sensible default */
    generate_psk();

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        s.rows[i] = make_label(panel, 4, HEADER_H + (int)i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.status = make_label(panel, 4, STATUS_TOP, PANEL_W - 8, ROW_H * 2,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    render();
    TRACE("cadd", "create", "slot=%u", (unsigned)s.active_idx);
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
        case MOKYA_KEY_LEFT:
            if (s.cursor == CADD_ROW_ROLE) cycle_role(-1);
            break;
        case MOKYA_KEY_RIGHT:
            if (s.cursor == CADD_ROW_ROLE) cycle_role(+1);
            break;
        case MOKYA_KEY_OK:
            switch (s.cursor) {
                case CADD_ROW_NAME: open_name_edit(); break;
                case CADD_ROW_SAVE: fire_save();      break;
                /* OK on Role / PSK rows is a no-op (Role uses LEFT/RIGHT,
                 * PSK is read-only). */
                default: break;
            }
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_CHANNELS);
            break;
        default: break;
    }
}

static void refresh(void) { /* no live-data refresh needed */ }

static const view_descriptor_t CHANNEL_ADD_DESC = {
    .id      = VIEW_ID_CHANNEL_ADD,
    .name    = "ch_add",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN 移動", "OK 進入/送出", "BACK 返回" },
};

const view_descriptor_t *channel_add_view_descriptor(void)
{
    return &CHANNEL_ADD_DESC;
}
