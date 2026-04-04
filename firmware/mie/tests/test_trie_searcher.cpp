// test_trie_searcher.cpp — Unit tests for mie::TrieSearcher
// SPDX-License-Identifier: MIT
//
// All tests use synthetic in-memory binary data so that no actual dictionary
// files are required.  The helper functions produce minimal but fully
// spec-compliant dict_dat.bin / dict_values.bin blobs.
//
// Key encoding (half-keyboard, matches gen_dict.py and ImeLogic):
//   key_index   = row × 5 + col   (rows 0-3, col 0-4 → 0-19)
//   key_seq_byte = key_index + 0x21 (→ ASCII '!' to '4')
//
// Keyboard positions used in this test:
//   (0,0) ㄅ/ㄉ  key_index=0  byte=0x21 '!'
//   (1,1) ㄍ/ㄐ  key_index=6  byte=0x27 '\''
//   (1,3) ㄧ/ㄛ  key_index=8  byte=0x29 ')'
//   (1,4) ㄟ/ㄣ  key_index=9  byte=0x2A '*'
//   (2,3) ㄨ/ㄜ  key_index=13 byte=0x2E '.'
//   (0,4) ㄞ/ㄢ  key_index=4  byte=0x25 '%'
//
// Test dictionary key sequences (sorted lexicographically by byte value):
//   "\x21"           → key (0,0)             → 蝙(100), 巴(80)
//   "\x27\x29\x2a"   → keys (1,1)(1,3)(1,4)  → 今(200), 金(150)  [ㄍ/ㄐ + ㄧ/ㄛ + ㄟ/ㄣ]
//   "\x27\x2e\x25"   → keys (1,1)(2,3)(0,4)  → 關(300)           [ㄍ/ㄐ + ㄨ/ㄜ + ㄞ/ㄢ]
// Sorted order: 0x21 < 0x27… ✓ ; 0x29 < 0x2E ✓

#include <gtest/gtest.h>
#include <mie/trie_searcher.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// ── Binary builder helpers ────────────────────────────────────────────────

static void push_u8 (std::vector<uint8_t>& v, uint8_t  x) { v.push_back(x); }
static void push_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
}
static void push_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >>  8));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 24));
}
static void push_bytes(std::vector<uint8_t>& v, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) v.push_back((uint8_t)s[i]);
}
static void push_str(std::vector<uint8_t>& v, const char* s) {
    while (*s) v.push_back((uint8_t)*s++);
}

struct WordEntry { const char* word; uint16_t freq; };

// DictEntry.key is a raw byte string (key_index + 0x21 encoding).
// key_len must be provided explicitly since keys may contain null bytes... but
// in practice our keys are ASCII printable (0x21-0x34) and null-free, so we
// can use strlen.  Still, store explicitly to be safe.
struct DictEntry { const char* key; size_t key_len; std::vector<WordEntry> words; };

/// Build spec-compliant dict_dat.bin / dict_values.bin from sorted DictEntry list.
static void build_buffers(const std::vector<DictEntry>& entries,
                          std::vector<uint8_t>&          dat_out,
                          std::vector<uint8_t>&          val_out) {
    // 1. dict_values.bin
    std::vector<uint32_t> val_offsets;
    for (const auto& e : entries) {
        val_offsets.push_back((uint32_t)val_out.size());
        push_u16(val_out, (uint16_t)e.words.size());
        for (const auto& w : e.words) {
            size_t wlen = strlen(w.word);
            push_u16(val_out, w.freq);
            push_u8 (val_out, (uint8_t)wlen);
            push_str(val_out, w.word);
        }
    }

    // 2. Keys-data section
    std::vector<uint8_t>  keys_sec;
    std::vector<uint32_t> key_offsets;
    for (const auto& e : entries) {
        key_offsets.push_back((uint32_t)keys_sec.size());
        push_u8(keys_sec, (uint8_t)e.key_len);
        push_bytes(keys_sec, e.key, e.key_len);
    }

    // 3. Offsets
    const uint32_t key_count     = (uint32_t)entries.size();
    const uint32_t keys_data_off = 16 + key_count * 8;

    // 4. Header
    push_str(dat_out, "MIED");
    push_u16(dat_out, 1);  // version
    push_u16(dat_out, 0);  // flags
    push_u32(dat_out, key_count);
    push_u32(dat_out, keys_data_off);

    // 5. Index table
    for (size_t i = 0; i < entries.size(); ++i) {
        push_u32(dat_out, key_offsets[i]);
        push_u32(dat_out, val_offsets[i]);
    }

    // 6. Keys-data
    dat_out.insert(dat_out.end(), keys_sec.begin(), keys_sec.end());
}

