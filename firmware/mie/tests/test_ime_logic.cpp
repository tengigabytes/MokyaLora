// test_ime_logic.cpp — Unit tests for mie::ImeLogic
// SPDX-License-Identifier: MIT
//
// Tests cover Smart Mode and Direct Mode without requiring actual dictionary
// files.  A TrieSearcher with a tiny in-memory dictionary is used where needed;
// for tests that only check key-sequence building or Direct Mode cycling,
// an unloaded TrieSearcher suffices.
//
// Physical key references (row, col):
//   (0,0) ㄅ/ㄉ/1/2   key_index=0  seq_byte=0x21
//   (1,1) ㄍ/ㄐ/E/R    key_index=6  seq_byte=0x27
//   (2,3) ㄨ/ㄜ/J/K    key_index=13 seq_byte=0x2E
//   (4,0) MODE
//   (4,2) SPACE
//   (4,3) ，SYM
//   (4,4) 。.？
//   (2,5) BACK
//   (5,4) OK

#include <gtest/gtest.h>
#include <mie/ime_logic.h>
#include <mie/trie_searcher.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────

static mie::KeyEvent kev(uint8_t row, uint8_t col, bool pressed = true) {
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

// Build a small in-memory TrieSearcher.
// key_bytes / key_len: raw key-index-encoded key.
// tone: Bopomofo tone 1-5; 0=unspecified.  Defaults to 1 for ZH, 0 for EN.
struct TEntry {
    const char* key;
    size_t      klen;
    const char* word;
    uint16_t    freq;
    uint8_t     tone = 1;  // v2 tone field; default 1 (陰平)
};

static void push_u8 (std::vector<uint8_t>& v, uint8_t  x) { v.push_back(x); }
static void push_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8));
}
static void push_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x>>8));
    v.push_back((uint8_t)(x>>16)); v.push_back((uint8_t)(x>>24));
}
static void push_raw(std::vector<uint8_t>& v, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) v.push_back((uint8_t)s[i]);
}
static void push_str(std::vector<uint8_t>& v, const char* s) {
    while (*s) v.push_back((uint8_t)*s++);
}

// Build v2-format buffers for a list of single-word entries (one word per key).
// v2 per-word layout in values: freq:u16, tone:u8, word_len:u8, word_utf8
static void build_single(const std::vector<TEntry>& entries,
                         std::vector<uint8_t>& dat, std::vector<uint8_t>& val) {
    std::vector<uint32_t> val_off, key_off;
    std::vector<uint8_t> keys_sec;

    for (const auto& e : entries) {
        val_off.push_back((uint32_t)val.size());
        push_u16(val, 1);  // word_count = 1
        size_t wlen = strlen(e.word);
        push_u16(val, e.freq);
        push_u8 (val, e.tone);          // v2: tone byte
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
    push_u16(dat, 2); push_u16(dat, 0);  // version=2
    push_u32(dat, kc); push_u32(dat, kdo);
    for (size_t i = 0; i < entries.size(); ++i) {
        push_u32(dat, key_off[i]);
        push_u32(dat, val_off[i]);
    }
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}

// ══════════════════════════════════════════════════════════════════════════
// Smart Mode — basic state
// ══════════════════════════════════════════════════════════════════════════

TEST(SmartMode, InitialState) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    EXPECT_EQ(ime.mode(), mie::InputMode::SmartZh);
    EXPECT_EQ(ime.input_bytes(), 0);
    EXPECT_STREQ(ime.input_str(), "");
    EXPECT_EQ(ime.zh_candidate_count(), 0);
    EXPECT_EQ(ime.en_candidate_count(), 0);
}

TEST(SmartMode, KeyUpIgnored) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    EXPECT_FALSE(ime.process_key(kev(0, 0, false)));  // key up
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(SmartMode, InputKeyBuildsKeySeqAndDisplay) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    // Press (0,0) = ㄅ/ㄉ: key_index=0, display phoneme=ㄅ
    EXPECT_TRUE(ime.process_key(kev(0, 0)));
    EXPECT_GT(ime.input_bytes(), 0);
    // Display should show primary phoneme "ㄅ" (3 UTF-8 bytes)
    EXPECT_STREQ(ime.input_str(), "ㄅ");
}

TEST(SmartMode, MultipleKeysPhonemesAccumulate) {
    // With no auto-commit, phonemes always accumulate freely in the buffer.
    // The display shows ALL phonemes (matched prefix + remaining), e.g. "ㄅㄍㄨ".
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // ㄅ
    ime.process_key(kev(1, 1));  // ㄍ
    ime.process_key(kev(2, 3));  // ㄨ
    EXPECT_STREQ(ime.input_str(), "ㄅㄍㄨ");
}

TEST(SmartMode, BackspaceRemovesLastPhoneme) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // ㄅ
    ime.process_key(kev(1, 1));  // ㄍ
    EXPECT_STREQ(ime.input_str(), "ㄅㄍ");
    ime.process_key(kBACK);
    EXPECT_STREQ(ime.input_str(), "ㄅ");
}

TEST(SmartMode, BackspaceOnEmptyIsNoop) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    EXPECT_TRUE(ime.process_key(kBACK));  // should still return true (refresh)
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(SmartMode, ClearInputResetsAll) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kev(0, 0));
    ime.clear_input();
    EXPECT_EQ(ime.input_bytes(), 0);
    EXPECT_STREQ(ime.input_str(), "");
}

// ── Mode toggle ──────────────────────────────────────────────────────────

TEST(SmartMode, ModeKeyCycles) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    EXPECT_EQ(ime.mode(), mie::InputMode::SmartZh);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::SmartEn);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectUpper);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectLower);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectBopomofo);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::SmartZh);
}

TEST(SmartMode, ModeKeyClearsInput) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kev(0, 0));
    EXPECT_GT(ime.input_bytes(), 0);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.input_bytes(), 0);
}

// ── Commit callback ──────────────────────────────────────────────────────

TEST(SmartMode, CommitCallbackCalledOnOK) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // With no dictionary loaded, OK commits the raw display string
    ime.process_key(kev(0, 0));  // ㄅ
    ime.process_key(kOK);
    EXPECT_EQ(committed, "ㄅ");
    EXPECT_EQ(ime.input_bytes(), 0);  // cleared after commit
}

