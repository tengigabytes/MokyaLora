/* rhw_pins_view.c — see rhw_pins_view.h.
 *
 * Layout (panel 320 × 224):
 *   y   0..15   header
 *   y  16..33   row 0  Module enabled              [bool, LEFT/RIGHT toggle]
 *   y  34..51   row 1  Allow undefined pin access  [bool, LEFT/RIGHT toggle]
 *   y  52..69   row 2  Pin count                   [u8 0..4, LEFT/RIGHT ±1]
 *   y  70..73   separator
 *   y  74..91   row 3  Slot 0 summary              [OK → edit]
 *   y  92..109  row 4  Slot 1 summary
 *   y 110..127  row 5  Slot 2 summary
 *   y 128..145  row 6  Slot 3 summary
 *   y 146..149  separator
 *   y 150..167  row 7  Apply                       [OK → commit]
 *   y 200..223  hint
 *
 * Working copy lives in static .bss; primed from phoneapi_cache on
 * create. Edits are local until Apply. SET frames sent only for keys
 * that differ from cache (cheap optimization keeps the COMMIT short).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rhw_pins_view.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "global/ui_theme.h"
#include "key_event.h"
#include "mie/keycode.h"
#include "settings_client.h"
#include "phoneapi_cache.h"
#include "ipc_protocol.h"
#include "mokya_trace.h"

/* Forward — defined in rhw_pin_edit_view.c, declared here to avoid an
 * extra header. */
extern void rhw_pin_edit_view_set_target(uint8_t slot);

#define HEADER_H        16
#define ROW_H           18
#define ROW_TOP         16
#define MODULE_ROWS      3   /* enabled / allow_undef / pin_count */
#define SLOT_ROWS        4
#define APPLY_ROW_IDX   (MODULE_ROWS + SLOT_ROWS)   /* row 7 */
#define ROW_COUNT       (APPLY_ROW_IDX + 1)         /* 8 total */
#define HINT_TOP       200
#define HINT_H          24
#define PANEL_W        320

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *hint;
    uint8_t   cursor;
    /* Working copy of moduleConfig.remote_hardware. */
    phoneapi_module_remote_hw_t work;
    phoneapi_module_remote_hw_t baseline;  /* for diff at Apply time */
} rhwp_t;

/* PSRAM .bss to keep SRAM tight — LVGL pointer access from Core 1 is
 * cache-coherent within Core 1's own world. SWD inspection of `s`
 * is not needed (IPC test scripts cover end-to-end). */
static rhwp_t s __attribute__((section(".psram_bss")));

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
        case 1: return "Rd";
        case 2: return "Wr";
        default: return "--";
    }
}

static void prime_working(void)
{
    phoneapi_module_remote_hw_t cached;
    if (phoneapi_cache_get_module_remote_hw(&cached)) {
        s.work = cached;
    } else {
        memset(&s.work, 0, sizeof(s.work));
    }
    s.baseline = s.work;
}

/* ── Render ────────────────────────────────────────────────────────── */

