// ime_logic.cpp — ImeLogic implementation
// SPDX-License-Identifier: MIT
//
// Three input modes (cycled by MODE key):
//   Bopomofo     — primary-phoneme mapping → Trie search → Traditional Chinese candidates
//   English      — half-keyboard letter-pair expansion → English prefix search  [stub]
//   Alphanumeric — multi-tap single character cycling (no dictionary)            [stub]
//
// Full disambiguation (spatial + phonetic correction) is deferred to Phase 3.

#include <mie/ime_logic.h>

#include <cstring>

namespace mie {

// ── Primary phoneme table ─────────────────────────────────────────────────
//
// Layout reference: docs/requirements/hardware-requirements.md §8.1
// and docs/design-notes/mie-architecture.md §7.1
//
// Each entry maps a physical (row, col) to its primary Bopomofo phoneme
// (the first symbol printed on the key).  UTF-8 literals are used directly
// so the source file must be compiled as UTF-8 (MSVC: /utf-8).
//
// Rows 4–5 contain only control / navigation keys → no phoneme entry.

struct PhonemeEntry {
    uint8_t     row;
    uint8_t     col;
    const char* phoneme;  // null-terminated UTF-8
};

// clang-format off
static const PhonemeEntry kPhonemeTable[] = {
    // Row 0 — ㄅ/ㄉ | ˇ/ˋ | ㄓ/ˊ | ˙/ㄚ | ㄞ/ㄢ/ㄦ
    { 0, 0, "ㄅ" },   // U+3105
    { 0, 1, "ˇ"  },   // U+02C7 (3rd tone)
    { 0, 2, "ㄓ" },   // U+3113
    { 0, 3, "˙"  },   // U+02D9 (neutral tone)
    { 0, 4, "ㄞ" },   // U+311E

    // Row 1 — ㄆ/ㄊ | ㄍ/ㄐ | ㄔ/ㄗ | ㄧ/ㄛ | ㄟ/ㄣ
    { 1, 0, "ㄆ" },   // U+3106
    { 1, 1, "ㄍ" },   // U+310D
    { 1, 2, "ㄔ" },   // U+3114
    { 1, 3, "ㄧ" },   // U+3127
    { 1, 4, "ㄟ" },   // U+311F

    // Row 2 — ㄇ/ㄋ | ㄎ/ㄑ | ㄕ/ㄘ | ㄨ/ㄜ | ㄠ/ㄤ
    { 2, 0, "ㄇ" },   // U+3107
    { 2, 1, "ㄎ" },   // U+310E
    { 2, 2, "ㄕ" },   // U+3115
    { 2, 3, "ㄨ" },   // U+3128
    { 2, 4, "ㄠ" },   // U+3120

    // Row 3 — ㄈ/ㄌ | ㄏ/ㄒ | ㄖ/ㄙ | ㄩ/ㄝ | ㄡ/ㄥ
    { 3, 0, "ㄈ" },   // U+3108
    { 3, 1, "ㄏ" },   // U+310F
    { 3, 2, "ㄖ" },   // U+3116
    { 3, 3, "ㄩ" },   // U+3129
    { 3, 4, "ㄡ" },   // U+3121

    // Sentinel
    { 0xFF, 0xFF, nullptr },
};
// clang-format on

const char* ImeLogic::key_to_phoneme(uint8_t row, uint8_t col) {
    for (const PhonemeEntry* e = kPhonemeTable; e->phoneme != nullptr; ++e) {
        if (e->row == row && e->col == col)
            return e->phoneme;
    }
    return nullptr;
}

// ── Constructor ───────────────────────────────────────────────────────────

ImeLogic::ImeLogic(TrieSearcher& searcher)
    : searcher_(searcher) {}

// ── Input buffer helpers ──────────────────────────────────────────────────

void ImeLogic::clear_input() {
    input_bytes_  = 0;
    input_buf_[0] = '\0';
    cand_count_   = 0;
    multi_tap_    = {};
}

void ImeLogic::append_phoneme(const char* utf8) {
    if (!utf8 || !*utf8) return;
    int len = static_cast<int>(strlen(utf8));
    if (input_bytes_ + len >= static_cast<int>(sizeof(input_buf_)) - 1)
        return;  // buffer full
    memcpy(input_buf_ + input_bytes_, utf8, static_cast<size_t>(len));
    input_bytes_ += len;
    input_buf_[input_bytes_] = '\0';
}

void ImeLogic::backspace_phoneme() {
    if (input_bytes_ == 0) return;

    // Walk backward from input_bytes_-1 to find the start of the last
    // UTF-8 code point.  UTF-8 continuation bytes have the pattern 10xxxxxx
    // (i.e., (byte & 0xC0) == 0x80).
    int pos = input_bytes_ - 1;
    while (pos > 0 && (static_cast<uint8_t>(input_buf_[pos]) & 0xC0) == 0x80)
        --pos;

    input_bytes_      = pos;
    input_buf_[pos]   = '\0';
}

void ImeLogic::run_search() {
    if (input_bytes_ == 0 || !searcher_.is_loaded()) {
        cand_count_ = 0;
        return;
    }
    cand_count_ = searcher_.search(input_buf_, candidates_, kMaxCandidates);
}

// ── Key event processing ──────────────────────────────────────────────────

bool ImeLogic::process_key(const KeyEvent& ev) {
    if (!ev.pressed) return false;  // ignore key-up events for now

    // MODE key (4,0): cycle through 3 modes and clear input.
    if (ev.row == 4 && ev.col == 0) {
        mode_ = static_cast<InputMode>(
            (static_cast<uint8_t>(mode_) + 1) % 3);
        clear_input();
        return true;
    }

    switch (mode_) {
        case InputMode::Bopomofo:     return process_bopomofo(ev);
        case InputMode::English:      return process_english(ev);
        case InputMode::Alphanumeric: return process_alpha(ev);
    }
    return false;
}

bool ImeLogic::process_bopomofo(const KeyEvent& ev) {
    // BACK (2,5) and DEL (3,5): remove last phoneme
    if ((ev.row == 2 && ev.col == 5) || (ev.row == 3 && ev.col == 5)) {
        backspace_phoneme();
        run_search();
        return true;
    }

    // SPACE (4,2) and OK (5,4): commit first candidate, clear input
    if ((ev.row == 4 && ev.col == 2) || (ev.row == 5 && ev.col == 4)) {
        // Committed word forwarded to output callback in Phase 2+.
        // For Phase 1 (REPL), we simply clear the buffer.
        clear_input();
        return true;
    }

    // Phoneme keys (rows 0–3, cols 0–4)
    const char* phoneme = key_to_phoneme(ev.row, ev.col);
    if (phoneme) {
        append_phoneme(phoneme);
        run_search();
        return true;
    }

    return false;  // unhandled key
}

bool ImeLogic::process_english(const KeyEvent& ev) {
    (void)ev;
    // Phase 1 extension: English word prediction via half-keyboard pair expansion.
    // Each ambiguous key carries two letters; the search layer expands all
    // combinations and queries an English-language MIED dictionary.
    // Not yet implemented.
    return false;
}

bool ImeLogic::process_alpha(const KeyEvent& ev) {
    (void)ev;
    // Phase 1 extension: multi-tap single character input.
    // Consecutive presses of the same key cycle through its two characters.
    // A different key press (or timeout) confirms the pending character.
    // Not yet implemented.
    return false;
}

} // namespace mie
