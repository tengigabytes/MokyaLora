// SPDX-License-Identifier: MIT
// test_helpers.h — Shared helpers for mie::ImeLogic v2 unit tests.
//
// v2 API exercises a listener interface + process_key(ev) with a
// monotonic timestamp + tick(now_ms) for multi-tap / long-press.

#pragma once

#include <gtest/gtest.h>
#include <mie/ime_logic.h>
#include <mie/keycode.h>
#include <mie/trie_searcher.h>

#include <cstring>
#include <string>
#include <tuple>
#include <vector>

// ── Mock listener ────────────────────────────────────────────────────────────

struct MockListener : public mie::IImeListener {
    std::string             committed;
    std::vector<mie::NavDir> cursor_moves;
    int                     delete_before_count      = 0;
    int                     composition_changed_count = 0;

    void on_commit(const char* utf8) override         { committed += utf8; }
    void on_cursor_move(mie::NavDir d) override       { cursor_moves.push_back(d); }
    void on_delete_before() override                  { ++delete_before_count; }
    void on_composition_changed() override            { ++composition_changed_count; }

    void reset() {
        committed.clear();
        cursor_moves.clear();
        delete_before_count = 0;
        composition_changed_count = 0;
    }
};

// ── Key event constructors ──────────────────────────────────────────────────

static inline mie::KeyEvent kev(mokya_keycode_t kc, bool pressed = true, uint32_t now_ms = 0) {
    mie::KeyEvent e;
    e.keycode = kc;
    e.pressed = pressed;
    e.now_ms  = now_ms;
    return e;
}

// "Press" helper: send a single key-down with the given timestamp.
// SYM1 uses both edges (short vs long press); its dedicated tests
// supply explicit press / release events.
static inline bool press(mie::ImeLogic& ime, mokya_keycode_t kc, uint32_t now_ms = 0) {
    return ime.process_key(kev(kc, true, now_ms));
}

// Physical matrix (row, col) → keycode using the canonical row*6+col+1 map.
// Useful when readability wants "(0,0) = ㄅㄉ" over MOKYA_KEY_1.
static inline mokya_keycode_t kc_rc(uint8_t row, uint8_t col) {
    return (mokya_keycode_t)(row * 6 + col + 1);
}

// Convenience accessors for PendingView snapshot.
static inline const char* pending_str(const mie::ImeLogic& ime) {
    return ime.pending_view().str;
}
static inline int pending_bytes(const mie::ImeLogic& ime) {
    return ime.pending_view().byte_len;
}
static inline mie::PendingStyle pending_style(const mie::ImeLogic& ime) {
    return ime.pending_view().style;
}
static inline int matched_prefix_bytes(const mie::ImeLogic& ime) {
    return ime.pending_view().matched_prefix_bytes;
}

// ── Binary buffer builders (unchanged from v1) ──────────────────────────────

struct TEntry {
    const char* key;
    size_t      klen;
    const char* word;
    uint16_t    freq;
    uint8_t     tone = 1;
};

static inline void push_u8 (std::vector<uint8_t>& v, uint8_t  x) { v.push_back(x); }
static inline void push_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8));
}
static inline void push_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x>>8));
    v.push_back((uint8_t)(x>>16)); v.push_back((uint8_t)(x>>24));
}
static inline void push_raw(std::vector<uint8_t>& v, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) v.push_back((uint8_t)s[i]);
}
static inline void push_str(std::vector<uint8_t>& v, const char* s) {
    while (*s) v.push_back((uint8_t)*s++);
}

// Build v2-format MIED buffers with one word per key entry (1:1).
// v2 per-word layout: freq:u16, tone:u8, word_len:u8, word_utf8
static inline void build_single(const std::vector<TEntry>& entries,
                                 std::vector<uint8_t>& dat,
                                 std::vector<uint8_t>& val) {
    std::vector<uint32_t> val_off, key_off;
    std::vector<uint8_t>  keys_sec;

    for (const auto& e : entries) {
        val_off.push_back((uint32_t)val.size());
        push_u16(val, 1);
        size_t wlen = strlen(e.word);
        push_u16(val, e.freq);
        push_u8 (val, e.tone);
        push_u8 (val, (uint8_t)wlen);
        push_str(val, e.word);
    }
    for (const auto& e : entries) {
        key_off.push_back((uint32_t)keys_sec.size());
        push_u8(keys_sec, (uint8_t)e.klen);
        push_raw(keys_sec, e.key, e.klen);
    }

    uint32_t kc  = (uint32_t)entries.size();
    uint32_t kdo = 16 + kc * 8;

    push_str(dat, "MIED");
    push_u16(dat, 2); push_u16(dat, 0);
    push_u32(dat, kc); push_u32(dat, kdo);
    for (size_t i = 0; i < entries.size(); ++i) {
        push_u32(dat, key_off[i]);
        push_u32(dat, val_off[i]);
    }
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}

// Build v1-format MIED with two words sharing key 0x21. Used by candidate-nav tests.
static inline void build_two_zh(std::vector<uint8_t>& dat, std::vector<uint8_t>& val) {
    val.clear();
    push_u16(val, 2);
    push_u16(val, 500); push_u8(val, 3); push_str(val, "\xe5\xb7\xb4");  // 巴
    push_u16(val, 400); push_u8(val, 3); push_str(val, "\xe6\x8a\x8a");  // 把

    dat.clear();
    std::vector<uint8_t> keys_sec;
    push_u8(keys_sec, 1); push_u8(keys_sec, 0x21);

    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;
    push_str(dat, "MIED");
    push_u16(dat, 1); push_u16(dat, 0);
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, 0); push_u32(dat, 0);
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}

// Build v2-format MIED with ONE key and MANY words; used for tone-sort tests.
static inline void build_multi(
    const char* key, size_t klen,
    const std::vector<std::tuple<const char*, uint16_t, uint8_t>>& words,
    std::vector<uint8_t>& dat, std::vector<uint8_t>& val) {

    uint32_t v_off = (uint32_t)val.size();
    push_u16(val, (uint16_t)words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        const char* w    = std::get<0>(words[i]);
        uint16_t    freq = std::get<1>(words[i]);
        uint8_t     tone = std::get<2>(words[i]);
        size_t wlen = strlen(w);
        push_u16(val, freq);
        push_u8 (val, tone);
        push_u8 (val, (uint8_t)wlen);
        push_str(val, w);
    }

    std::vector<uint8_t> keys_sec;
    push_u8(keys_sec, (uint8_t)klen);
    push_raw(keys_sec, key, klen);

    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;
    push_str(dat, "MIED");
    push_u16(dat, 2); push_u16(dat, 0);
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, 0);
    push_u32(dat, v_off);
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}
