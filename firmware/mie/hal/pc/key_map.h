// key_map.h — PC virtual half-keyboard mapping for MokyaInput Engine
// SPDX-License-Identifier: MIT
//
// Maps PC keyboard characters to MokyaLora 6×6 matrix positions.
//
// Mapping rule: use the FIRST letter printed on each physical key as the
// PC trigger. This preserves the ambiguous (half-keyboard) nature of the
// layout so the IME disambiguation logic is exercised on PC exactly as it
// would be on real hardware.
//
// Physical layout reference: docs/requirements/hardware-requirements.md §8.1
// Architecture reference:    docs/design-notes/mie-architecture.md §7.1

#pragma once
#include <stdint.h>

namespace mie {
namespace pc {

/// A single entry in the PC key map.
struct KeyMapEntry {
    int     pc_key;   ///< ASCII char or special key constant (see below)
    uint8_t row;
    uint8_t col;
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
/// Static mapping table: PC key → KeyEvent{row, col}.
/// Terminate with {-1, 0, 0}.
static constexpr KeyMapEntry kPcKeyMap[] = {
    // ── Core input rows (Bopomofo half-keyboard) ─────────────────────────
    // Row 0
    { '1',          0, 0 },  // ㄅ ㄉ
    { '3',          0, 1 },  // ˇ ˋ
    { '5',          0, 2 },  // ㄓ ˊ
    { '7',          0, 3 },  // ˙ ㄚ
    { '9',          0, 4 },  // ㄞ ㄢ ㄦ
    // Row 1
    { 'q',          1, 0 },  // ㄆ ㄊ
    { 'e',          1, 1 },  // ㄍ ㄐ
    { 't',          1, 2 },  // ㄔ ㄗ
    { 'u',          1, 3 },  // ㄧ ㄛ
    { 'o',          1, 4 },  // ㄟ ㄣ
    // Row 2
    { 'a',          2, 0 },  // ㄇ ㄋ
    { 'd',          2, 1 },  // ㄎ ㄑ
    { 'g',          2, 2 },  // ㄕ ㄘ
    { 'j',          2, 3 },  // ㄨ ㄜ
    { 'l',          2, 4 },  // ㄠ ㄤ
    // Row 3
    { 'z',          3, 0 },  // ㄈ ㄌ
    { 'c',          3, 1 },  // ㄏ ㄒ
    { 'b',          3, 2 },  // ㄖ ㄙ
    { 'm',          3, 3 },  // ㄩ ㄝ
    { '\\',         3, 4 },  // ㄡ ㄥ

    // ── Col 5 function keys ───────────────────────────────────────────────
    { KEY_F1,       0, 5 },  // FUNC
    { KEY_F2,       1, 5 },  // SET
    { KEY_BACKSPACE,2, 5 },  // BACK
    { KEY_DELETE,   3, 5 },  // DEL

    // ── Row 4 function bar ────────────────────────────────────────────────
    { '`',          4, 0 },  // MODE
    { KEY_TAB,      4, 1 },  // TAB
    { ' ',          4, 2 },  // SPACE
    { ',',          4, 3 },  // ，SYM
    { '.',          4, 4 },  // 。.？
    { '=',          4, 5 },  // VOL+

    // ── Row 5 navigation ──────────────────────────────────────────────────
    { KEY_UP,       5, 0 },  // UP
    { KEY_DOWN,     5, 1 },  // DOWN
    { KEY_LEFT,     5, 2 },  // LEFT
    { KEY_RIGHT,    5, 3 },  // RIGHT
    { KEY_ENTER,    5, 4 },  // OK
    { '-',          5, 5 },  // VOL−

    { -1,           0, 0 },  // sentinel
};
// clang-format on

} // namespace pc
} // namespace mie
