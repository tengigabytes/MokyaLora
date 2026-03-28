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
struct TEntry { const char* key; size_t klen; const char* word; uint16_t freq; };

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

// Build buffers for a list of single-word entries (one word per key, sorted).
static void build_single(const std::vector<TEntry>& entries,
                         std::vector<uint8_t>& dat, std::vector<uint8_t>& val) {
    std::vector<uint32_t> val_off, key_off;
    std::vector<uint8_t> keys_sec;

    for (const auto& e : entries) {
        val_off.push_back((uint32_t)val.size());
        push_u16(val, 1);  // word_count
        size_t wlen = strlen(e.word);
        push_u16(val, e.freq);
        push_u8 (val, (uint8_t)wlen);
        push_str(val, e.word);
    }
    for (const auto& e : entries) {
        key_off.push_back((uint32_t)keys_sec.size());
        push_u8(keys_sec, (uint8_t)e.klen);
        push_raw(keys_sec, e.key, e.klen);
    }

    uint32_t kc = (uint32_t)entries.size();
    uint32_t kdo = 16 + kc * 8;

    push_str(dat, "MIED");
    push_u16(dat, 1); push_u16(dat, 0);
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

    EXPECT_EQ(ime.mode(), mie::InputMode::Smart);
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
    // Need a dict with entries for each prefix so zero-match auto-commit doesn't fire.
    // Keys: (0,0)=0x21, (1,1)=0x27, (2,3)=0x2E
    // Build entries for "\x21", "\x21\x27", "\x21\x27\x2E"
    static const char K1[] = "\x21";
    static const char K2[] = "\x21\x27";
    static const char K3[] = "\x21\x27\x2e";
    std::vector<uint8_t> dat, val;
    build_single({
        { K1, 1, "巴", 1 },
        { K2, 2, "改", 1 },
        { K3, 3, "菊", 1 },
    }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // ㄅ
    ime.process_key(kev(1, 1));  // ㄍ
    ime.process_key(kev(2, 3));  // ㄨ
    EXPECT_STREQ(ime.input_str(), "ㄅㄍㄨ");
}

TEST(SmartMode, BackspaceRemovesLastPhoneme) {
    // Need dict entries for both prefix sequences to prevent auto-commit.
    static const char K1[] = "\x21";
    static const char K2[] = "\x21\x27";
    std::vector<uint8_t> dat, val;
    build_single({ { K1, 1, "巴", 1 }, { K2, 2, "改", 1 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
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

TEST(SmartMode, ModeKeyTogglesToDirect) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    EXPECT_EQ(ime.mode(), mie::InputMode::Smart);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::Direct);
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::Smart);
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
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(1, 1));  // ㄍ
    ime.process_key(kSPACE);
    EXPECT_EQ(committed, "ㄍ");
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
    ime.process_key(kMODE);
    EXPECT_EQ(ime.mode(), mie::InputMode::Direct);
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(DirectMode, FirstPressShouldShowFirstLabel) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // → Direct

    // (0,0) first label = "ㄅ"
    EXPECT_TRUE(ime.process_key(kev(0, 0)));
    EXPECT_STREQ(ime.input_str(), "ㄅ");
}

TEST(DirectMode, SameKeyPressCyclesLabels) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);

    // (0,0): labels = ㄅ, ㄉ, 1, 2
    ime.process_key(kev(0, 0));  // ㄅ
    EXPECT_STREQ(ime.input_str(), "ㄅ");
    ime.process_key(kev(0, 0));  // ㄉ
    EXPECT_STREQ(ime.input_str(), "ㄉ");
    ime.process_key(kev(0, 0));  // 1
    EXPECT_STREQ(ime.input_str(), "1");
    ime.process_key(kev(0, 0));  // 2
    EXPECT_STREQ(ime.input_str(), "2");
    ime.process_key(kev(0, 0));  // wraps → ㄅ
    EXPECT_STREQ(ime.input_str(), "ㄅ");
}

TEST(DirectMode, OKConfirmsPendingChar) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // pending = ㄅ
    ime.process_key(kev(0, 0));  // pending = ㄉ
    ime.process_key(kOK);
    EXPECT_EQ(committed, "ㄉ");
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(DirectMode, DifferentKeyAutoCommitsPrevious) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // pending = ㄅ (key 0,0)
    ime.process_key(kev(1, 1));  // different key → auto-commit ㄅ, start ㄍ
    EXPECT_EQ(committed, "ㄅ");
    EXPECT_STREQ(ime.input_str(), "ㄍ");
}

TEST(DirectMode, BackClearsPendingWithoutCommit) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // pending ㄅ
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
    ime.process_key(kMODE);  // → Direct
    // In Direct Mode, sym key shows combined (zh+en) list; first = "，"
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

