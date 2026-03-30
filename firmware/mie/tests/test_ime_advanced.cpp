// test_ime_advanced.cpp — ImeLogic tests: GreedyPrefix, AbbreviatedInput,
//   SpaceTone, CompoundDisplay, SingleCharPriority, SmartEn, BackspaceTone1,
//   DirectBopomofo, ToneSort
// SPDX-License-Identifier: MIT

#include "test_helpers.h"

namespace {

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
// Abbreviated input (聲母猜字) — all-initials and prefix-initials variants
// ══════════════════════════════════════════════════════════════════════════
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
//
// 今天: full [0x27,0x29,0x2A,0x26,0x29,0x25]; all-initials [0x27,0x26]
// 要去: prefix+full-last [0x29,0x2C,0x33,0x22]  ← `ufm4`
// 臭豆腐: all-initials [0x28,0x21,0x30]  ← `t2z`

TEST(AbbreviatedInput, AllInitialsFindsMultiCharWord) {
    // Dict has 今天 indexed by all-initials key [0x27,0x26].
    std::vector<uint8_t> dat, val;
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
    // Dict has initial-key entry [0x21] → 巴.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 1 } }, dat, val);  // 巴
    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));  // 1 → ㄅ/ㄉ

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xb7\xb4");  // 巴
}

TEST(AbbreviatedInput, SpaceCommitsAbbreviatedCandidate) {
    // SmartZh: SPACE marks first tone (no commit). Use OK to commit.
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

    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_EQ(committed, "");

    // SPACE appends first-tone marker: compound should contain "ˉ" (U+02C9, 0xCB 0x89)
    ime.process_key(kSPACE);
    EXPECT_EQ(committed, "");
    EXPECT_NE(nullptr, strstr(ime.compound_input_str(), "\xcb\x89"));  // ˉ present

    // Second SPACE is a no-op
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

    val.clear();
    push_u16(val, 2);
    push_u16(val, 900); push_u8(val, 6); push_str(val, "\xe5\xa5\xbd\xe5\x90\xa7");  // 好吧
    push_u16(val, 500); push_u8(val, 3); push_str(val, "\xe5\xa5\xbd");              // 好

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

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    mie::ImeLogic ime(ts);

    ime.process_key(kev(0, 0));
    ASSERT_GE(ime.zh_candidate_count(), 2);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xa5\xbd");              // 好
    EXPECT_STREQ(ime.zh_candidate(1).word, "\xe5\xa5\xbd\xe5\x90\xa7"); // 好吧
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

    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.en_candidate_count(), 0);
    EXPECT_STREQ(ime.en_candidate(0).word, "cat");

    ime.process_key(kOK);
    EXPECT_EQ(committed, "cat ");
}

TEST(SmartEn, AutoCapAfterSentencePunctuation) {
    // After committing ".", the next EN word is automatically capitalized.
    std::vector<uint8_t> en_dat, en_val;
    build_single({ { "\x21", 1, "cat", 1 } }, en_dat, en_val);
    mie::TrieSearcher zh_ts;
    mie::TrieSearcher en_ts;
    ASSERT_TRUE(en_ts.load_from_memory(en_dat.data(), en_dat.size(), en_val.data(), en_val.size()));
    mie::ImeLogic ime(zh_ts, &en_ts);
    ime.process_key(kMODE);
    ASSERT_EQ(ime.mode(), mie::InputMode::SmartEn);

    std::string committed;
    ime.set_commit_callback([](const char* s, void* ctx) {
        *static_cast<std::string*>(ctx) += s;
    }, &committed);

    // Commit "cat" to set context_lang_ = EN
    ime.process_key(kev(0, 0));
    ASSERT_GT(ime.en_candidate_count(), 0);
    ime.process_key(kSPACE);   // commits "cat " → context_lang_ = EN
    EXPECT_EQ(committed, "cat ");

    // SYM4 with EN context: first symbol = "."
    ime.process_key(kSYM4);

    committed.clear();
    ime.process_key(kev(0, 0));   // commits pending ".", then adds key to buffer
    EXPECT_EQ(committed, ".");    // en_capitalize_next_ is now true

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

    ime.process_key(kev(0, 0));
    EXPECT_STREQ(ime.input_str(), "\xe3\x84\x85");  // ㄅ (U+3105)
    int bytes_before = ime.input_bytes();
    EXPECT_GT(bytes_before, 0);

    // SPACE appends 0x20 first-tone marker — display byte count unchanged.
    ime.process_key(kSPACE);
    EXPECT_EQ(ime.input_bytes(), bytes_before);

    // BACK removes 0x20 — input_str() must remain "ㄅ".
    ime.process_key(kBACK);
    EXPECT_STREQ(ime.input_str(), "\xe3\x84\x85");
    EXPECT_EQ(ime.input_bytes(), bytes_before);

    // Second BACK removes the phoneme key.
    ime.process_key(kBACK);
    EXPECT_STREQ(ime.input_str(), "");
    EXPECT_EQ(ime.input_bytes(), 0);
}

// ══════════════════════════════════════════════════════════════════════════
// DirectBopomofo — phoneme selection triggers single-char ZH candidate search
// ══════════════════════════════════════════════════════════════════════════

