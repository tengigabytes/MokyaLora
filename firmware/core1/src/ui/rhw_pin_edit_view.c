/* rhw_pin_edit_view.c — see rhw_pin_edit_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header   "S-7.10 Pin [N]"
 *   y  16..51   field 0  GPIO pin    [LEFT/RIGHT ±1, U/D ±10]
 *   y  60..95   field 1  Name        [OK opens IME modal]
 *   y 104..139  field 2  Type        [LEFT/RIGHT cycle 0..2]
 *   y 156..173  Save     [OK applies edit, returns to pins view]
 *   y 200..223  hint
 *
 * Working copy is a single phoneapi_remote_hw_pin_t in static .bss,
 * primed from rhw_pins_view's working copy on create. Save propagates
 * the edit back via rhw_pins_view_apply_slot_edit(); BACK discards
 * (returns to pins view without pushing).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rhw_pin_edit_view.h"
#include "rhw_pins_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "ime_task.h"
#include "phoneapi_cache.h"
#include "mokya_trace.h"

#define HEADER_H        16
#define FIELD_H         36   /* tall enough for two lines (label + value) */
#define ROW_TOP         16
#define ROW_GAP          8
#define ROW_COUNT        4   /* gpio / name / type / save */
#define HINT_TOP       200
#define HINT_H          24
#define PANEL_W        320

#define ROW_GPIO         0
#define ROW_NAME         1
#define ROW_TYPE         2
#define ROW_SAVE         3

typedef struct {
    lv_obj_t *header;
    lv_obj_t *labels[ROW_COUNT];
    lv_obj_t *values[ROW_COUNT];
    lv_obj_t *hint;
    uint8_t   cursor;
    uint8_t   slot;             /* 0..3 — set externally before navigate */
    phoneapi_remote_hw_pin_t work;
} rhwe_t;

/* PSRAM .bss — same rationale as rhw_pins_view's s. */
static rhwe_t s __attribute__((section(".psram_bss")));

/* ── Helpers ───────────────────────────────────────────────────────── */

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

static const char *type_str(uint8_t t)
{
    switch (t) {
        case 1: return "DIGITAL_READ";
        case 2: return "DIGITAL_WRITE";
        default: return "UNKNOWN";
    }
}

static void prime_working(void)
{
    const phoneapi_module_remote_hw_t *parent = rhw_pins_view_get_working();
    if (parent != NULL && s.slot < 4u) {
        s.work = parent->pins[s.slot];
    } else {
        memset(&s.work, 0, sizeof(s.work));
    }
}

/* ── Render ────────────────────────────────────────────────────────── */

static void render(void)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "S-7.10 Pin [%u]", (unsigned)s.slot);
    lv_label_set_text(s.header, buf);

    lv_label_set_text(s.labels[ROW_GPIO], "GPIO pin");
    snprintf(buf, sizeof(buf), "%s %u",
             (s.cursor == ROW_GPIO) ? ">" : " ",
             (unsigned)s.work.gpio_pin);
    lv_label_set_text(s.values[ROW_GPIO], buf);

    lv_label_set_text(s.labels[ROW_NAME], "Name (OK to edit)");
    snprintf(buf, sizeof(buf), "%s %s",
             (s.cursor == ROW_NAME) ? ">" : " ",
             (s.work.name[0] != '\0') ? s.work.name : "(empty)");
    lv_label_set_text(s.values[ROW_NAME], buf);

    lv_label_set_text(s.labels[ROW_TYPE], "Type");
    snprintf(buf, sizeof(buf), "%s %s (%u)",
             (s.cursor == ROW_TYPE) ? ">" : " ",
             type_str(s.work.type), (unsigned)s.work.type);
    lv_label_set_text(s.values[ROW_TYPE], buf);

    lv_label_set_text(s.labels[ROW_SAVE], "");
    snprintf(buf, sizeof(buf), "%s [Save]  back to pin list",
             (s.cursor == ROW_SAVE) ? ">" : " ");
    lv_label_set_text(s.values[ROW_SAVE], buf);

    /* Highlight focused row in accent. */
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        lv_color_t c = (i == s.cursor)
                           ? ui_color(UI_COLOR_ACCENT_FOCUS)
                           : ui_color(UI_COLOR_TEXT_PRIMARY);
        lv_obj_set_style_text_color(s.values[i], c, 0);
    }
}

