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
/// Each physical half-keyboard key is reachable via TWO PC keys so that
/// both phonemes / letters on a key are equally accessible during testing.
/// Terminate with {-1, 0, 0}.
static constexpr KeyMapEntry kPcKeyMap[] = {
    // ── Core input rows (Bopomofo half-keyboard) ─────────────────────────
    // Row 0  (primary odd digit, secondary even digit)
    { '1',          0, 0 },  // ㄅ ㄉ  — primary
    { '2',          0, 0 },  // ㄅ ㄉ  — secondary
    { '3',          0, 1 },  // ˇ ˋ   — primary
    { '4',          0, 1 },  // ˇ ˋ   — secondary
    { '5',          0, 2 },  // ㄓ ˊ  — primary
    { '6',          0, 2 },  // ㄓ ˊ  — secondary
    { '7',          0, 3 },  // ˙ ㄚ  — primary
    { '8',          0, 3 },  // ˙ ㄚ  — secondary
    { '9',          0, 4 },  // ㄞ ㄢ ㄦ — primary
    { '0',          0, 4 },  // ㄞ ㄢ ㄦ — secondary
    { '-',          0, 4 },  // ㄞ ㄢ ㄦ — alternate (ergonomic for ㄦ)
    // Row 1  (primary first QWERTY letter of the pair, secondary second)
    { 'q',          1, 0 },  // ㄆ ㄊ  — primary
    { 'w',          1, 0 },  // ㄆ ㄊ  — secondary
    { 'e',          1, 1 },  // ㄍ ㄐ  — primary
    { 'r',          1, 1 },  // ㄍ ㄐ  — secondary
    { 't',          1, 2 },  // ㄔ ㄗ  — primary
    { 'y',          1, 2 },  // ㄔ ㄗ  — secondary
    { 'u',          1, 3 },  // ㄧ ㄛ  — primary
    { 'i',          1, 3 },  // ㄧ ㄛ  — secondary
    { 'o',          1, 4 },  // ㄟ ㄣ  — primary
    { 'p',          1, 4 },  // ㄟ ㄣ  — secondary
    // Row 2
    { 'a',          2, 0 },  // ㄇ ㄋ  — primary
    { 's',          2, 0 },  // ㄇ ㄋ  — secondary
    { 'd',          2, 1 },  // ㄎ ㄑ  — primary
    { 'f',          2, 1 },  // ㄎ ㄑ  — secondary
    { 'g',          2, 2 },  // ㄕ ㄘ  — primary
    { 'h',          2, 2 },  // ㄕ ㄘ  — secondary
    { 'j',          2, 3 },  // ㄨ ㄜ  — primary
    { 'k',          2, 3 },  // ㄨ ㄜ  — secondary
    { 'l',          2, 4 },  // ㄠ ㄤ  — primary
    { ';',          2, 4 },  // ㄠ ㄤ  — alternate (mirrors l)
    // Row 3
    { 'z',          3, 0 },  // ㄈ ㄌ  — primary
    { 'x',          3, 0 },  // ㄈ ㄌ  — secondary
    { 'c',          3, 1 },  // ㄏ ㄒ  — primary
    { 'v',          3, 1 },  // ㄏ ㄒ  — secondary
    { 'b',          3, 2 },  // ㄖ ㄙ  — primary
    { 'n',          3, 2 },  // ㄖ ㄙ  — secondary
    { 'm',          3, 3 },  // ㄩ ㄝ  — primary
    { ',',          3, 3 },  // ㄩ ㄝ  — alternate (mirrors m)
    { '\\',         3, 4 },  // ㄡ ㄥ  — primary
    { '.',          3, 4 },  // ㄡ ㄥ  — alternate-1 (mirrors \)
    { '/',          3, 4 },  // ㄡ ㄥ  — alternate-2

    // ── Col 5 function keys ───────────────────────────────────────────────
    { KEY_F1,       0, 5 },  // FUNC
    { KEY_F2,       1, 5 },  // SET
    { KEY_BACKSPACE,2, 5 },  // BACK
    { KEY_DELETE,   3, 5 },  // DEL

    // ── Row 4 function bar ────────────────────────────────────────────────
    { '`',          4, 0 },  // MODE
    { KEY_TAB,      4, 1 },  // TAB
    { ' ',          4, 2 },  // SPACE
    { '[',          4, 3 },  // ，SYM
    { ']',          4, 4 },  // 。.？
    { '=',          4, 5 },  // VOL+

    // ── Row 5 navigation ──────────────────────────────────────────────────
    { KEY_UP,       5, 0 },  // UP
    { KEY_DOWN,     5, 1 },  // DOWN
    { KEY_LEFT,     5, 2 },  // LEFT
    { KEY_RIGHT,    5, 3 },  // RIGHT
    { KEY_ENTER,    5, 4 },  // OK
    { '_',          5, 5 },  // VOL−  (moved from '-'; '-' now maps to (0,4) ㄦ)

    { -1,           0, 0 },  // sentinel
};
// clang-format on

} // namespace pc
} // namespace mie
