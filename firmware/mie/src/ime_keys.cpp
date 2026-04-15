// ime_keys.cpp — Static phoneme/symbol tables and all key-to-label lookups.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <cstring>

namespace mie {

// ── Primary phoneme table (Smart Mode display) ────────────────────────────

struct PhonemeEntry { mokya_keycode_t keycode; const char* phoneme; };

// clang-format off
static const PhonemeEntry kPhonemeTable[] = {
    { MOKYA_KEY_1, "ㄅ" }, { MOKYA_KEY_3, "ˇ"  }, { MOKYA_KEY_5, "ㄓ" }, { MOKYA_KEY_7, "˙"  }, { MOKYA_KEY_9, "ㄞ" },
    { MOKYA_KEY_Q, "ㄆ" }, { MOKYA_KEY_E, "ㄍ" }, { MOKYA_KEY_T, "ㄔ" }, { MOKYA_KEY_U, "ㄧ" }, { MOKYA_KEY_O, "ㄟ" },
    { MOKYA_KEY_A, "ㄇ" }, { MOKYA_KEY_D, "ㄎ" }, { MOKYA_KEY_G, "ㄕ" }, { MOKYA_KEY_J, "ㄨ" }, { MOKYA_KEY_L, "ㄠ" },
    { MOKYA_KEY_Z, "ㄈ" }, { MOKYA_KEY_C, "ㄏ" }, { MOKYA_KEY_B, "ㄖ" }, { MOKYA_KEY_M, "ㄩ" }, { MOKYA_KEY_BACKSLASH, "ㄡ" },
    { MOKYA_KEY_NONE, nullptr },
};
// clang-format on

const char* ImeLogic::key_to_phoneme(mokya_keycode_t kc) {
    for (const PhonemeEntry* e = kPhonemeTable; e->phoneme; ++e)
        if (e->keycode == kc) return e->phoneme;
    return nullptr;
}

// ── Direct Mode label lookups ─────────────────────────────────────────────

const char* ImeLogic::key_to_direct_label(mokya_keycode_t kc, int idx) {
    if (idx < 0 || idx >= 5) return nullptr;
    const DirectEntry* e = find_direct_entry(kc);
    if (!e) return nullptr;
    return e->labels[idx];
}

int ImeLogic::direct_label_count(mokya_keycode_t kc) {
    const DirectEntry* e = find_direct_entry(kc);
    if (!e) return 0;
    int n = 0;
    while (n < 5 && e->labels[n]) ++n;
    return n;
}

// Number of cycling slots for the current direct mode:
//   DirectBopomofo → phoneme slots (indices 0-2).
//   DirectUpper/DirectLower → letter/digit slots (indices 3-4).
int ImeLogic::direct_mode_slot_count(mokya_keycode_t kc) const {
    const DirectEntry* e = find_direct_entry(kc);
    if (!e) return 0;
    if (mode_ == InputMode::DirectBopomofo) {
        int n = 0;
        while (n < 3 && e->labels[n]) ++n;
        return n;
    } else {
        int n = 0;
        while (n < 2 && e->labels[3 + n]) ++n;
        return n;
    }
}

// Lowercase-conversion buffer (single-threaded use only).
static char s_lower_buf[8];

static const char* to_lower_label(const char* lbl) {
    if (!lbl) return nullptr;
    int i = 0;
    while (lbl[i] && i < 7) {
        char c = lbl[i];
        s_lower_buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        ++i;
    }
    s_lower_buf[i] = '\0';
    return s_lower_buf;
}

const char* ImeLogic::direct_mode_slot_label(mokya_keycode_t kc, int idx) const {
    const DirectEntry* e = find_direct_entry(kc);
    if (!e || idx < 0) return nullptr;
    int actual = (mode_ == InputMode::DirectBopomofo) ? idx : (3 + idx);
    if (actual < 0 || actual >= 5 || !e->labels[actual]) return nullptr;
    if (mode_ == InputMode::DirectLower)
        return to_lower_label(e->labels[actual]);
    return e->labels[actual];
}

// ── Symbol tables ─────────────────────────────────────────────────────────

// clang-format off
static const char* const kSymZH3[] = { "，","、","；","：","「","」","（","）","【","】", nullptr };
static const char* const kSymEN3[] = { ",", ";", ":", "(", ")", "[", "]", "\"","'", nullptr };
static const char* const kSymZH4[] = { "。","？","！","…","—","～", nullptr };
static const char* const kSymEN4[] = { ".", "?", "!",  "-","_", "~", nullptr };
static const char* const kSymDir3[] = { "，","、","；","：","「","」","（","）","【","】",
                                        ",", ";", ":", "\"","'", "(",")", "[","]", nullptr };
static const char* const kSymDir4[] = { "。","？","！","…","—","～",
                                        ".", "?", "!",  "-","_","~", nullptr };
// clang-format on

static int str_arr_len(const char* const* arr) {
    int n = 0; while (arr[n]) ++n; return n;
}

const char* ImeLogic::sym_label(mokya_keycode_t kc, int idx) const {
    const char* const* arr = nullptr;
    if (is_direct_mode(mode_)) {
        arr = (kc == MOKYA_KEY_SYM1) ? kSymDir3 : kSymDir4;
    } else {
        if (kc == MOKYA_KEY_SYM1) arr = (context_lang_ == ZH) ? kSymZH3 : kSymEN3;
        else                      arr = (context_lang_ == ZH) ? kSymZH4 : kSymEN4;
    }
    if (!arr || idx < 0) return nullptr;
    int n = 0;
    while (arr[n] && n <= idx) {
        if (n == idx) return arr[idx];
        ++n;
    }
    return nullptr;
}

int ImeLogic::sym_label_count(mokya_keycode_t kc) const {
    const char* const* arr = nullptr;
    if (is_direct_mode(mode_)) {
        arr = (kc == MOKYA_KEY_SYM1) ? kSymDir3 : kSymDir4;
    } else {
        if (kc == MOKYA_KEY_SYM1) arr = (context_lang_ == ZH) ? kSymZH3 : kSymEN3;
        else                      arr = (context_lang_ == ZH) ? kSymZH4 : kSymEN4;
    }
    return arr ? str_arr_len(arr) : 0;
}

} // namespace mie
