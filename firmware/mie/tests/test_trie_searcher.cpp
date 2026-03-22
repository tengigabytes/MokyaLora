// test_trie_searcher.cpp — Unit tests for mie::TrieSearcher
// SPDX-License-Identifier: MIT
//
// All tests use synthetic in-memory binary data so that no actual dictionary
// files are required.  The helper functions below produce minimal but fully
// spec-compliant dict_dat.bin and dict_values.bin blobs.

#include <gtest/gtest.h>
#include <mie/trie_searcher.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// ── Binary builder helpers ────────────────────────────────────────────────

// Append a little-endian uint8 / uint16 / uint32 to a byte vector.
static void push_u8 (std::vector<uint8_t>& v, uint8_t  x) { v.push_back(x); }
static void push_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
static void push_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >>  8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}
static void push_str(std::vector<uint8_t>& v, const char* s) {
    while (*s) v.push_back(static_cast<uint8_t>(*s++));
}

struct WordEntry { const char* word; uint16_t freq; };
struct DictEntry { const char* key; std::vector<WordEntry> words; };

/// Build a minimal but spec-compliant dict_dat.bin and dict_values.bin pair
/// from an array of DictEntry (assumed already sorted lexicographically by key).
static void build_buffers(const std::vector<DictEntry>& entries,
                          std::vector<uint8_t>&          dat_out,
                          std::vector<uint8_t>&          val_out) {
    // 1. Build dict_values.bin and collect val_data_off per entry.
    std::vector<uint32_t> val_offsets;
    for (const auto& e : entries) {
        val_offsets.push_back(static_cast<uint32_t>(val_out.size()));
        push_u16(val_out, static_cast<uint16_t>(e.words.size()));
        for (const auto& w : e.words) {
            const size_t wlen = strlen(w.word);
            push_u16(val_out, w.freq);
            push_u8 (val_out, static_cast<uint8_t>(wlen));
            push_str(val_out, w.word);
        }
    }

    // 2. Build keys-data section and collect key_data_off per entry.
    std::vector<uint8_t>  keys_sec;
    std::vector<uint32_t> key_offsets;
    for (const auto& e : entries) {
        key_offsets.push_back(static_cast<uint32_t>(keys_sec.size()));
        const uint8_t klen = static_cast<uint8_t>(strlen(e.key));
        push_u8(keys_sec, klen);
        push_str(keys_sec, e.key);
    }

    // 3. Compute keys_data_off.
    const uint32_t key_count    = static_cast<uint32_t>(entries.size());
    const uint32_t header_size  = 16;
    const uint32_t index_size   = key_count * 8;
    const uint32_t keys_data_off = header_size + index_size;

    // 4. Header.
    push_str(dat_out, "MIED");              // magic
    push_u16(dat_out, 1);                   // version
    push_u16(dat_out, 0);                   // flags
    push_u32(dat_out, key_count);
    push_u32(dat_out, keys_data_off);

    // 5. Index table.
    for (size_t i = 0; i < entries.size(); ++i) {
        push_u32(dat_out, key_offsets[i]);
        push_u32(dat_out, val_offsets[i]);
    }

    // 6. Keys-data section.
    dat_out.insert(dat_out.end(), keys_sec.begin(), keys_sec.end());
}

// ── Tests ─────────────────────────────────────────────────────────────────

// Verify that the build environment (GTest + mie library) is still healthy.
TEST(MieStub, BuildEnvironmentWorks) {
    SUCCEED();
}

// ── TrieSearcher: invalid / empty input ──────────────────────────────────

TEST(TrieSearcher, LoadNullBuffersFails) {
    mie::TrieSearcher ts;
    EXPECT_FALSE(ts.load_from_memory(nullptr, 0, nullptr, 0));
    EXPECT_FALSE(ts.is_loaded());
}

TEST(TrieSearcher, LoadTooShortHeaderFails) {
    const uint8_t tiny[] = { 'M', 'I', 'E' };  // only 3 bytes
    const uint8_t val[]  = { 0 };
    mie::TrieSearcher ts;
    EXPECT_FALSE(ts.load_from_memory(tiny, sizeof(tiny), val, sizeof(val)));
}

TEST(TrieSearcher, LoadBadMagicFails) {
    std::vector<uint8_t> dat, val;
    build_buffers({}, dat, val);
    dat[0] = 'X';  // corrupt magic
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

// ── TrieSearcher: search correctness ────────────────────────────────────

// Three entries sorted by key (UTF-8 lex order):
//   "ㄅ"      → [("蝙",100), ("巴",80)]
//   "ㄍㄨㄢ"  → [("關",300)]
//   "ㄐㄧㄣ"  → [("今",200), ("金",150)]
//
// UTF-8 byte sequences:
//   ㄅ  = E3 84 85
//   ㄍ  = E3 84 8D,  ㄨ = E3 84 A8,  ㄢ = E3 84 A2
//   ㄐ  = E3 84 90,  ㄧ = E3 84 A7,  ㄣ = E3 84 A3
// Sorted lex: "ㄅ" (85) < "ㄍ..." (8D) < "ㄐ..." (90) ✓

static const std::vector<DictEntry> kTestDict = {
    { "ㄅ",      { {"蝙", 100}, {"巴", 80} } },
    { "ㄍㄨㄢ",  { {"關", 300}              } },
    { "ㄐㄧㄣ",  { {"今", 200}, {"金", 150} } },
};

TEST(TrieSearcher, SearchNotFoundReturnsZero) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[10];
    EXPECT_EQ(ts.search("ㄒ", out, 10), 0);
    EXPECT_EQ(ts.search("",   out, 10), 0);
    EXPECT_EQ(ts.search("ㄅㄅ", out, 10), 0);  // prefix but not an exact key
}

TEST(TrieSearcher, SearchSingleCandidateFound) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));

    mie::Candidate out[10];
    const int n = ts.search("ㄍㄨㄢ", out, 10);
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
    // "ㄐㄧㄣ" → 今(200), 金(150) — already stored in descending freq order.
    const int n = ts.search("ㄐㄧㄣ", out, 10);
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
    const int n = ts.search("ㄅ", out, 10);
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
    const int n = ts.search("ㄅ", out, 1);  // dict has 2 but we ask for 1
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
    mie::TrieSearcher ts;  // not loaded
    mie::Candidate out[10];
    EXPECT_EQ(ts.search("ㄅ", out, 10), 0);
}

TEST(TrieSearcher, KeyCountMatchesDictSize) {
    std::vector<uint8_t> dat, val;
    build_buffers(kTestDict, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));
    EXPECT_EQ(ts.key_count(), static_cast<uint32_t>(kTestDict.size()));
}

} // namespace