TEST(SmartMode, CommitCallbackCalledOnSpace) {
    // In SmartZh mode, SPACE with pending input appends the first-tone marker
    // (0x20) to the key sequence and does NOT commit any text.
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(1, 1));  // ㄍ
    ime.process_key(kSPACE);     // first tone — no commit in SmartZh
    EXPECT_EQ(committed, "");    // nothing committed
    EXPECT_GT(ime.input_bytes(), 0);  // input still pending
}

TEST(SmartMode, CommitFirstZhCandidate) {
    // Build a tiny dict so TrieSearcher returns a zh candidate.
    // Key: pressing (0,0) → key_index 0 → byte 0x21
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 500 } }, dat, val);

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // generates key_seq 0x21, search finds 巴
    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "巴");

    ime.process_key(kOK);
    EXPECT_EQ(committed, "巴");
}

// ══════════════════════════════════════════════════════════════════════════
// Direct Mode
// ══════════════════════════════════════════════════════════════════════════

TEST(DirectMode, InitialStateAfterModeSwitch) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectUpper);
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(DirectMode, FirstPressShouldShowFirstLabel) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper

    // (1,0) first letter slot = "Q"
    EXPECT_TRUE(ime.process_key(kev(1, 0)));
    EXPECT_STREQ(ime.input_str(), "Q");
}

TEST(DirectMode, SameKeyPressCyclesLabels) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper

    // (1,0) in DirectUpper: letter slots = Q, W (2 slots)
    ime.process_key(kev(1, 0));  // Q
    EXPECT_STREQ(ime.input_str(), "Q");
    ime.process_key(kev(1, 0));  // W
    EXPECT_STREQ(ime.input_str(), "W");
    ime.process_key(kev(1, 0));  // wraps → Q
    EXPECT_STREQ(ime.input_str(), "Q");
}

TEST(DirectMode, DirectBopomofoSameKeyPressCyclesPhonemes) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper
    ime.process_key(kMODE);  // DirectUpper → DirectLower
    ime.process_key(kMODE);  // DirectLower → DirectBopomofo
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectBopomofo);

    // (0,0) in DirectBopomofo: phoneme slots = ㄅ, ㄉ
    ime.process_key(kev(0, 0));  // ㄅ
    EXPECT_STREQ(ime.input_str(), "ㄅ");
    ime.process_key(kev(0, 0));  // ㄉ
    EXPECT_STREQ(ime.input_str(), "ㄉ");
    ime.process_key(kev(0, 0));  // wraps → ㄅ
    EXPECT_STREQ(ime.input_str(), "ㄅ");
}

TEST(DirectMode, DirectLowerProducesLowercaseLetters) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper
    ime.process_key(kMODE);  // DirectUpper → DirectLower
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectLower);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // (1,0) in DirectLower: letter slots = q, w
    ime.process_key(kev(1, 0));  // q
    EXPECT_STREQ(ime.input_str(), "q");
    ime.process_key(kev(1, 0));  // w
    EXPECT_STREQ(ime.input_str(), "w");
    ime.process_key(kOK);
    EXPECT_EQ(committed, "w");
}

TEST(DirectMode, OKConfirmsPendingChar) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // (1,0) in DirectUpper: Q → W
    ime.process_key(kev(1, 0));  // pending = Q
    ime.process_key(kev(1, 0));  // pending = W
    ime.process_key(kOK);
    EXPECT_EQ(committed, "W");
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(DirectMode, DifferentKeyAutoCommitsPrevious) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // (1,0) pending = Q; different key (1,1) → auto-commit Q, start E
    ime.process_key(kev(1, 0));  // pending = Q
    ime.process_key(kev(1, 1));  // different key → auto-commit Q, start E
    EXPECT_EQ(committed, "Q");
    EXPECT_STREQ(ime.input_str(), "E");
}

TEST(DirectMode, BackClearsPendingWithoutCommit) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(1, 0));  // pending Q
    ime.process_key(kBACK);
    EXPECT_EQ(committed, "");    // nothing committed
    EXPECT_EQ(ime.input_bytes(), 0);
}

// ══════════════════════════════════════════════════════════════════════════
// Symbol keys (row 4, col 3 = ，SYM ; col 4 = 。.？)
// ══════════════════════════════════════════════════════════════════════════

TEST(SymbolKeys, SmartModeZH_SYM3_FirstPress) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    // context_lang_ defaults to ZH; first symbol in ZH col-3 list = "，"
    EXPECT_TRUE(ime.process_key(kSYM3));
    EXPECT_STREQ(ime.input_str(), "，");
}

TEST(SymbolKeys, SmartModeZH_SYM3_Cycles) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kSYM3);  // ，
    ime.process_key(kSYM3);  // 、
    EXPECT_STREQ(ime.input_str(), "、");
}

TEST(SymbolKeys, SmartModeZH_SYM4_FirstPress) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kSYM4);
    EXPECT_STREQ(ime.input_str(), "。");
}

TEST(SymbolKeys, SYM3_OKCommitsSymbol) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kSYM3);  // pending "，"
    ime.process_key(kOK);
    EXPECT_EQ(committed, "，");
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(SymbolKeys, SwitchSymKeyAutoCommitsPrevious) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kSYM3);  // pending "，"
    ime.process_key(kSYM4);  // different sym key → commits "，", starts "。"
    EXPECT_EQ(committed, "，");
    EXPECT_STREQ(ime.input_str(), "。");
}

TEST(SymbolKeys, NonSymKeyCommitsPendingSymbol) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kSYM3);       // pending "，"
    ime.process_key(kev(0, 0));   // non-sym key → auto-commit "，", then add to input
    EXPECT_EQ(committed, "，");
    // After committing sym, the phoneme key is also processed → display "ㄅ"
    EXPECT_STREQ(ime.input_str(), "ㄅ");
}

TEST(SymbolKeys, DirectMode_SYM3_ShowsCombinedList) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper
    // In any Direct mode, sym key shows combined (zh+en) list; first = "，"
    ime.process_key(kSYM3);
    EXPECT_STREQ(ime.input_str(), "，");
}

// ══════════════════════════════════════════════════════════════════════════
// Context language update
// ══════════════════════════════════════════════════════════════════════════

TEST(ContextLang, CommitZhCandidateSetZH_SetsZhContext) {
    // After committing a zh candidate, context_lang_ = ZH.
    // Verify by checking that sym key (4,3) shows ZH punctuation "，"
    // (which it already does by default; this test just ensures it stays ZH).
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 500 } }, dat, val);

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(),
                                    val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // get zh candidate
    ASSERT_GT(ime.zh_candidate_count(), 0);
    ime.process_key(kOK);  // commit zh → context = ZH

    ime.process_key(kSYM3);
    EXPECT_STREQ(ime.input_str(), "，");  // ZH punctuation
}

