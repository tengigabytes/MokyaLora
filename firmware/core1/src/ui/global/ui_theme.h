/* ui_theme.h — Global colour tokens + typography helpers.
 *
 * Mirrors the 11-token palette in docs/ui/00-design-charter.md. Views may
 * either pull tokens through `ui_color()` or use the `LV_*` literal macros
 * directly. New code should prefer tokens — if the spec ever changes a
 * colour, only this file edits.
 *
 * The font helper wraps `mie_font_unifont_sm_16()` so views don't need to
 * include the MIE font header just to set a label font.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_UI_THEME_H
#define MOKYA_CORE1_UI_THEME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_COLOR_BG_PRIMARY     = 0,  /* #0B0F14 — main background          */
    UI_COLOR_BG_SECONDARY   = 1,  /* #161C24 — cards / inputs           */
    UI_COLOR_BG_PREEDIT     = 2,  /* #2A2018 — IME preedit block        */
    UI_COLOR_TEXT_PRIMARY   = 3,  /* #E6EDF3 — main text                */
    UI_COLOR_TEXT_SECONDARY = 4,  /* #7D8590 — hints / units            */
    UI_COLOR_TEXT_PREEDIT   = 5,  /* #FFA657 — preedit text             */
    UI_COLOR_ACCENT_FOCUS   = 6,  /* #FFA657 — focus / TX / IME mode    */
    UI_COLOR_ACCENT_SUCCESS = 7,  /* #39D353 — RX / unread / 3D fix     */
    UI_COLOR_BORDER_NORMAL  = 8,  /* #30363D — default border / dim TX  */
    UI_COLOR_WARN_YELLOW    = 9,  /* #F1E05A — caution / 2D fix         */
    UI_COLOR_WARN_RED       = 10, /* #F85149 — critical / no GPS / SOS  */
    UI_COLOR_ALERT_BG_CRIT  = 11, /* #8B1A1A — SOS bar background       */
    UI_COLOR_ALERT_BG_WARN  = 12, /* #6E1A1A — extreme low battery bg   */
} ui_color_token_t;

lv_color_t ui_color(ui_color_token_t token);

/* Typography — small (16 px) is the global default; everything else
 * derives from it. Pulled via getter so tokens can change later without
 * touching call sites. */
const lv_font_t *ui_font_sm16(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  /* MOKYA_CORE1_UI_THEME_H */