static void render(void)
{
    char buf[80];
    lv_label_set_text(s.header, "S-7.10 Remote Hardware");

    /* Row 0 — enabled */
    snprintf(buf, sizeof(buf), "%s%s Module enabled",
             (s.cursor == 0) ? ">" : " ",
             s.work.enabled ? " [X]" : " [ ]");
    lv_label_set_text(s.rows[0], buf);

    /* Row 1 — allow_undef */
    snprintf(buf, sizeof(buf), "%s%s Allow undefined pin access",
             (s.cursor == 1) ? ">" : " ",
             s.work.allow_undefined_pin_access ? " [X]" : " [ ]");
    lv_label_set_text(s.rows[1], buf);

    /* Row 2 — pin_count */
    snprintf(buf, sizeof(buf), "%s Pin count: %u / 4",
             (s.cursor == 2) ? ">" : " ",
             (unsigned)s.work.pin_count);
    lv_label_set_text(s.rows[2], buf);

    /* Rows 3..6 — slot summary */
    for (uint8_t i = 0; i < SLOT_ROWS; ++i) {
        const phoneapi_remote_hw_pin_t *p = &s.work.pins[i];
        bool valid = (i < s.work.pin_count);
        if (valid) {
            snprintf(buf, sizeof(buf),
                     "%s [%u] gpio=%-3u %-9s %s",
                     (s.cursor == MODULE_ROWS + i) ? ">" : " ",
                     (unsigned)i, (unsigned)p->gpio_pin,
                     (p->name[0] != '\0') ? p->name : "(no name)",
                     type_str(p->type));
        } else {
            snprintf(buf, sizeof(buf), "%s [%u] -- empty --",
                     (s.cursor == MODULE_ROWS + i) ? ">" : " ",
                     (unsigned)i);
        }
        lv_label_set_text(s.rows[MODULE_ROWS + i], buf);
        lv_obj_set_style_text_color(s.rows[MODULE_ROWS + i],
            ui_color(valid ? UI_COLOR_TEXT_PRIMARY
                           : UI_COLOR_TEXT_SECONDARY), 0);
    }

    /* Row 7 — Apply */
    snprintf(buf, sizeof(buf), "%s [Apply]  send SET + COMMIT_CONFIG",
             (s.cursor == APPLY_ROW_IDX) ? ">" : " ");
    lv_label_set_text(s.rows[APPLY_ROW_IDX], buf);

    /* Highlight focused row in accent. */
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        if (i >= MODULE_ROWS && i < APPLY_ROW_IDX) continue;  /* slot color set above */
        lv_color_t c = (i == s.cursor)
                           ? ui_color(UI_COLOR_ACCENT_FOCUS)
                           : ui_color(UI_COLOR_TEXT_PRIMARY);
        lv_obj_set_style_text_color(s.rows[i], c, 0);
    }
    if (s.cursor >= MODULE_ROWS && s.cursor < APPLY_ROW_IDX) {
        lv_obj_set_style_text_color(s.rows[s.cursor],
            ui_color(UI_COLOR_ACCENT_FOCUS), 0);
    }
}

/* ── Apply (send SETs for diffs + COMMIT) ──────────────────────────── */

static int apply_diff(void)
{
    int sent = 0;

    if (s.work.enabled != s.baseline.enabled) {
        uint8_t v = s.work.enabled ? 1u : 0u;
        if (settings_client_send_set(IPC_CFG_RHW_ENABLED, 0, &v, 1u)) sent++;
    }
    if (s.work.allow_undefined_pin_access != s.baseline.allow_undefined_pin_access) {
        uint8_t v = s.work.allow_undefined_pin_access ? 1u : 0u;
        if (settings_client_send_set(IPC_CFG_RHW_ALLOW_UNDEFINED_PIN_ACCESS,
                                     0, &v, 1u)) sent++;
    }

    /* Per-slot diffs first (before count change), so that count's
     * truncation effect doesn't reorder values mid-flight. */
    uint8_t slots = (s.work.pin_count > s.baseline.pin_count)
                        ? s.work.pin_count : s.baseline.pin_count;
    for (uint8_t i = 0; i < slots && i < 4; ++i) {
        const phoneapi_remote_hw_pin_t *w = &s.work.pins[i];
        const phoneapi_remote_hw_pin_t *b = &s.baseline.pins[i];
        if (w->gpio_pin != b->gpio_pin) {
            uint8_t v = w->gpio_pin;
            if (settings_client_send_set(IPC_CFG_RHW_PIN_GPIO, i, &v, 1u)) sent++;
        }
        if (strncmp(w->name, b->name, sizeof(w->name)) != 0) {
            uint16_t nlen = (uint16_t)strnlen(w->name, sizeof(w->name) - 1u);
            if (settings_client_send_set(IPC_CFG_RHW_PIN_NAME, i,
                                          w->name, nlen)) sent++;
        }
        if (w->type != b->type) {
            uint8_t v = w->type;
            if (settings_client_send_set(IPC_CFG_RHW_PIN_TYPE, i, &v, 1u)) sent++;
        }
    }

    /* Pin count last. */
    if (s.work.pin_count != s.baseline.pin_count) {
        uint8_t v = s.work.pin_count;
        if (settings_client_send_set(IPC_CFG_RHW_PIN_COUNT, 0, &v, 1u)) sent++;
    }

    if (sent > 0) {
        settings_client_send_commit(false);
    }
    return sent;
}