// ══════════════════════════════════════════════════════════════════════════
// HF-1: Candidate navigation (UP/DOWN/LEFT/RIGHT)
// ══════════════════════════════════════════════════════════════════════════

// Build a dict with two entries for the same key so we can test multi-candidate nav.
static void build_two_zh(std::vector<uint8_t>& dat, std::vector<uint8_t>& val) {
    // Two words sharing key 0x21 (key_index=0, key (0,0)):
    //   word_count=2, [freq=500,word="巴"], [freq=400,word="把"]
    val.clear();
    push_u16(val, 2);              // word_count
    push_u16(val, 500); push_u8(val, 3); push_str(val, "巴");
    push_u16(val, 400); push_u8(val, 3); push_str(val, "把");

    dat.clear();
    // keys section: key_len=1, byte=0x21
    std::vector<uint8_t> keys_sec;
    push_u8(keys_sec, 1); push_u8(keys_sec, 0x21);

    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;
    push_str(dat, "MIED");
    push_u16(dat, 1); push_u16(dat, 0);
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, 0); push_u32(dat, 0);  // key_off=0, val_off=0
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}

TEST(CandidateNav, InitialGroupIsZH) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    EXPECT_EQ(ime.candidate_group(), 0);
    EXPECT_EQ(ime.candidate_index(), 0);
}

TEST(CandidateNav, DownMovesToNextCandidate) {
    std::vector<uint8_t> dat, val;
    build_two_zh(dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // trigger search → 巴, 把
    ASSERT_EQ(ime.zh_candidate_count(), 2);
    EXPECT_EQ(ime.candidate_index(), 0);  // initially on 巴

    ime.process_key(kev(5, 1));  // DOWN
    EXPECT_EQ(ime.candidate_index(), 1);  // now on 把
}

TEST(CandidateNav, DownWrapsAround) {
    std::vector<uint8_t> dat, val;
    build_two_zh(dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // 2 candidates
    ime.process_key(kev(5, 1));  // DOWN → index 1
    ime.process_key(kev(5, 1));  // DOWN wraps → index 0
    EXPECT_EQ(ime.candidate_index(), 0);
}

TEST(CandidateNav, UpWrapsAround) {
    std::vector<uint8_t> dat, val;
    build_two_zh(dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // 2 candidates, index=0
    ime.process_key(kev(5, 0));  // UP wraps → index 1
    EXPECT_EQ(ime.candidate_index(), 1);
}

TEST(CandidateNav, OKCommitsSelectedCandidate) {
    std::vector<uint8_t> dat, val;
    build_two_zh(dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // search → 巴(0), 把(1)
    ime.process_key(kev(5, 1));  // DOWN → index 1 (把)
    ime.process_key(kOK);
    EXPECT_EQ(committed, "把");
}

TEST(CandidateNav, RightMovesToNextMergedSlot) {
    // RIGHT navigates merged list forward; use a ZH dict with 2 candidates.
    // build_single with two words for the same key.
    std::vector<uint8_t> dat, val;
    {
        // Value record: 2 words for key 0x21
        push_u16(val, 2);
        push_u16(val, 2); push_u8(val, 3); push_str(val, "\xe5\xb7\xb4");  // 巴
        push_u16(val, 1); push_u8(val, 3); push_str(val, "\xe6\x8a\x8a");  // 把
        std::vector<uint8_t> ks;
        push_u8(ks, 1); push_u8(ks, 0x21);
        push_str(dat, "MIED"); push_u16(dat, 1); push_u16(dat, 0);
        push_u32(dat, 1); push_u32(dat, 16 + 8);
        push_u32(dat, 0); push_u32(dat, 0);
        dat.insert(dat.end(), ks.begin(), ks.end());
    }
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));
    ASSERT_GE(ime.zh_candidate_count(), 2);
    EXPECT_EQ(ime.candidate_index(), 0);   // starts at slot 0

    ime.process_key(kev(5, 3));  // RIGHT → slot 1
    EXPECT_EQ(ime.candidate_index(), 1);
}

TEST(CandidateNav, LeftMovesToPrevMergedSlot) {
    std::vector<uint8_t> dat, val;
    {
        push_u16(val, 2);
        push_u16(val, 2); push_u8(val, 3); push_str(val, "\xe5\xb7\xb4");  // 巴
        push_u16(val, 1); push_u8(val, 3); push_str(val, "\xe6\x8a\x8a");  // 把
        std::vector<uint8_t> ks;
        push_u8(ks, 1); push_u8(ks, 0x21);
        push_str(dat, "MIED"); push_u16(dat, 1); push_u16(dat, 0);
        push_u32(dat, 1); push_u32(dat, 16 + 8);
        push_u32(dat, 0); push_u32(dat, 0);
        dat.insert(dat.end(), ks.begin(), ks.end());
    }
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));
    ime.process_key(kev(5, 3));  // RIGHT → slot 1
    EXPECT_EQ(ime.candidate_index(), 1);

    ime.process_key(kev(5, 2));  // LEFT → slot 0
    EXPECT_EQ(ime.candidate_index(), 0);
}

TEST(CandidateNav, NavIgnoredWhenNoCandidates) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    // No dict, no candidates — navigation keys should return false
    ime.process_key(kev(0, 0));
    EXPECT_FALSE(ime.process_key(kev(5, 1)));  // DOWN
    EXPECT_FALSE(ime.process_key(kev(5, 0)));  // UP
    EXPECT_FALSE(ime.process_key(kev(5, 3)));  // RIGHT (no EN)
}

// ══════════════════════════════════════════════════════════════════════════
// HF-2: Greedy prefix search and partial commit
//
// Auto-commit is REMOVED.  Instead, run_search() greedily finds the longest
// prefix of key_seq_buf_ that has dictionary matches.  OK/SPACE commit
// only the matched prefix and leave the remaining keys for the next word.
// ══════════════════════════════════════════════════════════════════════════

TEST(GreedyPrefix, ExtraKeysStillMatchPrefix) {
    // Dict: key "\x21" → 巴.  No entry for "\x21\x27".
    // After typing (0,0)(1,1), greedy search finds "\x21" at len=1 even though
    // the total buffer is 2 bytes.  Candidates still show 巴.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 1 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // "\x21" → 巴
    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_EQ(ime.matched_prefix_len(), 1);

    // Type an extra key that doesn't extend the match.
    ime.process_key(kev(1, 1));  // buf="\x21\x27", greedy still finds "\x21"→巴
    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "巴");
    EXPECT_EQ(ime.matched_prefix_len(), 1);
    // No auto-commit happened.
    EXPECT_TRUE(committed.empty());
}