/* ── IME callback ──────────────────────────────────────────────────── */

static void on_name_done(bool committed, const char *utf8,
                         uint16_t byte_len, void *ctx)
{
    (void)ctx;
    if (!committed) return;
    if (byte_len > sizeof(s.work.name) - 1u) byte_len = sizeof(s.work.name) - 1u;
    memcpy(s.work.name, utf8, byte_len);
    s.work.name[byte_len] = '\0';
    render();
}

static void open_name_edit(void)
{
    if (ime_request_text_active()) return;
    ime_text_request_t req = {
        .prompt    = "Pin name",
        .initial   = (s.work.name[0] != '\0') ? s.work.name : NULL,
        .max_bytes = (uint16_t)(sizeof(s.work.name) - 1u),
        .mode_hint = IME_TEXT_MODE_DEFAULT,
        .flags     = IME_TEXT_FLAG_ALLOW_EMPTY | IME_TEXT_FLAG_ASCII_ONLY,
        .layout    = IME_TEXT_LAYOUT_FULLSCREEN,
        .draft_id  = 0u,
    };
    (void)ime_request_text(&req, on_name_done, NULL);
}

/* ── Lifecycle / input ─────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    /* Preserve s.slot across create/destroy — set by external API. */
    uint8_t slot = s.slot;
    memset(&s, 0, sizeof(s));
    s.slot = slot;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));

    int y = ROW_TOP;
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        s.labels[i] = make_label(panel, 4, y,
                                 PANEL_W - 8, HEADER_H,
                                 ui_color(UI_COLOR_TEXT_SECONDARY));
        s.values[i] = make_label(panel, 4, y + HEADER_H,
                                 PANEL_W - 8, HEADER_H,
                                 ui_color(UI_COLOR_TEXT_PRIMARY));
        y += FIELD_H + ROW_GAP;
    }
    s.hint = make_label(panel, 4, HINT_TOP, PANEL_W - 8, HINT_H,
                        ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.hint, "L/R adj  OK edit/Save  BACK 取消");

    prime_working();
    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.hint = NULL;
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        s.labels[i] = NULL;
        s.values[i] = NULL;
    }
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
        case MOKYA_KEY_RIGHT: {
            int delta = (ev->keycode == MOKYA_KEY_RIGHT) ? +1 : -1;
            if (s.cursor == ROW_GPIO) {
                int v = (int)s.work.gpio_pin + delta;
                if (v < 0)   v = 255;     /* wrap to keep editing cheap */
                if (v > 255) v = 0;
                s.work.gpio_pin = (uint8_t)v;
                render();
            } else if (s.cursor == ROW_TYPE) {
                int v = (int)s.work.type + delta;
                if (v < 0) v = 2;
                if (v > 2) v = 0;
                s.work.type = (uint8_t)v;
                render();
            }
            break;
        }
        case MOKYA_KEY_OK:
            if (s.cursor == ROW_NAME) {
                open_name_edit();
            } else if (s.cursor == ROW_SAVE) {
                rhw_pins_view_apply_slot_edit(s.slot, &s.work);
                TRACE("rhwe", "save",
                      "slot=%u gpio=%u type=%u",
                      (unsigned)s.slot, (unsigned)s.work.gpio_pin,
                      (unsigned)s.work.type);
                view_router_navigate(VIEW_ID_T10_RHW_PINS);
            } else if (s.cursor == ROW_GPIO || s.cursor == ROW_TYPE) {
                /* OK on adjustable field is a no-op shortcut into Save. */
                s.cursor = ROW_SAVE;
                render();
            }
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_T10_RHW_PINS);
            break;
        default: break;
    }
}

static void refresh(void) { /* edits are cursor-driven; no polling */ }

static const view_descriptor_t RHW_PIN_EDIT_DESC = {
    .id      = VIEW_ID_T10_RHW_PIN_EDIT,
    .name    = "rhw_pin_e",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN nav", "L/R adj · OK Save", "BACK 取消" },
};

const view_descriptor_t *rhw_pin_edit_view_descriptor(void)
{
    return &RHW_PIN_EDIT_DESC;
}

/* ── Public API ────────────────────────────────────────────────────── */

void rhw_pin_edit_view_set_target(uint8_t slot)
{
    s.slot = (slot < 4u) ? slot : 0u;
    s.cursor = 0u;
}