/* ── Lifecycle / input ─────────────────────────────────────────────── */

static void create(lv_obj_t *panel)
{
    uint8_t saved_cursor = s.cursor;
    memset(&s, 0, sizeof(s));
    s.cursor = (saved_cursor < ROW_COUNT) ? saved_cursor : 0u;

    lv_obj_set_style_bg_color(panel, ui_color(UI_COLOR_BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);

    s.header = make_label(panel, 4, 0, PANEL_W - 8, HEADER_H,
                          ui_color(UI_COLOR_TEXT_SECONDARY));
    for (uint8_t i = 0; i < ROW_COUNT; ++i) {
        s.rows[i] = make_label(panel, 4, ROW_TOP + i * ROW_H,
                               PANEL_W - 8, ROW_H,
                               ui_color(UI_COLOR_TEXT_PRIMARY));
    }
    s.hint = make_label(panel, 4, HINT_TOP, PANEL_W - 8, HINT_H,
                        ui_color(UI_COLOR_TEXT_SECONDARY));
    lv_label_set_text(s.hint, "UP/DN nav  L/R adj  OK edit  BACK cancel");

    prime_working();
    render();
}

static void destroy(void)
{
    s.header = NULL;
    s.hint = NULL;
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
        case MOKYA_KEY_RIGHT: {
            int delta = (ev->keycode == MOKYA_KEY_RIGHT) ? +1 : -1;
            if (s.cursor == 0) {
                s.work.enabled = !s.work.enabled;
            } else if (s.cursor == 1) {
                s.work.allow_undefined_pin_access = !s.work.allow_undefined_pin_access;
            } else if (s.cursor == 2) {
                int nc = (int)s.work.pin_count + delta;
                if (nc < 0) nc = 0;
                if (nc > 4) nc = 4;
                s.work.pin_count = (uint8_t)nc;
            }
            render();
            break;
        }
        case MOKYA_KEY_OK: {
            if (s.cursor >= MODULE_ROWS && s.cursor < APPLY_ROW_IDX) {
                /* Slot row — open per-slot editor. */
                uint8_t slot = (uint8_t)(s.cursor - MODULE_ROWS);
                rhw_pin_edit_view_set_target(slot);
                TRACE("rhwp", "edit", "slot=%u", (unsigned)slot);
                view_router_navigate(VIEW_ID_T10_RHW_PIN_EDIT);
            } else if (s.cursor == APPLY_ROW_IDX) {
                int n = apply_diff();
                TRACE("rhwp", "apply", "sets=%d", n);
                /* Re-baseline so a second Apply is a no-op. */
                s.baseline = s.work;
                /* Bounce back to modules index to mirror channel_add flow. */
                view_router_navigate(VIEW_ID_MODULES_INDEX);
            }
            break;
        }
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_MODULES_INDEX);
            break;
        default: break;
    }
}

static void refresh(void) { /* edits are cursor-driven; no polling */ }

static const view_descriptor_t RHW_PINS_DESC = {
    .id      = VIEW_ID_T10_RHW_PINS,
    .name    = "rhw_pins",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "UP/DN nav", "OK edit / Apply", "BACK 模組" },
};

const view_descriptor_t *rhw_pins_view_descriptor(void)
{
    return &RHW_PINS_DESC;
}

/* ── Public API for rhw_pin_edit_view ──────────────────────────────── */

const phoneapi_module_remote_hw_t *rhw_pins_view_get_working(void)
{
    return &s.work;
}

void rhw_pins_view_apply_slot_edit(uint8_t slot,
                                    const phoneapi_remote_hw_pin_t *pin)
{
    if (slot >= 4u || pin == NULL) return;
    s.work.pins[slot] = *pin;
    /* If user populated a previously-empty slot, optimistically extend
     * pin_count so the slot becomes visible / saved. */
    if (slot >= s.work.pin_count) {
        s.work.pin_count = (uint8_t)(slot + 1u);
        if (s.work.pin_count > 4u) s.work.pin_count = 4u;
    }
}