TEST(GreedyPrefix, NoAutoCommitWithEmptyDict) {
    // With an empty dict, keys always accumulate without any auto-commit.
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));
    ime.process_key(kev(1, 1));
    ime.process_key(kev(2, 3));
    EXPECT_TRUE(committed.empty());
    EXPECT_STREQ(ime.input_str(), "ㄅㄍㄨ");
}

TEST(GreedyPrefix, OKRemovesOnlyMatchedBytes) {
    // Dict: key "\x21" → 巴.  User types (0,0)(1,1) → buffer="\x21\x27",
    // matched_prefix_len=1.  Pressing OK commits 巴 and removes only the
    // first byte; the second byte "\x27" stays in the buffer.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 1 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // matched prefix "\x21"
    ime.process_key(kev(1, 1));  // extra key "\x27" in remaining
    ASSERT_EQ(ime.matched_prefix_len(), 1);

    ime.process_key(kOK);
    EXPECT_EQ(committed, "巴");
    // Remaining: "\x27" (ㄍ) still in buffer — shows as the new display.
    EXPECT_STREQ(ime.input_str(), "ㄍ");
    EXPECT_GT(ime.input_bytes(), 0);
}

TEST(GreedyPrefix, BackspaceReducesTail) {
    // Dict: key "\x21" → 巴.  After typing (0,0)(1,1), greedy matches "\x21".
    // Pressing BACK removes the last byte "\x27", leaving "\x21" — still matches 巴.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 1 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));
    ime.process_key(kev(1, 1));
    ASSERT_GT(ime.zh_candidate_count(), 0);
    ASSERT_EQ(ime.matched_prefix_len(), 1);
    EXPECT_STREQ(ime.input_str(), "ㄅㄍ");  // both keys visible

    ime.process_key(kBACK);
    EXPECT_STREQ(ime.input_str(), "ㄅ");
    EXPECT_GT(ime.zh_candidate_count(), 0);   // still matches 巴
    EXPECT_EQ(ime.matched_prefix_len(), 1);
}

// ══════════════════════════════════════════════════════════════════════════
// HF-3: MODE key commits before switching
// ══════════════════════════════════════════════════════════════════════════

TEST(ModeSwitch, SmartZhToSmartEnCommitsPendingInput) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));   // pending "ㄅ" in SmartZh mode (no dict = no candidates)
    EXPECT_GT(ime.input_bytes(), 0);

    ime.process_key(kMODE);       // SmartZh → SmartEn: should commit "ㄅ"
    EXPECT_EQ(committed, "ㄅ");   // text was preserved
    EXPECT_EQ(ime.mode(), mie::InputMode::SmartEn);
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(ModeSwitch, DirectUpperToDirectLowerCommitsPendingChar) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ime.process_key(kMODE);  // SmartEn → DirectUpper

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // (1,0) in DirectUpper: Q → W
    ime.process_key(kev(1, 0));  // pending "Q"
    ime.process_key(kev(1, 0));  // cycle to "W"
    EXPECT_STREQ(ime.input_str(), "W");

    ime.process_key(kMODE);      // DirectUpper → DirectLower: should commit "W"
    EXPECT_EQ(committed, "W");
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectLower);
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(ModeSwitch, DirectBopomofoToSmartZhCommitsPendingChar) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    // Reach DirectBopomofo (4 MODE presses from SmartZh)
    ime.process_key(kMODE);  // → SmartEn
    ime.process_key(kMODE);  // → DirectUpper
    ime.process_key(kMODE);  // → DirectLower
    ime.process_key(kMODE);  // → DirectBopomofo
    EXPECT_EQ(ime.mode(), mie::InputMode::DirectBopomofo);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // (0,0) in DirectBopomofo: ㄅ → ㄉ
    ime.process_key(kev(0, 0));  // pending "ㄅ"
    ime.process_key(kev(0, 0));  // cycle to "ㄉ"
    EXPECT_STREQ(ime.input_str(), "ㄉ");

    ime.process_key(kMODE);      // DirectBopomofo → SmartZh: should commit "ㄉ"
    EXPECT_EQ(committed, "ㄉ");
    EXPECT_EQ(ime.mode(), mie::InputMode::SmartZh);
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(ModeSwitch, SmartZhToSmartEnNoCommitWhenInputEmpty) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // No input → MODE should switch without committing anything
    ime.process_key(kMODE);
    EXPECT_EQ(committed, "");
    EXPECT_EQ(ime.mode(), mie::InputMode::SmartEn);
}

TEST(ModeSwitch, SmartZhToSmartEnCommitsBestZhCandidate) {
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 500 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));   // candidate 巴 in SmartZh
    ASSERT_GT(ime.zh_candidate_count(), 0);
    ime.process_key(kMODE);       // SmartZh → SmartEn: should commit 巴
    EXPECT_EQ(committed, "巴");
    EXPECT_EQ(ime.mode(), mie::InputMode::SmartEn);
}

TEST(ModeSwitch, ModeIndicatorString) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    EXPECT_STREQ(ime.mode_indicator(), "\xe4\xb8\xad");  // 中
    ime.process_key(kMODE);
    EXPECT_STREQ(ime.mode_indicator(), "EN");
    ime.process_key(kMODE);
    EXPECT_STREQ(ime.mode_indicator(), "ABC");
    ime.process_key(kMODE);
    EXPECT_STREQ(ime.mode_indicator(), "abc");
    ime.process_key(kMODE);
    EXPECT_STREQ(ime.mode_indicator(), "\xe3\x84\x85");  // ㄅ
    ime.process_key(kMODE);
    EXPECT_STREQ(ime.mode_indicator(), "\xe4\xb8\xad");  // 中 (wrapped)
}

// ══════════════════════════════════════════════════════════════════════════
// Mode separation — SmartZh sees only ZH; SmartEn sees only EN
// ══════════════════════════════════════════════════════════════════════════

