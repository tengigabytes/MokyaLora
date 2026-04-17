/* key_name.h — mokya_keycode_t → short display name (≤4 chars).
 *
 * Internal-only helper for Core 1 debug/diagnostic UIs (Phase C keypad
 * view). Not part of any public API — do NOT use for user-facing text
 * or for the USB Control Protocol, which reference keys by their
 * mokya_keycode_t numeric value.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "mie/keycode.h"

static inline const char *key_name_short(mokya_keycode_t kc)
{
    switch (kc) {
    case MOKYA_KEY_1:         return "1/2";
    case MOKYA_KEY_3:         return "3/4";
    case MOKYA_KEY_5:         return "5/6";
    case MOKYA_KEY_7:         return "7/8";
    case MOKYA_KEY_9:         return "9/0";
    case MOKYA_KEY_FUNC:      return "FUN";
    case MOKYA_KEY_Q:         return "Q/W";
    case MOKYA_KEY_E:         return "E/R";
    case MOKYA_KEY_T:         return "T/Y";
    case MOKYA_KEY_U:         return "U/I";
    case MOKYA_KEY_O:         return "O/P";
    case MOKYA_KEY_SET:       return "SET";
    case MOKYA_KEY_A:         return "A/S";
    case MOKYA_KEY_D:         return "D/F";
    case MOKYA_KEY_G:         return "G/H";
    case MOKYA_KEY_J:         return "J/K";
    case MOKYA_KEY_L:         return "L";
    case MOKYA_KEY_BACK:      return "BK";
    case MOKYA_KEY_Z:         return "Z/X";
    case MOKYA_KEY_C:         return "C/V";
    case MOKYA_KEY_B:         return "B/N";
    case MOKYA_KEY_M:         return "M";
    case MOKYA_KEY_BACKSLASH: return "\\";
    case MOKYA_KEY_DEL:       return "DEL";
    case MOKYA_KEY_MODE:      return "MOD";
    case MOKYA_KEY_TAB:       return "TAB";
    case MOKYA_KEY_SPACE:     return "SPC";
    case MOKYA_KEY_SYM1:      return ",";
    case MOKYA_KEY_SYM2:      return ".";
    case MOKYA_KEY_VOL_UP:    return "V+";
    case MOKYA_KEY_UP:        return "UP";
    case MOKYA_KEY_DOWN:      return "DN";
    case MOKYA_KEY_LEFT:      return "LF";
    case MOKYA_KEY_RIGHT:     return "RT";
    case MOKYA_KEY_OK:        return "OK";
    case MOKYA_KEY_VOL_DOWN:  return "V-";
    case MOKYA_KEY_POWER:     return "PWR";
    default:                  return "?";
    }
}
