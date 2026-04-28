/* ui_theme.c — see ui_theme.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_theme.h"

#include "mie_font.h"

lv_color_t ui_color(ui_color_token_t token)
{
    switch (token) {
        case UI_COLOR_BG_PRIMARY:     return lv_color_hex(0x0B0F14);
        case UI_COLOR_BG_SECONDARY:   return lv_color_hex(0x161C24);
        case UI_COLOR_BG_PREEDIT:     return lv_color_hex(0x2A2018);
        case UI_COLOR_TEXT_PRIMARY:   return lv_color_hex(0xE6EDF3);
        case UI_COLOR_TEXT_SECONDARY: return lv_color_hex(0x7D8590);
        case UI_COLOR_TEXT_PREEDIT:   return lv_color_hex(0xFFA657);
        case UI_COLOR_ACCENT_FOCUS:   return lv_color_hex(0xFFA657);
        case UI_COLOR_ACCENT_SUCCESS: return lv_color_hex(0x39D353);
        case UI_COLOR_BORDER_NORMAL:  return lv_color_hex(0x30363D);
        case UI_COLOR_WARN_YELLOW:    return lv_color_hex(0xF1E05A);
        case UI_COLOR_WARN_RED:       return lv_color_hex(0xF85149);
        case UI_COLOR_ALERT_BG_CRIT:  return lv_color_hex(0x8B1A1A);
        case UI_COLOR_ALERT_BG_WARN:  return lv_color_hex(0x6E1A1A);
    }
    return lv_color_hex(0xFF00FF);  /* unmistakable when a token is missed */
}

const lv_font_t *ui_font_sm16(void)
{
    return mie_font_unifont_sm_16();
}