TEST(ModeSeparation, SmartZhSeesOnlyZhCandidates) {
    // Both dicts loaded; SmartZh should return ZH candidates only.
    std::vector<uint8_t> zh_dat, zh_val, en_dat, en_val;
    build_single({ { "\x21", 1, "巴", 1 } }, zh_dat, zh_val);
    build_single({ { "\x21", 1, "abc", 1 } }, en_dat, en_val);

    mie::TrieSearcher zh_ts, en_ts;
    ASSERT_TRUE(zh_ts.load_from_memory(zh_dat.data(), zh_dat.size(), zh_val.data(), zh_val.size()));
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(), en_val.data(), en_val.size()));

    mie::ImeLogic ime(zh_ts, &en_ts);
    // Default mode is SmartZh
    ime.process_key(kev(0, 0));

    EXPECT_GT(ime.zh_candidate_count(), 0);
    EXPECT_EQ(ime.en_candidate_count(), 0);  // EN dict ignored in SmartZh
    EXPECT_STREQ(ime.merged_candidate(0).word, "\xe5\xb7\xb4");  // 巴
}

TEST(ModeSeparation, SmartEnSeesOnlyEnCandidates) {
    // Both dicts loaded; SmartEn should return EN candidates only.
    std::vector<uint8_t> zh_dat, zh_val, en_dat, en_val;
    build_single({ { "\x21", 1, "巴", 1 } }, zh_dat, zh_val);
    build_single({ { "\x21", 1, "abc", 1 } }, en_dat, en_val);

    mie::TrieSearcher zh_ts, en_ts;
    ASSERT_TRUE(zh_ts.load_from_memory(zh_dat.data(), zh_dat.size(), zh_val.data(), zh_val.size()));
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(), en_val.data(), en_val.size()));

    mie::ImeLogic ime(zh_ts, &en_ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ASSERT_EQ(ime.mode(), mie::InputMode::SmartEn);
    ime.process_key(kev(0, 0));

    EXPECT_EQ(ime.zh_candidate_count(), 0);  // ZH dict ignored in SmartEn
    EXPECT_GT(ime.en_candidate_count(), 0);
    EXPECT_STREQ(ime.merged_candidate(0).word, "abc");
}

TEST(ModeSeparation, SmartZhMergedIsZhOnly) {
    // SmartZh, only ZH dict loaded: merged view equals ZH candidates.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 1 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));

    EXPECT_EQ(ime.zh_candidate_count(), 1);
    EXPECT_EQ(ime.en_candidate_count(), 0);
    EXPECT_EQ(ime.merged_candidate_count(), 1);
    EXPECT_EQ(ime.merged_candidate_lang(0), 0);
    EXPECT_STREQ(ime.merged_candidate(0).word, "\xe5\xb7\xb4");  // 巴
}

TEST(ModeSeparation, SmartZhSpaceCommitsZhCandidate) {
    // SmartZh: SPACE no longer commits — it appends the first-tone marker.
    // Use OK to commit the candidate.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 1 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.zh_candidate_count(), 0);
    ime.process_key(kSPACE);     // first tone — no commit
    EXPECT_EQ(committed, "");
    // OK commits the candidate
    ime.process_key(kOK);
    EXPECT_EQ(committed, "\xe5\xb7\xb4");  // 巴
}

TEST(ModeSeparation, SmartEnSpaceCommitsEnCandidate) {
    std::vector<uint8_t> en_dat, en_val;
    build_single({ { "\x21", 1, "abc", 1 } }, en_dat, en_val);
    mie::TrieSearcher zh_ts;
    mie::TrieSearcher en_ts;
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(), en_val.data(), en_val.size()));
    mie::ImeLogic ime(zh_ts, &en_ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ASSERT_EQ(ime.mode(), mie::InputMode::SmartEn);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.en_candidate_count(), 0);
    ime.process_key(kSPACE);
    // SmartEn: word committed + auto-space appended
    EXPECT_EQ(committed, "abc ");
}

TEST(ModeSeparation, SmartEnDisplayShowsLetters) {
    // SmartEn mode: input_str() shows primary letter labels, not phonemes.
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // → SmartEn
    ASSERT_EQ(ime.mode(), mie::InputMode::SmartEn);

    ime.process_key(kev(1, 0));  // key (1,0): primary letter = "Q"
    EXPECT_STREQ(ime.input_str(), "Q");

    ime.process_key(kev(2, 0));  // key (2,0): primary letter = "A"
    EXPECT_STREQ(ime.input_str(), "QA");
}

// ══════════════════════════════════════════════════════════════════════════
// Abbreviated input (聲母猜字) — all-initials and prefix-initials variants
// ══════════════════════════════════════════════════════════════════════════
//
// These tests verify that dictionary entries generated for abbreviated key
// sequences (as produced by gen_dict.py's abbreviated_keyseqs()) are found
// correctly by the trie.
//
// Key reference (row, col) → key_byte:
//   (1,1)=ㄍ/ㄐ → 0x27  r/e
//   (1,0)=ㄆ/ㄊ → 0x26  q/w
//   (1,3)=ㄧ/ㄛ → 0x29  u/i
//   (1,4)=ㄟ/ㄣ → 0x2A  o/p
//   (0,4)=ㄞ/ㄢ → 0x25  9/0
//   (2,1)=ㄎ/ㄑ → 0x2C  d/f
//   (3,3)=ㄩ/ㄝ → 0x33  m
//   (0,1)=ˇ/ˋ  → 0x22  3/4
//   (1,2)=ㄔ/ㄗ → 0x28  t/y
//   (0,0)=ㄅ/ㄉ → 0x21  1/2
//   (3,0)=ㄈ/ㄌ → 0x30  z/x

// 今天 keys:
//   full (ㄐㄧㄣ ㄊㄧㄢ):      [0x27,0x29,0x2A,0x26,0x29,0x25]
//   all-initials (ㄐ+ㄊ):      [0x27,0x26]
//   prefix+full-last (ㄐ+ㄊㄧㄢ): [0x27,0x26,0x29,0x25]  ← user's `rwu0`

// 要去 keys:
//   full (ㄧㄠˋ ㄑㄩˋ):        [0x29,0x2F,0x22,0x2C,0x33,0x22]
//   all-initials (ㄧ+ㄑ):       [0x29,0x2C]
//   prefix+full-last (ㄧ+ㄑㄩˋ): [0x29,0x2C,0x33,0x22]  ← user's `ufm4`

// 臭豆腐 keys:
//   full (ㄔㄡˋ ㄉㄡˋ ㄈㄨˋ):        [0x28,0x34,0x22,0x21,0x34,0x22,0x30,0x2E,0x22]
//   all-initials (ㄔ+ㄉ+ㄈ):          [0x28,0x21,0x30]  ← user's `t2z`
//   prefix+full-last (ㄔ+ㄉ+ㄈㄨˋ):   [0x28,0x21,0x30,0x2E,0x22]

