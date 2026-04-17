/* keymap_matrix.h — 6x6 keypad scan-index to mokya_keycode_t lookup table.
 *
 * This is the SOLE translation point from matrix geometry to semantic
 * keycodes. It lives under Core 1 (Apache-2.0) on purpose: everything
 * above this layer (MIE, UI, USB Control Protocol, host tooling) sees
 * only keycodes — matrix (row, col) concept does not leak past the
 * KeypadScan task (DEC-1, firmware-architecture.md §4.4).
 *
 * The (r, c) indices are the *firmware scan order*, which is NOT the
 * logical Row/Col used in the hardware-requirements.md electrical map.
 * The mapping below is copied verbatim from
 * docs/bringup/rev-a-bringup-log.md Step 6, where the scan-order layout
 * was physically verified against each switch on the PCB:
 *
 *   | r \ c | C0   | C1   | C2    | C3   | C4        | C5    |
 *   |-------|------|------|-------|------|-----------|-------|
 *   | R0    | FUNC | BACK | LEFT  | DEL  | VOL-      | UP    |
 *   | R1    | 1/2  | 3/4  | 5/6   | 7/8  | 9/0       | OK    |
 *   | R2    | Q/W  | E/R  | T/Y   | U/I  | O/P       | DOWN  |
 *   | R3    | A/S  | D/F  | G/H   | J/K  | L         | RIGHT |
 *   | R4    | Z/X  | C/V  | B/N   | M    | ㄡㄥ(\)   | SET   |
 *   | R5    | MODE | TAB  | SPACE | SYM1 | SYM2      | VOL+  |
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "mie/keycode.h"

#define KEYMAP_ROWS 6u
#define KEYMAP_COLS 6u

/* Indexed as g_keymap[row][col]; MOKYA_KEY_NONE marks unmapped cells
 * (currently none — every matrix position has a defined key). */
static const mokya_keycode_t g_keymap[KEYMAP_ROWS][KEYMAP_COLS] = {
    /* C0                C1               C2               C3               C4                     C5                 */
    { MOKYA_KEY_FUNC,    MOKYA_KEY_BACK,  MOKYA_KEY_LEFT,  MOKYA_KEY_DEL,   MOKYA_KEY_VOL_DOWN,    MOKYA_KEY_UP    }, /* R0 */
    { MOKYA_KEY_1,       MOKYA_KEY_3,     MOKYA_KEY_5,     MOKYA_KEY_7,     MOKYA_KEY_9,           MOKYA_KEY_OK    }, /* R1 */
    { MOKYA_KEY_Q,       MOKYA_KEY_E,     MOKYA_KEY_T,     MOKYA_KEY_U,     MOKYA_KEY_O,           MOKYA_KEY_DOWN  }, /* R2 */
    { MOKYA_KEY_A,       MOKYA_KEY_D,     MOKYA_KEY_G,     MOKYA_KEY_J,     MOKYA_KEY_L,           MOKYA_KEY_RIGHT }, /* R3 */
    { MOKYA_KEY_Z,       MOKYA_KEY_C,     MOKYA_KEY_B,     MOKYA_KEY_M,     MOKYA_KEY_BACKSLASH,   MOKYA_KEY_SET   }, /* R4 */
    { MOKYA_KEY_MODE,    MOKYA_KEY_TAB,   MOKYA_KEY_SPACE, MOKYA_KEY_SYM1,  MOKYA_KEY_SYM2,        MOKYA_KEY_VOL_UP}, /* R5 */
};