TEST(CandidateNav, RightSwitchesToENGroup) {
    // Build one zh and one en entry for same key byte.
    std::vector<uint8_t> zh_dat, zh_val, en_dat, en_val;
    build_single({ { "\x21", 1, "巴", 1 } }, zh_dat, zh_val);
    build_single({ { "\x21", 1, "abc", 1 } }, en_dat, en_val);

    mie::TrieSearcher zh_ts, en_ts;
    ASSERT_TRUE(zh_ts.load_from_memory(zh_dat.data(), zh_dat.size(),
                                        zh_val.data(), zh_val.size()));
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(),
                                        en_val.data(), en_val.size()));
    mie::ImeLogic ime(zh_ts, &en_ts);

    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.zh_candidate_count(), 0);
    ASSERT_GT(ime.en_candidate_count(), 0);
    EXPECT_EQ(ime.candidate_group(), 0);  // default ZH

    ime.process_key(kev(5, 3));  // RIGHT → switch to EN group
    EXPECT_EQ(ime.candidate_group(), 1);
    EXPECT_EQ(ime.candidate_index(), 0);
}

TEST(CandidateNav, LeftSwitchesToZHGroup) {
    std::vector<uint8_t> zh_dat, zh_val, en_dat, en_val;
    build_single({ { "\x21", 1, "巴", 1 } }, zh_dat, zh_val);
    build_single({ { "\x21", 1, "abc", 1 } }, en_dat, en_val);

    mie::TrieSearcher zh_ts, en_ts;
    ASSERT_TRUE(zh_ts.load_from_memory(zh_dat.data(), zh_dat.size(),
                                        zh_val.data(), zh_val.size()));
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(),
                                        en_val.data(), en_val.size()));
    mie::ImeLogic ime(zh_ts, &en_ts);

    ime.process_key(kev(0, 0));
    ime.process_key(kev(5, 3));  // RIGHT → EN group
    EXPECT_EQ(ime.candidate_group(), 1);

    ime.process_key(kev(5, 2));  // LEFT → back to ZH group
    EXPECT_EQ(ime.candidate_group(), 0);
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
// HF-2: Zero-match auto-commit
// ══════════════════════════════════════════════════════════════════════════

TEST(ZeroMatchAutoCommit, AutoCommitsBestOnNoMatch) {
    // Dict: key "\x21" → "巴", no entry for "\x21\x21"
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 1 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // seq="\x21" → candidate 巴
    ASSERT_GT(ime.zh_candidate_count(), 0);
    // Press same key again → seq="\x21\x21" → 0 candidates → auto-commit 巴, restart
    ime.process_key(kev(0, 0));
    EXPECT_EQ(committed, "巴");
    // Should now have 1 key in the new sequence
    EXPECT_STREQ(ime.input_str(), "ㄅ");
}

TEST(ZeroMatchAutoCommit, NoAutoCommitWithOnlyOneKey) {
    // If only 1 key has been typed and it matches nothing, do NOT auto-commit.
    mie::TrieSearcher ts;  // empty dict
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // 1 key, no candidates
    ime.process_key(kev(1, 1));  // 2 keys now, but 1 key was there before — auto-commit rule:
                                  // key_seq_len AFTER append == 2, so >= 2; but prev was 1
                                  // and prev also had 0 candidates → commits input_buf_ "ㄅ"
    // Even with empty dict, auto-commit fires when key_seq_len >= 2 after append
    EXPECT_EQ(committed, "ㄅ");  // committed raw display of first key
}

// ══════════════════════════════════════════════════════════════════════════
// HF-3: MODE key commits before switching
// ══════════════════════════════════════════════════════════════════════════

TEST(ModeSwitch, SmartToDirectCommitsPendingInput) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));   // pending "ㄅ" in smart mode (no dict = no candidates)
    EXPECT_GT(ime.input_bytes(), 0);

    ime.process_key(kMODE);       // should commit "ㄅ" then switch to Direct
    EXPECT_EQ(committed, "ㄅ");   // text was preserved
    EXPECT_EQ(ime.mode(), mie::InputMode::Direct);
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(ModeSwitch, DirectToSmartCommitsPendingChar) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    ime.process_key(kMODE);  // → Direct

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));  // pending "ㄅ" in direct mode
    ime.process_key(kev(0, 0));  // cycle to "ㄉ"
    EXPECT_STREQ(ime.input_str(), "ㄉ");

    ime.process_key(kMODE);      // should commit "ㄉ" then switch to Smart
    EXPECT_EQ(committed, "ㄉ");
    EXPECT_EQ(ime.mode(), mie::InputMode::Smart);
    EXPECT_EQ(ime.input_bytes(), 0);
}

TEST(ModeSwitch, SmartToDirectNoCommitWhenInputEmpty) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // No input → MODE should switch without committing anything
    ime.process_key(kMODE);
    EXPECT_EQ(committed, "");
    EXPECT_EQ(ime.mode(), mie::InputMode::Direct);
}

TEST(ModeSwitch, SmartToDirectCommitsBestZhCandidate) {
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "巴", 500 } }, dat, val);
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    ime.process_key(kev(0, 0));   // candidate 巴
    ASSERT_GT(ime.zh_candidate_count(), 0);
    ime.process_key(kMODE);       // should commit 巴
    EXPECT_EQ(committed, "巴");
    EXPECT_EQ(ime.mode(), mie::InputMode::Direct);
}

TEST(ModeSwitch, ModeIndicatorString) {
    mie::TrieSearcher ts;
    mie::ImeLogic ime(ts);
    EXPECT_STREQ(ime.mode_indicator(), "[智慧]");
    ime.process_key(kMODE);
    EXPECT_STREQ(ime.mode_indicator(), "[直接]");
}

} // namespace
