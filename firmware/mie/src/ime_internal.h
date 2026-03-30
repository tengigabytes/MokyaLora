// ime_internal.h — Private helpers shared across ime_*.cpp translation units.
// SPDX-License-Identifier: MIT
//
// This header is NOT part of the public API. Include only from src/*.cpp.

#pragma once

#include <mie/ime_logic.h>
#include <cstring>

namespace mie {

// ── Direct Mode label table ───────────────────────────────────────────────
// Defined here (not in ime_keys.cpp) so that ime_display.cpp can also use
// find_direct_entry() for compound_input_str() / matched_prefix_compound_bytes().

struct DirectEntry {
    uint8_t     row, col;
    // [0]=phoneme-primary  [1]=phoneme-secondary  [2]=phoneme-tertiary
    // [3]=letter-primary   [4]=letter-secondary
    // nullptr marks absent slots.
    const char* labels[5];
};

// clang-format off
static const DirectEntry kDirectTable[] = {
    { 0, 0, { "ㄅ","ㄉ",nullptr,"1","2"      } },
    { 0, 1, { "ˇ", "ˋ",nullptr,"3","4"      } },
    { 0, 2, { "ㄓ","ˊ",nullptr,"5","6"      } },
    { 0, 3, { "˙", "ㄚ",nullptr,"7","8"     } },
    { 0, 4, { "ㄞ","ㄢ","ㄦ","9","0"        } },

    { 1, 0, { "ㄆ","ㄊ",nullptr,"Q","W"     } },
    { 1, 1, { "ㄍ","ㄐ",nullptr,"E","R"     } },
    { 1, 2, { "ㄔ","ㄗ",nullptr,"T","Y"     } },
    { 1, 3, { "ㄧ","ㄛ",nullptr,"U","I"     } },
    { 1, 4, { "ㄟ","ㄣ",nullptr,"O","P"     } },

    { 2, 0, { "ㄇ","ㄋ",nullptr,"A","S"     } },
    { 2, 1, { "ㄎ","ㄑ",nullptr,"D","F"     } },
    { 2, 2, { "ㄕ","ㄘ",nullptr,"G","H"     } },
    { 2, 3, { "ㄨ","ㄜ",nullptr,"J","K"     } },
    { 2, 4, { "ㄠ","ㄤ",nullptr,"L",nullptr } },

    { 3, 0, { "ㄈ","ㄌ",nullptr,"Z","X"     } },
    { 3, 1, { "ㄏ","ㄒ",nullptr,"C","V"     } },
    { 3, 2, { "ㄖ","ㄙ",nullptr,"B","N"     } },
    { 3, 3, { "ㄩ","ㄝ",nullptr,"M",nullptr } },
    { 3, 4, { "ㄡ","ㄥ",nullptr,nullptr,nullptr } },
};
// clang-format on

static inline const DirectEntry* find_direct_entry(uint8_t row, uint8_t col) {
    static const int kCount = (int)(sizeof(kDirectTable) / sizeof(kDirectTable[0]));
    for (int i = 0; i < kCount; ++i)
        if (kDirectTable[i].row == row && kDirectTable[i].col == col)
            return &kDirectTable[i];
    return nullptr;
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
