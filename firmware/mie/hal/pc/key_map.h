// key_map.h — PC virtual half-keyboard mapping for MokyaInput Engine
// SPDX-License-Identifier: MIT
//
// Maps PC keyboard characters to MokyaLora semantic keycodes (MOKYA_KEY_*).
//
// Mapping rule: use the FIRST letter printed on each physical key as the
// PC trigger. This preserves the ambiguous (half-keyboard) nature of the
// layout so the IME disambiguation logic is exercised on PC exactly as it
// would be on real hardware. Both halves of each key are reachable (two
// PC keys per physical key) so tests can exercise both labels.
//
// Physical layout reference: docs/requirements/hardware-requirements.md §8.1
// Architecture reference:    docs/design-notes/mie-architecture.md §7.1

#pragma once
#include <stdint.h>
#include <mie/keycode.h>

namespace mie {
namespace pc {

/// A single entry in the PC key map.
struct KeyMapEntry {
    int             pc_key;     ///< ASCII char or special key constant (see below)
    mokya_keycode_t keycode;    ///< MokyaLora semantic keycode (<mie/keycode.h>)
};

/// Special key constants (values above 0x7F to avoid ASCII collision).
enum SpecialKey : int {
    KEY_BACKSPACE = 0x08,
    KEY_TAB       = 0x09,
    KEY_ENTER     = 0x0D,
    KEY_ESCAPE    = 0x1B,
    KEY_DELETE    = 0x7F,
    KEY_F1        = 0x101,
    KEY_F2        = 0x102,
    KEY_UP        = 0x103,
    KEY_DOWN      = 0x104,
    KEY_LEFT      = 0x105,
    KEY_RIGHT     = 0x106,
};

// clang-format off
/// Static mapping table: PC key → MokyaLora keycode.
/// Each physical half-keyboard key is reachable via TWO PC keys so that
/// both phonemes / letters on a key are equally accessible during testing.
/// Terminate with {-1, MOKYA_KEY_NONE}.
static constexpr KeyMapEntry kPcKeyMap[] = {
    // ── Core input rows (Bopomofo half-keyboard) ─────────────────────────
    { '1',           MOKYA_KEY_1 },          // ㄅ ㄉ — primary
    { '2',           MOKYA_KEY_1 },          // ㄅ ㄉ — secondary
    { '3',           MOKYA_KEY_3 },          // ˇ ˋ  — primary
    { '4',           MOKYA_KEY_3 },          // ˇ ˋ  — secondary
    { '5',           MOKYA_KEY_5 },          // ㄓ ˊ — primary
    { '6',           MOKYA_KEY_5 },          // ㄓ ˊ — secondary
    { '7',           MOKYA_KEY_7 },          // ˙ ㄚ — primary
    { '8',           MOKYA_KEY_7 },          // ˙ ㄚ — secondary
    { '9',           MOKYA_KEY_9 },          // ㄞ ㄢ ㄦ — primary
    { '0',           MOKYA_KEY_9 },          // ㄞ ㄢ ㄦ — secondary
    { '-',           MOKYA_KEY_9 },          // ㄞ ㄢ ㄦ — alternate (ergonomic for ㄦ)

    { 'q',           MOKYA_KEY_Q },          // ㄆ ㄊ — primary
    { 'w',           MOKYA_KEY_Q },          // ㄆ ㄊ — secondary
    { 'e',           MOKYA_KEY_E },          // ㄍ ㄐ — primary
    { 'r',           MOKYA_KEY_E },          // ㄍ ㄐ — secondary
    { 't',           MOKYA_KEY_T },          // ㄔ ㄗ — primary
    { 'y',           MOKYA_KEY_T },          // ㄔ ㄗ — secondary
    { 'u',           MOKYA_KEY_U },          // ㄧ ㄛ — primary
    { 'i',           MOKYA_KEY_U },          // ㄧ ㄛ — secondary
    { 'o',           MOKYA_KEY_O },          // ㄟ ㄣ — primary
    { 'p',           MOKYA_KEY_O },          // ㄟ ㄣ — secondary

    { 'a',           MOKYA_KEY_A },          // ㄇ ㄋ — primary
    { 's',           MOKYA_KEY_A },          // ㄇ ㄋ — secondary
    { 'd',           MOKYA_KEY_D },          // ㄎ ㄑ — primary
    { 'f',           MOKYA_KEY_D },          // ㄎ ㄑ — secondary
    { 'g',           MOKYA_KEY_G },          // ㄕ ㄘ — primary
    { 'h',           MOKYA_KEY_G },          // ㄕ ㄘ — secondary
    { 'j',           MOKYA_KEY_J },          // ㄨ ㄜ — primary
    { 'k',           MOKYA_KEY_J },          // ㄨ ㄜ — secondary
    { 'l',           MOKYA_KEY_L },          // ㄠ ㄤ — primary
    { ';',           MOKYA_KEY_L },          // ㄠ ㄤ — alternate (mirrors l)

    { 'z',           MOKYA_KEY_Z },          // ㄈ ㄌ — primary
    { 'x',           MOKYA_KEY_Z },          // ㄈ ㄌ — secondary
    { 'c',           MOKYA_KEY_C },          // ㄏ ㄒ — primary
    { 'v',           MOKYA_KEY_C },          // ㄏ ㄒ — secondary
    { 'b',           MOKYA_KEY_B },          // ㄖ ㄙ — primary
    { 'n',           MOKYA_KEY_B },          // ㄖ ㄙ — secondary
    { 'm',           MOKYA_KEY_M },          // ㄩ ㄝ — primary
    { ',',           MOKYA_KEY_M },          // ㄩ ㄝ — alternate (mirrors m)
    { '\\',          MOKYA_KEY_BACKSLASH },  // ㄡ ㄥ — primary
    { '.',           MOKYA_KEY_BACKSLASH },  // ㄡ ㄥ — alternate-1 (mirrors \)
    { '/',           MOKYA_KEY_BACKSLASH },  // ㄡ ㄥ — alternate-2

    // ── Col 5 function keys ───────────────────────────────────────────────
    // Note: PC Backspace maps to MOKYA_KEY_DEL (MIE's delete) so it behaves
    // like a backspace in typical PC editing contexts. MOKYA_KEY_BACK is not
    // bound to any PC key — BACK is for UI-level navigation (not MIE's scope)
    // and the REPL uses ESC to quit.
    { KEY_F1,        MOKYA_KEY_FUNC },
    { KEY_F2,        MOKYA_KEY_SET  },
    { KEY_BACKSPACE, MOKYA_KEY_DEL  },
    { KEY_DELETE,    MOKYA_KEY_DEL  },

    // ── Row 4 function bar ────────────────────────────────────────────────
    { '`',           MOKYA_KEY_MODE   },
    { KEY_TAB,       MOKYA_KEY_TAB    },
    { ' ',           MOKYA_KEY_SPACE  },
    { '[',           MOKYA_KEY_SYM1   },  // ，SYM
    { ']',           MOKYA_KEY_SYM2   },  // 。.？
    { '=',           MOKYA_KEY_VOL_UP },

    // ── Row 5 navigation ──────────────────────────────────────────────────
    { KEY_UP,        MOKYA_KEY_UP       },
    { KEY_DOWN,      MOKYA_KEY_DOWN     },
    { KEY_LEFT,      MOKYA_KEY_LEFT     },
    { KEY_RIGHT,     MOKYA_KEY_RIGHT    },
    { KEY_ENTER,     MOKYA_KEY_OK       },
    { '_',           MOKYA_KEY_VOL_DOWN },  // moved from '-'; '-' now maps to MOKYA_KEY_9 (ㄦ)

    { -1,            MOKYA_KEY_NONE },  // sentinel
};
// clang-format on

} // namespace pc
} // namespace mie
