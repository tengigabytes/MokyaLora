// SPDX-License-Identifier: MIT
// ime_internal.h — Private shared declarations for ime_*.cpp translation units.
//
// Not part of the public API; include only from src/*.cpp.

#pragma once

#include <mie/ime_logic.h>
#include <mie/keycode.h>
#include <cstdint>
#include <cstring>

namespace mie {

// ── Key entry table (20 input keys) ──────────────────────────────────────────
//
// Each key carries up to three label groups:
//   phonemes[3]     — ZH compound display (e.g. {"ㄅ","ㄉ",nullptr} or
//                     {"ㄞ","ㄢ","ㄦ"}). First non-null entry is the primary
//                     phoneme; the dict-key byte is slot+0x21.
//   digit_slots[2]  — row 0 only: {"1","2"} etc. Non-digit rows: all nullptr.
//   letter_slots[4] — Direct cycling order (a→s→A→S). Single-letter keys
//                     (L, M) leave inner slots nullptr. BACKSLASH has no
//                     letters at all (NOP in Direct mode).
//
// The slot order in kKeyTable[] is the canonical dictionary-key encoding:
// index i ↔ dict byte (i + 0x21). Matching gen_dict.py must produce this
// ordering or dictionaries will not match the runtime search.

struct KeyEntry {
    mokya_keycode_t keycode;
    const char*     phonemes[3];
    const char*     digit_slots[2];
    const char*     letter_slots[4];
};

extern const KeyEntry kKeyTable[20];

// Returns 0..19 if kc is one of the 20 input keys; -1 otherwise.
int             keycode_to_input_slot(mokya_keycode_t kc);

// Convenience: returns nullptr for non-input keys.
const KeyEntry* find_key_entry(mokya_keycode_t kc);

// Number of non-null entries at the front of arr[N].
template <int N>
inline int count_slots(const char* const (&arr)[N]) {
    int n = 0;
    while (n < N && arr[n]) ++n;
    return n;
}

// ── UTF-8 helpers ────────────────────────────────────────────────────────────

inline int utf8_char_count(const char* s) {
    int n = 0;
    while (*s) { if (((unsigned char)*s & 0xC0) != 0x80) ++n; ++s; }
    return n;
}

// ── Tone-aware ranking helpers (SmartZh) ─────────────────────────────────────

// Extract the tone the user intended at the matched prefix:
//   34 = tone 3 or 4 (trailing byte is ˇ/ˋ key, 0x22)
//    1 = tone 1 (next byte after matched prefix is 0x20 first-tone marker)
//    0 = unspecified
inline int extract_tone_intent(const char* key_buf, int seq_len, int prefix_len) {
    if (prefix_len == 0)                                  return 0;
    if ((uint8_t)key_buf[prefix_len - 1] == 0x22)         return 34;
    if (prefix_len < seq_len &&
        (uint8_t)key_buf[prefix_len] == 0x20)             return 1;
    return 0;
}

// Sort tier for a candidate under a given tone intent.
//   0 = best  — single-char match
//   1 =          multi-char match
//   2 =          single-char mismatch
//   3 = worst — multi-char mismatch
inline int tone_tier(const Candidate& c, int intent) {
    bool single = (utf8_char_count(c.word) == 1);
    bool match;
    if (intent == 0)        match = true;
    else if (intent == 1)   match = (c.tone == 1);
    else if (intent == 34)  match = (c.tone == 3 || c.tone == 4);
    else                    match = (c.tone == (uint8_t)intent);
    if ( single &&  match)  return 0;
    if (!single &&  match)  return 1;
    if ( single && !match)  return 2;
    return 3;
}

} // namespace mie
