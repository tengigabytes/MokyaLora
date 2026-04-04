// test_ime_modes.cpp — ImeLogic tests: ModeSwitch, ModeSeparation
// SPDX-License-Identifier: MIT

#include "test_helpers.h"

namespace {

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

} // namespace
