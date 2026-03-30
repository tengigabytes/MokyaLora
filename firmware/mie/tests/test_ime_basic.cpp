// test_ime_basic.cpp — Core ImeLogic tests: SmartMode, DirectMode,
//                      SymbolKeys, ContextLang, CandidateNav
// SPDX-License-Identifier: MIT

#include "test_helpers.h"

namespace {

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

} // namespace