TEST(AbbreviatedInput, AllInitialsFindsMultiCharWord) {
    // Dict has 今天 indexed by all-initials key [0x27,0x26].
    // Typing r(1,1) w(1,0) should find 今天.
    std::vector<uint8_t> dat, val;
    // all-initials key for 今天: ㄐ→(1,1)=0x27, ㄊ→(1,0)=0x26
    build_single({ { "\x27\x26", 2, "\xe4\xbb\x8a\xe5\xa4\xa9", 1 } }, dat, val);  // 今天
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(1, 1));  // r → ㄍ/ㄐ
    ime.process_key(kev(1, 0));  // w → ㄆ/ㄊ

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe4\xbb\x8a\xe5\xa4\xa9");  // 今天
}

TEST(AbbreviatedInput, PrefixInitialsPlusFullLastFindsWord) {
    // Dict has 今天 indexed by prefix-initials key [0x27,0x26,0x29,0x25].
    // Typing r(1,1) w(1,0) u(1,3) 0(0,4) = `rwu0` should find 今天.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x27\x26\x29\x25", 4, "\xe4\xbb\x8a\xe5\xa4\xa9", 1 } }, dat, val);  // 今天
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(1, 1));  // r
    ime.process_key(kev(1, 0));  // w
    ime.process_key(kev(1, 3));  // u
    ime.process_key(kev(0, 4));  // 0

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe4\xbb\x8a\xe5\xa4\xa9");  // 今天
}

TEST(AbbreviatedInput, ThreeCharAllInitialsChoudoufu) {
    // Dict has 臭豆腐 indexed by all-initials [0x28,0x21,0x30] = t2z.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x28\x21\x30", 3,
                     "\xe8\x87\xad\xe8\xb1\x86\xe8\x85\x90", 1 } }, dat, val);  // 臭豆腐
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(1, 2));  // t → ㄔ/ㄗ
    ime.process_key(kev(0, 0));  // 2 → ㄅ/ㄉ
    ime.process_key(kev(3, 0));  // z → ㄈ/ㄌ

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe8\x87\xad\xe8\xb1\x86\xe8\x85\x90");  // 臭豆腐
}

TEST(AbbreviatedInput, SingleInitialFindsTopCandidates) {
    // Dict has both 巴 (full key [0x21,0x24]) and an initial-key entry [0x21] → 巴.
    // Typing just 1 key should show the initial-key candidates.
    std::vector<uint8_t> dat, val;
    // Build entry: initial key [0x21] → 巴 (as gen_dict.py would emit for 巴(ㄅㄚ))
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 1 } }, dat, val);  // 巴
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // 1 → ㄅ/ㄉ

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xb7\xb4");  // 巴
}

TEST(AbbreviatedInput, SpaceCommitsAbbreviatedCandidate) {
    // Full flow: type abbreviated keys for 要去.
    // SmartZh: SPACE no longer commits — it marks first tone.
    // Commit with OK instead.
    std::vector<uint8_t> dat, val;
    // prefix-initials key of 要去: [0x29,0x2C,0x33,0x22] = ufm4
    build_single({ { "\x29\x2C\x33\x22", 4,
                     "\xe8\xa6\x81\xe5\x8e\xbb", 1 } }, dat, val);  // 要去
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(1, 3));  // u → ㄧ/ㄛ
    ime.process_key(kev(2, 1));  // f → ㄎ/ㄑ
    ime.process_key(kev(3, 3));  // m → ㄩ/ㄝ
    ime.process_key(kev(0, 1));  // 4 → ˇ/ˋ
    ASSERT_GT(ime.zh_candidate_count(), 0);

    // SPACE adds first-tone marker, no commit
    ime.process_key(kSPACE);
    EXPECT_EQ(committed, "");

    // OK commits 要去
    ime.process_key(kOK);
    EXPECT_EQ(committed, "\xe8\xa6\x81\xe5\x8e\xbb");  // 要去
}

// ══════════════════════════════════════════════════════════════════════════
// SpaceTone — SPACE key appends first-tone marker in SmartZh
// ══════════════════════════════════════════════════════════════════════════

TEST(SpaceTone, SmartZhSpaceAddsFirstToneMarker) {
    // SPACE in SmartZh appends 0x20 (first-tone marker) to key_seq_buf_ and
    // does NOT commit.  compound_input_str() shows the "ˉ" glyph.
    // A second SPACE is a no-op (already trailing 0x20).
    // BACK removes the marker, restoring the previous compound display.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 1 } }, dat, val);  // 巴
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // Press key (0,0): builds key_seq [0x21], compound = "[ㄅㄉ]"
    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_EQ(committed, "");

    // SPACE appends first-tone marker: compound should now contain "ˉ" (U+02C9, 0xCB 0x89)
    ime.process_key(kSPACE);
    EXPECT_EQ(committed, "");
    EXPECT_NE(nullptr, strstr(ime.compound_input_str(), "\xcb\x89"));  // ˉ present

    // Second SPACE is a no-op: compound length should not increase
    int clen = ime.compound_input_bytes();
    ime.process_key(kSPACE);
    EXPECT_EQ(ime.compound_input_bytes(), clen);

    // BACK removes the first-tone marker byte from key_seq
    ime.process_key(kBACK);
    EXPECT_EQ(nullptr, strstr(ime.compound_input_str(), "\xcb\x89"));  // ˉ gone
}

// ══════════════════════════════════════════════════════════════════════════
// CompoundDisplay — "[ph0ph1]" format for SmartZh keys
// ══════════════════════════════════════════════════════════════════════════

TEST(CompoundDisplay, SingleKeyShowsBothPhonemes) {
    // Pressing key (0,0) in SmartZh: compound_input_str() returns "[ㄅㄉ]".
    // ㄅ = U+3105 = E3 84 85;  ㄉ = U+3109 = E3 84 89
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // ㄅ/ㄉ
    EXPECT_STREQ(ime.compound_input_str(), "[\xe3\x84\x85\xe3\x84\x89]");
}

// ══════════════════════════════════════════════════════════════════════════
// SingleCharPriority — single-codepoint words rank before multi-codepoint
// ══════════════════════════════════════════════════════════════════════════