TEST(DirectBopomofo, SearchTriggeredOnPhonemeSelect) {
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 1 } }, dat, val);  // 巴

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    // Cycle MODE ×4 to reach DirectBopomofo.
    ime.process_key(kMODE); ime.process_key(kMODE);
    ime.process_key(kMODE); ime.process_key(kMODE);
    ASSERT_EQ(ime.mode(), mie::InputMode::DirectBopomofo);

    ime.process_key(kev(0, 0));
    EXPECT_STREQ(ime.input_str(), "\xe3\x84\x85");  // ㄅ
    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xb7\xb4");  // 巴

    ime.process_key(kBACK);
    EXPECT_EQ(ime.zh_candidate_count(), 0);
    EXPECT_EQ(ime.merged_candidate_count(), 0);
}

TEST(DirectBopomofo, SearchFiltersMultiCharWords) {
    // dict: key 0x21 → 2 words: "巴士" (freq=900, 6 bytes), "巴" (freq=500, 3 bytes)
    std::vector<uint8_t> dat, val;

    uint32_t v_off = (uint32_t)val.size();
    push_u16(val, 2);
    push_u16(val, 900); push_u8(val, 6);
    push_str(val, "\xe5\xb7\xb4\xe5\xa3\xab");   // 巴士
    push_u16(val, 500); push_u8(val, 3);
    push_str(val, "\xe5\xb7\xb4");                // 巴

    std::vector<uint8_t> keys_sec;
    uint32_t k_off = 0;
    push_u8(keys_sec, 1);
    push_u8(keys_sec, 0x21);

    uint32_t kc  = 1;
    uint32_t kdo = 16 + kc * 8;
    push_str(dat, "MIED");
    push_u16(dat, 1); push_u16(dat, 0);
    push_u32(dat, kc); push_u32(dat, kdo);
    push_u32(dat, k_off);
    push_u32(dat, v_off);
    dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    ime.process_key(kMODE); ime.process_key(kMODE);
    ime.process_key(kMODE); ime.process_key(kMODE);
    ASSERT_EQ(ime.mode(), mie::InputMode::DirectBopomofo);

    ime.process_key(kev(0, 0));
    // Only single-char "巴" should appear; "巴士" must be filtered out.
    EXPECT_EQ(ime.zh_candidate_count(), 1);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xb7\xb4");  // 巴
}

// ══════════════════════════════════════════════════════════════════════════
// Tone-aware sorting
// ══════════════════════════════════════════════════════════════════════════

// Tone 4 (ˋ) candidate beats higher-freq tone-1 candidate.
// Key [0x21, 0x24, 0x22]: 爸(tone=4,freq=100) vs 巴(tone=1,freq=200).
// Last key byte is 0x22 → tone intent = 34 (3 or 4).
// 爸 matches (tier 0); 巴 does not (tier 2) → strict filter hides it.
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
    ime.process_key(kev(0, 0));
    ime.process_key(kev(0, 3));
    ime.process_key(kev(0, 1));

    EXPECT_EQ(ime.zh_candidate_count(), 1);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe7\x88\xb8");  // 爸
}

// Tone 1 (SPACE) candidate beats higher-freq tone-3 candidate.
// Key [0x21, 0x25] then SPACE: 班(tone=1,freq=200) vs 版(tone=3,freq=500).
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
    ime.process_key(kev(0, 0));
    ime.process_key(kev(0, 4));
    ime.process_key(kSPACE);

    EXPECT_EQ(ime.zh_candidate_count(), 1);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe7\x8f\xad");  // 班
}

// Two-syllable exact match: 寶寶 stored under 6-byte key.
// No tone filter applied (intent=0 for last byte 0x24 which is not a dedicated
// tone key); 寶寶 appears as first candidate.
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
    ime.process_key(kev(0, 0));
    ime.process_key(kev(2, 4));
    ime.process_key(kev(0, 1));
    ime.process_key(kev(0, 0));
    ime.process_key(kev(2, 4));
    ime.process_key(kev(0, 3));

    ASSERT_GT(ime.zh_candidate_count(), 0);
    EXPECT_STREQ(ime.zh_candidate(0).word, "\xe5\xaf\xb6\xe5\xaf\xb6");  // 寶寶
}

// Regression: committing a tone-1 word (SPACE → OK) must not leave a stray
// 0x20 at the front of key_seq_buf_ for the next word.
TEST(ToneSort, SpaceCommitDoesNotLeaveStrayToneMarker) {
    static const char kKey[] = "\x21";
    std::vector<uint8_t> dat, val;
    build_multi(kKey, 1,
        { {"\xe5\x8c\x85", 500, 1} },   // 包 tone=1
        dat, val);

    mie::TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));

    mie::ImeLogic ime(ts);
    ime.process_key(kev(0, 0));   // ㄅ → key_seq=[0x21]
    ime.process_key(kSPACE);       // 0x20 appended → key_seq=[0x21,0x20]
    ASSERT_GT(ime.zh_candidate_count(), 0);
    ime.process_key(kOK);          // commits 包; remainder=[0x20] → stripped

    EXPECT_EQ(ime.input_bytes(), 0);
    EXPECT_STREQ(ime.input_str(), "");
    EXPECT_EQ(ime.zh_candidate_count(), 0);

    ime.process_key(kev(0, 0));
    const std::string is = ime.input_str();
    ASSERT_GE(is.size(), 1u);
    EXPECT_NE((uint8_t)is[0], 0xCB) << "Stray first-tone marker ˉ found at start of input";
}

} // namespace
