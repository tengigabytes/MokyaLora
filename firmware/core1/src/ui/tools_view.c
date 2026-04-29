/* tools_view.c — see tools_view.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tools_view.h"

#include <stdio.h>
#include <string.h>

#include "global/ui_theme.h"

#include "key_event.h"
#include "mie/keycode.h"

typedef struct {
    const char *label;
    view_id_t   target;     /* VIEW_ID_COUNT = placeholder */
} tool_entry_t;

/* Tools T-0..T-8 per docs/ui/01-page-architecture.md spec, plus the
 * existing debug overlays (Keypad / RF / Font test) at the bottom.
 * 11 rows = 11 × 18 = 198 px content + 16 px header = 214 px (within
 * the 224 px panel area). All spec entries land as placeholders for
 * now; the existing debug overlays stay as the only working entries. */
#define MAX_ENTRIES  11

static const tool_entry_t s_entries[MAX_ENTRIES] = {
    /* Spec entries (T-1..T-8) — all TBD until each lands. */
    { "T-1 Traceroute",      VIEW_ID_COUNT      },
    { "T-2 Range Test",      VIEW_ID_COUNT      },
    { "T-3 Spectrum",        VIEW_ID_COUNT      },
    { "T-4 Sniffer",         VIEW_ID_COUNT      },
    { "T-5 LoRa Self-test",  VIEW_ID_COUNT      },
    { "T-6 GNSS Sat",        VIEW_ID_COUNT      },
    { "T-7 Pairing Code",    VIEW_ID_COUNT      },
    { "T-8 Firmware Info",   VIEW_ID_COUNT      },
    /* Existing debug overlays — release builds drop the debug-only
     * entries and the cells render as placeholders so the layout
     * stays stable for muscle memory. */
    { "Dbg: Keypad grid",    VIEW_ID_KEYPAD     },
#if MOKYA_DEBUG_VIEWS
    { "Dbg: RF overlay",     VIEW_ID_RF_DEBUG   },
    { "Dbg: Font glyph",     VIEW_ID_FONT_TEST  },
#else
    { "Dbg: RF (release)",   VIEW_ID_COUNT      },
    { "Dbg: Font (release)", VIEW_ID_COUNT      },
#endif
};

typedef struct {
    lv_obj_t *header;
    lv_obj_t *rows[MAX_ENTRIES];
    uint8_t   cur_row;
} tools_t;

static tools_t s;

/* 18 px per row so 11 entries fit the 224 px panel area
 * (16 px header + 11 × 18 = 214 px, leaves 10 px slack at the bottom).
 * 16 px font + 2 px padding stays readable. */
#define TOOLS_ROW_H  18

static lv_obj_t *make_row(lv_obj_t *parent, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, 4, y);
    lv_obj_set_size(l, 320 - 8, TOOLS_ROW_H);
    lv_obj_set_style_text_font(l, ui_font_sm16(), 0);
    lv_obj_set_style_text_color(l, ui_color(UI_COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_text(l, "");
    return l;
}

static void rebuild_rows(void)
{
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s",
                 i == s.cur_row ? ">" : " ",
                 s_entries[i].label);
        lv_label_set_text(s.rows[i], buf);
        bool placeholder = (s_entries[i].target == VIEW_ID_COUNT);
        lv_obj_set_style_text_color(s.rows[i],
            i == s.cur_row && !placeholder
                ? ui_color(UI_COLOR_ACCENT_FOCUS)
                : (placeholder
                    ? ui_color(UI_COLOR_TEXT_SECONDARY)
                    : ui_color(UI_COLOR_TEXT_PRIMARY)), 0);
    }
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
        ui_color(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_all(s.header, 0, 0);
    lv_label_set_text(s.header, "Tools / Diagnostics");

    for (int i = 0; i < MAX_ENTRIES; ++i) {
        s.rows[i] = make_row(panel, 16 + i * TOOLS_ROW_H);
    }

    rebuild_rows();
}

static void destroy(void)
{
    s.header = NULL;
    for (int i = 0; i < MAX_ENTRIES; ++i) s.rows[i] = NULL;
}

static void apply(const key_event_t *ev)
{
    if (!ev->pressed) return;
    switch (ev->keycode) {
        case MOKYA_KEY_UP:
            if (s.cur_row > 0) { s.cur_row--; rebuild_rows(); }
            break;
        case MOKYA_KEY_DOWN:
            if (s.cur_row + 1 < MAX_ENTRIES) {
                s.cur_row++; rebuild_rows();
            }
            break;
        case MOKYA_KEY_OK:
            if (s_entries[s.cur_row].target != VIEW_ID_COUNT) {
                view_router_navigate(s_entries[s.cur_row].target);
            }
            break;
        case MOKYA_KEY_BACK:
            view_router_navigate(VIEW_ID_BOOT_HOME);
            break;
        default: break;
    }
}

static void refresh(void) {}

static const view_descriptor_t TOOLS_DESC = {
    .id      = VIEW_ID_TOOLS,
    .name    = "tools",
    .create  = create,
    .destroy = destroy,
    .apply   = apply,
    .refresh = refresh,
    .flags   = 0,
    .hints   = { "up/dn pick", "OK enter", "BACK home" },
};

const view_descriptor_t *tools_view_descriptor(void)
{
    return &TOOLS_DESC;
}
