// test_helpers.h — Shared helpers for mie::ImeLogic unit tests
// SPDX-License-Identifier: MIT
//
// Physical key references (row, col):
//   (0,0) ㄅ/ㄉ/1/2   key_index=0  seq_byte=0x21
//   (1,1) ㄍ/ㄐ/E/R    key_index=6  seq_byte=0x27
//   (2,3) ㄨ/ㄜ/J/K    key_index=13 seq_byte=0x2E
//   (4,0) MODE  (4,2) SPACE  (4,3) ，SYM  (4,4) 。.？
//   (2,5) BACK  (5,4) OK

#pragma once

#include <gtest/gtest.h>
#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>

#include <cstring>
#include <string>
#include <tuple>
#include <vector>

// ── Key event helpers ─────────────────────────────────────────────────────

static inline mie::KeyEvent kev(uint8_t row, uint8_t col, bool pressed = true) {
    mie::KeyEvent e;
    e.row = row; e.col = col; e.pressed = pressed;
    return e;
}

static const mie::KeyEvent kMODE  = kev(4, 0);
static const mie::KeyEvent kSPACE = kev(4, 2);
static const mie::KeyEvent kOK    = kev(5, 4);
static const mie::KeyEvent kBACK  = kev(2, 5);
static const mie::KeyEvent kSYM3  = kev(4, 3);  // ，SYM
static const mie::KeyEvent kSYM4  = kev(4, 4);  // 。.？

// ── Binary buffer builders ────────────────────────────────────────────────

// Entry descriptor for build_single: one word per key, v2 format.
struct TEntry {
    const char* key;
    size_t      klen;
    const char* word;
    uint16_t    freq;
    uint8_t     tone = 1;  // Bopomofo tone 1-5; 0=unknown/English
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

// Build v2-format MIED buffers: one word per key entry.
// v2 per-word layout: freq:u16, tone:u8, word_len:u8, word_utf8
static inline void build_single(const std::vector<TEntry>& entries,
                                 std::vector<uint8_t>& dat,
                                 std::vector<uint8_t>& val) {
    std::vector<uint32_t> val_off, key_off;
    std::vector<uint8_t>  keys_sec;

    for (const auto& e : entries) {
        val_off.push_back((uint32_t)val.size());
        push_u16(val, 1);                        // word_count = 1
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
    push_u16(dat, 2); push_u16(dat, 0);   // version = 2
    push_u32(dat, kc); push_u32(dat, kdo);
    for (size_t i = 0; i < entries.size(); ++i) {
        push_u32(dat, key_off[i]);
        push_u32(dat, val_off[i]);
    }
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}

// Build v1-format MIED buffers: two words for the same key 0x21.
// Used by CandidateNav tests to exercise multi-candidate navigation.
// v1 per-word layout: freq:u16, word_len:u8, word_utf8  (no tone byte)
static inline void build_two_zh(std::vector<uint8_t>& dat, std::vector<uint8_t>& val) {
    // Two words sharing key 0x21: 巴(freq=500) and 把(freq=400)
    val.clear();
    push_u16(val, 2);               // word_count
    push_u16(val, 500); push_u8(val, 3); push_str(val, "\xe5\xb7\xb4");  // 巴
    push_u16(val, 400); push_u8(val, 3); push_str(val, "\xe6\x8a\x8a");  // 把

    dat.clear();
    std::vector<uint8_t> keys_sec;
    push_u8(keys_sec, 1); push_u8(keys_sec, 0x21);

    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;
    push_str(dat, "MIED");
    push_u16(dat, 1); push_u16(dat, 0);   // version = 1
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, 0); push_u32(dat, 0);   // key_off=0, val_off=0
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}

// Build v2-format MIED buffers: ONE key → multiple words with explicit tones.
// Used by ToneSort tests.
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
    push_u16(dat, 2); push_u16(dat, 0);   // version = 2
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, 0);    // key_sec offset
    push_u32(dat, v_off);
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}