TEST(SingleCharPriority, SingleCharsRankBeforeMultiChar) {
    // Dict entry for key 0x21 has two words:
    //   freq=900 "好吧" (2 chars, 6 UTF-8 bytes) — higher frequency, stored first
    //   freq=500 "好"   (1 char,  3 UTF-8 bytes) — lower frequency, stored second
    // After the single-char stable sort, "好" should appear as zh_candidate(0).
    std::vector<uint8_t> dat, val;

    // Build val: word_count=2, "好吧" first (higher freq), "好" second
    val.clear();
    push_u16(val, 2);
    push_u16(val, 900); push_u8(val, 6); push_str(val, "\xe5\xa5\xbd\xe5\x90\xa7");  // 好吧
    push_u16(val, 500); push_u8(val, 3); push_str(val, "\xe5\xa5\xbd");              // 好

    // Build dat header (same pattern as build_two_zh)
    dat.clear();
    std::vector<uint8_t> keys_sec;
    push_u8(keys_sec, 1); push_u8(keys_sec, 0x21);  // key_len=1, key=0x21
    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;
    push_str(dat, "MIED");
    push_u16(dat, 1); push_u16(dat, 0);
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, 0); push_u32(dat, 0);  // key_off=0, val_off=0
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // triggers search
    ASSERT_GE(ime.zh_candidate_count(), 2);
    // Single-char "好" must be first despite lower frequency
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xa5\xbd");   // 好
    EXPECT_STREQ(ime.zh_candidate(1).word, "\xe5\xa5\xbd\xe5\x90\xa7");  // 好吧
}

// ══════════════════════════════════════════════════════════════════════════
// SmartEn — auto-space and auto-capitalize
// ══════════════════════════════════════════════════════════════════════════

TEST(SmartEn, AutoSpaceAfterWordCommit) {
    // After committing a word in SmartEn (via OK), a trailing space is
    // automatically appended to the output.
    std::vector<uint8_t> en_dat, en_val;
    build_single({ { "\x21", 1, "cat", 1 } }, en_dat, en_val);
    mie::TrieSearcher zh_ts;
    mie::TrieSearcher en_ts;
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(), en_val.data(), en_val.size()));
    mie::ImeLogic ime(zh_ts, &en_ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ASSERT_EQ(ime.mode(), mie::InputMode::SmartEn);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // key_seq [0x21] → finds "cat"
    ASSERT_GT(ime.en_candidate_count(), 0);
    EXPECT_STREQ(ime.en_candidate(0).word, "cat");

    ime.process_key(kOK);  // commit "cat" + auto-space
    EXPECT_EQ(committed, "cat ");
}

TEST(SmartEn, AutoCapAfterSentencePunctuation) {
    // After committing sentence-ending punctuation ("."), the next EN word
    // committed in SmartEn mode is automatically capitalized.
    std::vector<uint8_t> en_dat, en_val;
    build_single({ { "\x21", 1, "cat", 1 } }, en_dat, en_val);
    mie::TrieSearcher zh_ts;
    mie::TrieSearcher en_ts;
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(), en_val.data(), en_val.size()));
    mie::ImeLogic ime(zh_ts, &en_ts);
    ime.process_key(kMODE);  // SmartZh → SmartEn
    ASSERT_EQ(ime.mode(), mie::InputMode::SmartEn);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // Commit "cat" to set context_lang_ = EN (required for EN sym list).
    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.en_candidate_count(), 0);
    ime.process_key(kSPACE);   // commits "cat " → context_lang_ = EN
    EXPECT_EQ(committed, "cat ");

    // SYM4 (row 4, col 4) with EN context: first symbol = "." (kSymEN4[0])
    ime.process_key(kSYM4);    // sym_pending_ = { col:4, sym_idx:0 } → "."

    // Any non-sym key triggers commit_sym_pending() first, firing did_commit(".").
    committed.clear();
    ime.process_key(kev(0, 0));   // commits pending ".", then adds key to buffer
    EXPECT_EQ(committed, ".");    // "." committed; en_capitalize_next_ is now true

    // Next SmartEn commit should be capitalized: "cat" → "Cat"
    ASSERT_GT(ime.en_candidate_count(), 0);
    committed.clear();
    ime.process_key(kSPACE);   // commits "Cat " (auto-cap + auto-space)
    EXPECT_EQ(committed, "Cat ");
}

// ══════════════════════════════════════════════════════════════════════════
// BackspaceTone1 — BACK after 0x20 marker must not erase the preceding phoneme
// ══════════════════════════════════════════════════════════════════════════

TEST(BackspaceTone1, BackspaceAfterToneMarkerRestoresDisplay) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    // Press (0,0) → ㄅ appears in input bar.
    ime.process_key(kev(0, 0));
    EXPECT_STREQ(ime.input_str(), "\xe3\x84\x85");  // ㄅ (U+3105)
    int bytes_before = ime.input_bytes();
    EXPECT_GT(bytes_before, 0);

    // SPACE appends 0x20 first-tone marker — display is unchanged.
    ime.process_key(kSPACE);
    EXPECT_EQ(ime.input_bytes(), bytes_before);

    // BACK removes 0x20 — input_str() must remain "ㄅ", not become empty.
    ime.process_key(kBACK);
    EXPECT_STREQ(ime.input_str(), "\xe3\x84\x85");
    EXPECT_EQ(ime.input_bytes(), bytes_before);

    // Second BACK removes the phoneme key — display is now empty.
    ime.process_key(kBACK);
    EXPECT_STREQ(ime.input_str(), "");
    EXPECT_EQ(ime.input_bytes(), 0);
}

// ══════════════════════════════════════════════════════════════════════════
// DirectBopomofo — phoneme selection triggers single-char ZH candidate search
// ══════════════════════════════════════════════════════════════════════════

TEST(DirectBopomofo, SearchTriggeredOnPhonemeSelect) {
    // dict: key 0x21 ("\x21") → single word "巴" (freq=1)
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 1 } }, dat, val);  // "巴"

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    // Cycle MODE ×4 to reach DirectBopomofo.
    ime.process_key(kMODE); ime.process_key(kMODE);
    ime.process_key(kMODE); ime.process_key(kMODE);
    ASSERT_EQ(ime.mode(), mie::InputMode::DirectBopomofo);

    // Press (0,0) = ㄅ key: display "ㄅ" and candidates should be populated.
    ime.process_key(kev(0, 0));
    EXPECT_STREQ(ime.input_str(), "\xe3\x84\x85");  // ㄅ
    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xb7\xb4");  // "巴"

    // BACK clears candidates.
    ime.process_key(kBACK);
    EXPECT_EQ(ime.zh_candidate_count(), 0);
    EXPECT_EQ(ime.merged_candidate_count(), 0);
}