// ── Test dictionary (key_index encoding) ─────────────────────────────────

// Key sequences (see file header for derivation):
//   K0 = "\x21"           (0,0)
//   K1 = "\x27\x29\x2a"   (1,1)(1,3)(1,4)
//   K2 = "\x27\x2e\x25"   (1,1)(2,3)(0,4)
static const char K0[] = "\x21";
static const char K1[] = "\x27\x29\x2a";
static const char K2[] = "\x27\x2e\x25";

static const std::vector<DictEntry> kTestDict = {
    { K0, 1, { {"蝙", 100}, {"巴", 80} } },
    { K1, 3, { {"今", 200}, {"金", 150} } },
    { K2, 3, { {"關", 300}              } },
};

// ── Tests ─────────────────────────────────────────────────────────────────

TEST(MieStub, BuildEnvironmentWorks) {
    SUCCEED();
}

// ── Load / validation ─────────────────────────────────────────────────────

TEST(TrieSearcher, LoadNullBuffersFails) {
    mie::TrieSearcher ts;
    EXPECT_FALSE(ts.load_from_memory(nullptr, 0, nullptr, 0));
    EXPECT_FALSE(ts.is_loaded());
}

TEST(TrieSearcher, LoadTooShortHeaderFails) {
    const uint8_t tiny[] = { 'M', 'I', 'E' };
    const uint8_t val[]  = { 0 };
    mie::TrieSearcher ts;
    EXPECT_FALSE(ts.load_from_memory(tiny, sizeof(tiny), val, sizeof(val)));
}

TEST(TrieSearcher, LoadBadMagicFails) {
    std::vector<uint8_t> dat, val;
    build_buffers({}, dat, val);
    dat[0] = 'X';
    mie::TrieSearcher ts;
    EXPECT_FALSE(ts.load_from_memory(dat.data(), dat.size(),
                                     val.data(), val.size()));
}

TEST(TrieSearcher, LoadEmptyDictSucceeds) {
    std::vector<uint8_t> dat, val;
    build_buffers({}, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));
    EXPECT_TRUE(ts.is_loaded());
    EXPECT_EQ(ts.key_count(), 0u);
}

// ── Search correctness ────────────────────────────────────────────────────

TEST(TrieSearcher, SearchNotFoundReturnsZero) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[10];
    // Key for (3,1) = key_index 16 = 0x31 '1' — not in dict
    EXPECT_EQ(ts.search("\x31", out, 10), 0);
    EXPECT_EQ(ts.search("",    out, 10), 0);
    // Two presses of (0,0) — not in dict (only single (0,0) is present)
    EXPECT_EQ(ts.search("\x21\x21", out, 10), 0);
}

TEST(TrieSearcher, SearchSingleCandidateFound) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[10];
    // K2 = keys (1,1)(2,3)(0,4) → 關
    const int n = ts.search(K2, out, 10);
    ASSERT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "關");
    EXPECT_EQ(out[0].freq, 300);
}

TEST(TrieSearcher, SearchMultipleCandidatesFreqOrder) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[10];
    // K1 = keys (1,1)(1,3)(1,4) → 今(200), 金(150)
    const int n = ts.search(K1, out, 10);
    ASSERT_EQ(n, 2);
    EXPECT_STREQ(out[0].word, "今");
    EXPECT_EQ(out[0].freq, 200);
    EXPECT_STREQ(out[1].word, "金");
    EXPECT_EQ(out[1].freq, 150);
}

TEST(TrieSearcher, SearchFirstKey) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[10];
    // K0 = key (0,0) → 蝙(100), 巴(80)
    const int n = ts.search(K0, out, 10);
    ASSERT_EQ(n, 2);
    EXPECT_STREQ(out[0].word, "蝙");
    EXPECT_STREQ(out[1].word, "巴");
}

TEST(TrieSearcher, MaxResultsLimitsOutput) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[1];
    const int n = ts.search(K0, out, 1);  // dict has 2 but we ask for 1
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "蝙");
}

TEST(TrieSearcher, SearchNullKeyReturnsZero) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[10];
    EXPECT_EQ(ts.search(nullptr, out, 10), 0);
}

TEST(TrieSearcher, SearchWhenNotLoadedReturnsZero) {
    mie::TrieSearcher ts;
    mie::Candidate out[10];
    EXPECT_EQ(ts.search(K0, out, 10), 0);
}

TEST(TrieSearcher, KeyCountMatchesDictSize) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));
    EXPECT_EQ(ts.key_count(), (uint32_t)kTestDict.size());
}

} // namespace
