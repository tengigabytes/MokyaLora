// ime_internal.h — Private helpers shared across ime_*.cpp translation units.
// SPDX-License-Identifier: MIT
//
// This header is NOT part of the public API. Include only from src/*.cpp.

#pragma once

#include <mie/ime_logic.h>
#include <mie/keycode.h>
#include <cstring>

namespace mie {

// ── Direct Mode label table (indexed by keycode) ──────────────────────────
// Defined here (not in ime_keys.cpp) so that ime_display.cpp can also use
// find_direct_entry() for compound_input_str() / matched_prefix_compound_bytes().

struct DirectEntry {
    mokya_keycode_t keycode;
    // [0]=phoneme-primary  [1]=phoneme-secondary  [2]=phoneme-tertiary
    // [3]=letter-primary   [4]=letter-secondary
    // nullptr marks absent slots.
    const char* labels[5];
};

// clang-format off
static const DirectEntry kDirectTable[] = {
    { MOKYA_KEY_1, { "ㄅ","ㄉ",nullptr,"1","2"      } },
    { MOKYA_KEY_3, { "ˇ", "ˋ",nullptr,"3","4"      } },
    { MOKYA_KEY_5, { "ㄓ","ˊ",nullptr,"5","6"      } },
    { MOKYA_KEY_7, { "˙", "ㄚ",nullptr,"7","8"     } },
    { MOKYA_KEY_9, { "ㄞ","ㄢ","ㄦ","9","0"        } },

    { MOKYA_KEY_Q, { "ㄆ","ㄊ",nullptr,"Q","W"     } },
    { MOKYA_KEY_E, { "ㄍ","ㄐ",nullptr,"E","R"     } },
    { MOKYA_KEY_T, { "ㄔ","ㄗ",nullptr,"T","Y"     } },
    { MOKYA_KEY_U, { "ㄧ","ㄛ",nullptr,"U","I"     } },
    { MOKYA_KEY_O, { "ㄟ","ㄣ",nullptr,"O","P"     } },

    { MOKYA_KEY_A, { "ㄇ","ㄋ",nullptr,"A","S"     } },
    { MOKYA_KEY_D, { "ㄎ","ㄑ",nullptr,"D","F"     } },
    { MOKYA_KEY_G, { "ㄕ","ㄘ",nullptr,"G","H"     } },
    { MOKYA_KEY_J, { "ㄨ","ㄜ",nullptr,"J","K"     } },
    { MOKYA_KEY_L, { "ㄠ","ㄤ",nullptr,"L",nullptr } },

    { MOKYA_KEY_Z,         { "ㄈ","ㄌ",nullptr,"Z","X"     } },
    { MOKYA_KEY_C,         { "ㄏ","ㄒ",nullptr,"C","V"     } },
    { MOKYA_KEY_B,         { "ㄖ","ㄙ",nullptr,"B","N"     } },
    { MOKYA_KEY_M,         { "ㄩ","ㄝ",nullptr,"M",nullptr } },
    { MOKYA_KEY_BACKSLASH, { "ㄡ","ㄥ",nullptr,nullptr,nullptr } },
};
// clang-format on

static inline const DirectEntry* find_direct_entry(mokya_keycode_t kc) {
    static const int kCount = (int)(sizeof(kDirectTable) / sizeof(kDirectTable[0]));
    for (int i = 0; i < kCount; ++i)
        if (kDirectTable[i].keycode == kc)
            return &kDirectTable[i];
    return nullptr;
}

// ── Dictionary-key byte encoding ──────────────────────────────────────────
// MIE dictionary keys are a compact 20-key alphabet over rows 0-3 cols 0-4
// (the physical Bopomofo/Latin input area). Each input key maps to one byte
// in the range 0x21..0x34 (slot 0..19 + 0x21). kInputKeys[] lists the
// input keycodes in slot order; the inverse mapping is handled by
// keycode_to_input_slot() below.

static const mokya_keycode_t kInputKeys[20] = {
    MOKYA_KEY_1, MOKYA_KEY_3, MOKYA_KEY_5, MOKYA_KEY_7, MOKYA_KEY_9,
    MOKYA_KEY_Q, MOKYA_KEY_E, MOKYA_KEY_T, MOKYA_KEY_U, MOKYA_KEY_O,
    MOKYA_KEY_A, MOKYA_KEY_D, MOKYA_KEY_G, MOKYA_KEY_J, MOKYA_KEY_L,
    MOKYA_KEY_Z, MOKYA_KEY_C, MOKYA_KEY_B, MOKYA_KEY_M, MOKYA_KEY_BACKSLASH,
};

// Returns 0..19 if kc is one of the 20 dictionary-input keys, else -1.
static inline int keycode_to_input_slot(mokya_keycode_t kc) {
    for (int i = 0; i < 20; ++i)
        if (kInputKeys[i] == kc) return i;
    return -1;
}

// Inverse of keycode_to_input_slot(). slot must be 0..19.
static inline mokya_keycode_t input_slot_to_keycode(int slot) {
    return kInputKeys[slot];
}

// ── UTF-8 helper ──────────────────────────────────────────────────────────
static inline int utf8_char_count(const char* s) {
    int n = 0;
    while (*s) { if (((unsigned char)*s & 0xC0) != 0x80) ++n; ++s; }
    return n;
}

// ── Tone-intent extraction ────────────────────────────────────────────────
// Returns the tone the user intended for the matched prefix:
//   34 = tone 3 or 4  (key 0x22: ˇ/ˋ dedicated tone key)
//    1 = tone 1        (0x20 first-tone marker immediately after prefix)
//    0 = unspecified
static inline int extract_tone_intent(const char* key_buf, int seq_len, int prefix_len) {
    if (prefix_len == 0) return 0;
    if ((uint8_t)key_buf[prefix_len - 1] == 0x22) return 34;
    if (prefix_len < seq_len && (uint8_t)key_buf[prefix_len] == 0x20) return 1;
    return 0;
}

// ── Tone-tier comparator ─────────────────────────────────────────────────
// Returns sort tier (0=best, 3=worst) for a candidate given the tone intent.
static inline int tone_tier(const Candidate& c, int intent) {
    bool single = (utf8_char_count(c.word) == 1);
    bool match;
    if (intent == 0)        match = true;
    else if (intent == 1)   match = (c.tone == 1);
    else if (intent == 34)  match = (c.tone == 3 || c.tone == 4);
    else                    match = (c.tone == (uint8_t)intent);
    if ( single &&  match) return 0;
    if (!single &&  match) return 1;
    if ( single && !match) return 2;
    return 3;
}

// ── Mode predicate ────────────────────────────────────────────────────────
static inline bool is_direct_mode(InputMode m) {
    return m == InputMode::DirectUpper ||
           m == InputMode::DirectLower ||
           m == InputMode::DirectBopomofo;
}

} // namespace mie