TEST(DirectBopomofo, SearchFiltersMultiCharWords) {
    // dict: key 0x21 → 2 words: "巴士" (freq=900, 6 bytes), "巴" (freq=500, 3 bytes)
    std::vector<uint8_t> dat, val;

    // Build val manually: word_count=2, then [freq, len, utf8...] for each word.
    uint32_t v_off = (uint32_t)val.size();
    push_u16(val, 2);                             // word_count = 2
    push_u16(val, 900); push_u8(val, 6);          // "巴士" freq=900, len=6
    push_str(val, "\xe5\xb7\xb4\xe5\xa3\xab");   // 巴士
    push_u16(val, 500); push_u8(val, 3);          // "巴"  freq=500, len=3
    push_str(val, "\xe5\xb7\xb4");                // 巴

    // Build dat: header + 1 key entry + key section.
    std::vector<uint8_t> keys_sec;
    uint32_t k_off = 0;
    push_u8(keys_sec, 1);      // key length = 1 byte
    push_u8(keys_sec, 0x21);   // key byte = 0x21

    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;  // key-directory offset
    push_str(dat, "MIED");
    push_u16(dat, 1); push_u16(dat, 0);   // version
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, k_off);    // key_sec offset for entry 0
    push_u32(dat, v_off);    // val offset for entry 0
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    ime.process_key(kMODE); ime.process_key(kMODE);
    ime.process_key(kMODE); ime.process_key(kMODE);
    ASSERT_EQ(ime.mode(), mie::InputMode::DirectBopomofo);

    ime.process_key(kev(0, 0));
    // Only single-char "巴" should appear; multi-char "巴士" must be filtered out.
    EXPECT_EQ(ime.zh_candidate_count(), 1);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xb7\xb4");  // "巴"
}

// ══════════════════════════════════════════════════════════════════════════
// Tone-aware sorting
// ══════════════════════════════════════════════════════════════════════════

// Helper: build a v2 dict with ONE key → multiple words.
// words is [(word_utf8, freq, tone), ...]
static void build_multi(const char* key, size_t klen,
                        const std::vector<std::tuple<const char*,uint16_t,uint8_t>>& words,
                        std::vector<uint8_t>& dat, std::vector<uint8_t>& val) {
    uint32_t v_off = (uint32_t)val.size();
    push_u16(val, (uint16_t)words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        const char*    w    = std::get<0>(words[i]);
        uint16_t       freq = std::get<1>(words[i]);
        uint8_t        tone = std::get<2>(words[i]);
        size_t wlen = strlen(w);
        push_u16(val, freq);
        push_u8 (val, tone);
        push_u8 (val, (uint8_t)wlen);
        push_str(val, w);
    }

    std::vector<uint8_t> keys_sec;
    uint32_t k_off = 0;
    push_u8(keys_sec, (uint8_t)klen);
    push_raw(keys_sec, key, klen);

    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;
    push_str(dat, "MIED");
    push_u16(dat, 2); push_u16(dat, 0);   // version = 2
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, k_off);
    push_u32(dat, v_off);
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
}

// Tone 4 (ˋ) candidate beats higher-freq tone-1 candidate.
// Key [0x21, 0x24, 0x22]: 爸(tone=4,freq=100) vs 巴(tone=1,freq=200).
// Last key byte is 0x22 → tone intent = 34 (3 or 4).
// 爸 matches (tier 0, single+match); 巴 does not (tier 2, single+no-match).
TEST(ToneSort, ToneFourBeatsHigherFreqToneOne) {
    static const char kKey[] = "\x21\x24\x22";
    std::vector<uint8_t> dat, val;
    build_multi(kKey, 3,
        {
            {"\xe7\x88\xb8", 100, 4},  // 爸 tone=4 freq=100
            {"\xe5\xb7\xb4", 200, 1},  // 巴 tone=1 freq=200
        },
        dat, val);

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    // Press keys for [0x21, 0x24, 0x22]: (0,0), (0,3), (0,1)
    ime.process_key(kev(0, 0));
    ime.process_key(kev(0, 3));
    ime.process_key(kev(0, 1));

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe7\x88\xb8");  // 爸
}

// Tone-1 (SPACE) candidate beats higher-freq tone-3 candidate.
// Key [0x21, 0x25]: 版(tone=3,freq=500) vs 班(tone=1,freq=200).
// SPACE after matched prefix → tone intent = 1.
// 班 matches (tier 0); 版 does not (tier 2).
TEST(ToneSort, ToneOneBeatsHigherFreqToneThree) {
    static const char kKey[] = "\x21\x25";
    std::vector<uint8_t> dat, val;
    build_multi(kKey, 2,
        {
            {"\xe7\x89\x88", 500, 3},  // 版 tone=3 freq=500
            {"\xe7\x8f\xad", 200, 1},  // 班 tone=1 freq=200
        },
        dat, val);

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    // Press keys for [0x21, 0x25] then SPACE: (0,0), (0,4), SPACE
    ime.process_key(kev(0, 0));
    ime.process_key(kev(0, 4));
    ime.process_key(kSPACE);

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe7\x8f\xad");  // 班
}

// Two-syllable word: 寶寶 is stored under full key and appears as first candidate.
// Key [0x21, 0x2F, 0x22, 0x21, 0x2F, 0x24] → 寶寶 (6 UTF-8 bytes).
// Pressing those exact 6 keys triggers a greedy match at len=6; no tone filter
// (intent=0 for multi-key with last byte 0x24 which is not dedicated tone key).
TEST(ToneSort, BopomofoTwosyllableExact) {
    static const char kKey[] = "\x21\x2f\x22\x21\x2f\x24";
    std::vector<uint8_t> dat, val;
    build_multi(kKey, 6,
        {
            {"\xe5\xaf\xb6\xe5\xaf\xb6", 500, 0},  // 寶寶 tone=0 (multi-syll)
        },
        dat, val);

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    // Press keys for [0x21, 0x2F, 0x22, 0x21, 0x2F, 0x24]:
    // 0x21→(0,0)  0x2F→(2,4)  0x22→(0,1)  0x21→(0,0)  0x2F→(2,4)  0x24→(0,3)
    ime.process_key(kev(0, 0));
    ime.process_key(kev(2, 4));
    ime.process_key(kev(0, 1));
    ime.process_key(kev(0, 0));
    ime.process_key(kev(2, 4));
    ime.process_key(kev(0, 3));

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xaf\xb6\xe5\xaf\xb6");  // 寶寶
}

} // namespace
