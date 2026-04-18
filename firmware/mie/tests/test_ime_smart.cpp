// SPDX-License-Identifier: MIT
// test_ime_smart.cpp — SmartZh (Bopomofo) and SmartEn (T9 + digit cycling).

#include "test_helpers.h"

namespace {

using mie::ImeLogic;
using mie::InputMode;
using mie::PendingStyle;
using mie::TrieSearcher;

// ══════════════════════════════════════════════════════════════════════════
// SmartZh — compound display, key accumulation, dict search, commit
// ══════════════════════════════════════════════════════════════════════════

TEST(SmartZh, SingleKeyShowsCompound) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);    // ㄅ/ㄉ key → compound "ㄅㄉ"
    EXPECT_STREQ(pending_str(ime), "ㄅㄉ");
    EXPECT_EQ(pending_style(ime), PendingStyle::PrefixBold);
}

TEST(SmartZh, ThreePhonemeKey) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_9);    // ㄞ/ㄢ/ㄦ
    EXPECT_STREQ(pending_str(ime), "ㄞㄢㄦ");
}

TEST(SmartZh, ToneKeyCompound) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_3);    // ˇ/ˋ
    EXPECT_STREQ(pending_str(ime), "ˇˋ");
}

TEST(SmartZh, MultipleKeysSeparatedByComma) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);    // ㄅㄉ
    press(ime, MOKYA_KEY_Q);    // ㄆㄊ
    EXPECT_STREQ(pending_str(ime), "ㄅㄉ, ㄆㄊ");
}

TEST(SmartZh, FourKeysCompound) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);    // ㄅㄉ
    press(ime, MOKYA_KEY_Q);    // ㄆㄊ
    press(ime, MOKYA_KEY_E);    // ㄍㄐ
    press(ime, MOKYA_KEY_C);    // ㄏㄒ
    EXPECT_STREQ(pending_str(ime), "ㄅㄉ, ㄆㄊ, ㄍㄐ, ㄏㄒ");
}

TEST(SmartZh, DELRemovesLastKey) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_Q);
    press(ime, MOKYA_KEY_DEL);
    EXPECT_STREQ(pending_str(ime), "ㄅㄉ");
}

TEST(SmartZh, SpaceIdleEmitsHalfWidth) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_SPACE);
    EXPECT_EQ(L.committed, " ");
}

TEST(SmartZh, SpacePendingAppendsFirstTone) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_SPACE);    // first-tone marker ˉ
    EXPECT_TRUE(L.committed.empty());
    EXPECT_NE(std::string(pending_str(ime)).find("ˉ"), std::string::npos);
}

TEST(SmartZh, SpaceDoubleTapNoOp) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_SPACE);
    int b1 = pending_bytes(ime);
    press(ime, MOKYA_KEY_SPACE);
    EXPECT_EQ(pending_bytes(ime), b1);  // second SPACE is no-op
}

TEST(SmartZh, DictSearchFindsCandidate) {
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 500 } }, dat, val);  // 巴
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);

    press(ime, MOKYA_KEY_1);
    ASSERT_EQ(ime.candidate_count(), 1);
    EXPECT_STREQ(ime.candidate(0).word, "\xe5\xb7\xb4");  // 巴
}

TEST(SmartZh, OKCommitsSelectedCandidate) {
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 500 } }, dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);

    press(ime, MOKYA_KEY_1);
    ASSERT_GT(ime.candidate_count(), 0);
    press(ime, MOKYA_KEY_OK);
    EXPECT_EQ(L.committed, "\xe5\xb7\xb4");  // 巴
    EXPECT_FALSE(ime.has_pending());
}

TEST(SmartZh, PartialCommitKeepsUnmatchedTail) {
    // Two keys pressed; dict matches only the first. OK commits that match
    // and leaves the second key in pending.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 500 } }, dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);

    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_Q);   // not in dict; greedy falls back to len=1 match
    ASSERT_GT(ime.candidate_count(), 0);
    press(ime, MOKYA_KEY_OK);
    EXPECT_EQ(L.committed, "\xe5\xb7\xb4");
    // Second key (ㄆㄊ) should remain in pending.
    EXPECT_TRUE(ime.has_pending());
    EXPECT_STREQ(pending_str(ime), "ㄆㄊ");
}

TEST(SmartZh, PartialCommitStripsTrailingToneByte) {
    // Ensure the tone-byte-leak fix: after partial commit, leading tone
    // marker (0x20) or tone-key byte (0x22) is stripped from the remainder.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 500 } }, dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);

    press(ime, MOKYA_KEY_1);      // 0x21
    press(ime, MOKYA_KEY_SPACE);  // 0x20 first-tone marker
    press(ime, MOKYA_KEY_OK);
    EXPECT_EQ(L.committed, "\xe5\xb7\xb4");
    // Leading 0x20 should have been stripped.
    EXPECT_FALSE(ime.has_pending());
}

TEST(SmartZh, MatchedPrefixBytesSplitsDisplay) {
    // Match only the first key; matched_prefix_bytes points at end of first group.
    std::vector<uint8_t> dat, val;
    build_single({ { "\x21", 1, "\xe5\xb7\xb4", 500 } }, dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);

    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_Q);
    int m = matched_prefix_bytes(ime);
    // "ㄅㄉ" is 6 UTF-8 bytes.
    EXPECT_EQ(m, 6);
    EXPECT_EQ(pending_style(ime), PendingStyle::PrefixBold);
}

TEST(SmartZh, MultiSyllableTone1MarkerIsTransparent) {
    // Regression: user types ㄎ ㄞ SPACE ㄕ ˇ (intended: 開始).
    // Raw key_seq = \x2C\x25\x20\x2D\x22 (5 bytes).
    // gen_dict.py skips the tone-1 marker when encoding dict keys, so
    // 開始 is stored at \x2C\x25\x2D\x22 (4 bytes). run_search must
    // strip the 0x20 bytes from the search key so the multi-syllable
    // match still succeeds while preserving the tone intent from the
    // surrounding original key_seq_.
    //
    // Simulates a mini dict: 開 (tone 1) at \x2C\x25, 開始 (tone 3 —
    // gen_dict uses the last-syllable tone for phrases) at
    // \x2C\x25\x2D\x22.
    std::vector<uint8_t> dat, val;
    {
        // Two keys: \x2C\x25 -> 開(freq 500, tone 1)
        //           \x2C\x25\x2D\x22 -> 開始(freq 600, tone 3)
        // val layout:
        uint32_t v_off_kai = (uint32_t)val.size();
        push_u16(val, 1);
        push_u16(val, 500); push_u8(val, 1); push_u8(val, 3);
        push_str(val, "\xe9\x96\x8b");                       // 開
        uint32_t v_off_kaishi = (uint32_t)val.size();
        push_u16(val, 1);
        push_u16(val, 600); push_u8(val, 3); push_u8(val, 6);
        push_str(val, "\xe9\x96\x8b\xe5\xa7\x8b");           // 開始

        std::vector<uint8_t> keys_sec;
        uint32_t k_off_kai    = (uint32_t)keys_sec.size();
        push_u8(keys_sec, 2);  push_raw(keys_sec, "\x2C\x25", 2);
        uint32_t k_off_kaishi = (uint32_t)keys_sec.size();
        push_u8(keys_sec, 4);  push_raw(keys_sec, "\x2C\x25\x2D\x22", 4);

        uint32_t kc  = 2;
        uint32_t kdo = 16 + kc * 8;
        push_str(dat, "MIED");
        push_u16(dat, 2); push_u16(dat, 0);
        push_u32(dat, kc); push_u32(dat, kdo);
        push_u32(dat, k_off_kai);    push_u32(dat, v_off_kai);
        push_u32(dat, k_off_kaishi); push_u32(dat, v_off_kaishi);
        dat.insert(dat.end(), keys_sec.begin(), keys_sec.end());
    }
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);

    press(ime, MOKYA_KEY_D);        // ㄎ  -> 0x2C
    press(ime, MOKYA_KEY_9);        // ㄞ  -> 0x25
    press(ime, MOKYA_KEY_SPACE);    // first-tone marker 0x20
    press(ime, MOKYA_KEY_G);        // ㄕ  -> 0x2D
    press(ime, MOKYA_KEY_3);        // ˇ   -> 0x22

    ASSERT_GT(ime.candidate_count(), 0);
    // Trailing ˇ → intent 34. 開始's tone=3 passes the filter; 開's
    // tone=1 does not. So 開始 must be the top candidate.
    EXPECT_STREQ(ime.candidate(0).word, "\xe9\x96\x8b\xe5\xa7\x8b");  // 開始
}

TEST(SmartZh, ToneTierSortPromotesMatchingTone) {
    // Key 0x21 has two candidates at tone 1 and tone 3. With tone-3 intent
    // (0x22 suffix), the tone-3 candidate should move to position 0.
    std::vector<uint8_t> dat, val;
    build_multi("\x21\x22", 2, {
        std::make_tuple("\xe5\xb7\xb4", 500, (uint8_t)1),   // 巴 tone 1
        std::make_tuple("\xe6\x8a\x8a", 300, (uint8_t)3),   // 把 tone 3
    }, dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);

    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_3);  // ˇ/ˋ tone key (0x22)
    ASSERT_GT(ime.candidate_count(), 0);
    // Tone-3/4 intent filter: tone-3 candidate should be first.
    EXPECT_STREQ(ime.candidate(0).word, "\xe6\x8a\x8a");  // 把
}

// ══════════════════════════════════════════════════════════════════════════
// SmartEn — T9 letters + digit multi-tap
// ══════════════════════════════════════════════════════════════════════════

TEST(SmartEn, ModeIndicator) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_MODE);
    EXPECT_STREQ(ime.mode_indicator(), "EN");
}

TEST(SmartEn, LetterKeyBuildsPending) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_Q);    // primary letter 'q'
    press(ime, MOKYA_KEY_E);    // primary letter 'e'
    EXPECT_STREQ(pending_str(ime), "qe");
}

TEST(SmartEn, DigitKeyCyclesMultiTap) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_1, 100);  // digit "1"
    EXPECT_STREQ(pending_str(ime), "1");
    EXPECT_EQ(pending_style(ime), PendingStyle::Inverted);
    press(ime, MOKYA_KEY_1, 200);  // cycle → "2"
    EXPECT_STREQ(pending_str(ime), "2");
}

TEST(SmartEn, SpacePendingCommitsAndSpaces) {
    // SmartEn with no dict: no candidates; pending is discarded on SPACE.
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_Q);
    press(ime, MOKYA_KEY_SPACE);
    EXPECT_EQ(L.committed, " ");       // just the auto-space
    EXPECT_FALSE(ime.has_pending());
}

TEST(SmartEn, DigitAfterLetterCommitsLetterFirst) {
    // Ensure the digit multi-tap boundary: pressing a digit while letter
    // pending flushes the letter context first (discard on no candidate).
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_Q);
    press(ime, MOKYA_KEY_1, 50);
    EXPECT_STREQ(pending_str(ime), "1");
}

TEST(SmartZh, PrefixScanDedupsAbbreviationEntries) {
    // Regression: gen_dict.py stores a Chinese phrase at many per-syllable
    // prefix combinations, so the same word appears under several dict
    // keys (e.g. 心理學 at \x31\x30\x31\x33 AND \x31\x30\x31\x33\x33
    // AND \x31\x30\x31\x33\x33\x23). The prefix scan must collapse these
    // duplicates — the user should see 心理學 exactly once.
    std::vector<uint8_t> dat, val;
    {
        // Value records (one per key): each stores 心理學 with freq 1000.
        uint32_t v1 = (uint32_t)val.size();
        push_u16(val, 1);
        push_u16(val, 1000); push_u8(val, 5); push_u8(val, 9);
        push_str(val, "\xe5\xbf\x83\xe7\x90\x86\xe5\xad\xb8");  // 心理學
        uint32_t v2 = (uint32_t)val.size();
        push_u16(val, 1);
        push_u16(val, 1000); push_u8(val, 5); push_u8(val, 9);
        push_str(val, "\xe5\xbf\x83\xe7\x90\x86\xe5\xad\xb8");
        uint32_t v3 = (uint32_t)val.size();
        push_u16(val, 1);
        push_u16(val, 1000); push_u8(val, 5); push_u8(val, 9);
        push_str(val, "\xe5\xbf\x83\xe7\x90\x86\xe5\xad\xb8");

        // Keys (lex-sorted): length 4 → length 5 → length 6.
        std::vector<uint8_t> ks;
        uint32_t k1 = (uint32_t)ks.size();
        push_u8(ks, 4); push_raw(ks, "\x31\x30\x31\x33", 4);
        uint32_t k2 = (uint32_t)ks.size();
        push_u8(ks, 5); push_raw(ks, "\x31\x30\x31\x33\x33", 5);
        uint32_t k3 = (uint32_t)ks.size();
        push_u8(ks, 6); push_raw(ks, "\x31\x30\x31\x33\x33\x23", 6);

        uint32_t kc  = 3;
        uint32_t kdo = 16 + kc * 8;
        push_str(dat, "MIED");
        push_u16(dat, 2); push_u16(dat, 0);
        push_u32(dat, kc); push_u32(dat, kdo);
        push_u32(dat, k1); push_u32(dat, v1);
        push_u32(dat, k2); push_u32(dat, v2);
        push_u32(dat, k3); push_u32(dat, v3);
        dat.insert(dat.end(), ks.begin(), ks.end());
    }
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);

    press(ime, MOKYA_KEY_C);     // ㄏㄒ byte 0x31
    press(ime, MOKYA_KEY_Z);     // ㄈㄌ byte 0x30
    press(ime, MOKYA_KEY_C);     // ㄏㄒ byte 0x31
    press(ime, MOKYA_KEY_M);     // ㄩㄝ byte 0x33

    // The dict has 心理學 at three key lengths (4, 5, 6) starting with
    // the user's 4-byte input. Prefix scan finds all three; de-dup
    // collapses them back to one candidate.
    ASSERT_EQ(ime.candidate_count(), 1);
    EXPECT_STREQ(ime.candidate(0).word, "\xe5\xbf\x83\xe7\x90\x86\xe5\xad\xb8");
}

TEST(SmartEn, PrefixScanFindsLongerWords) {
    // Regression: EN dict stores only full-word keys (no abbreviation
    // entries). TrieSearcher::search must scan forward and include
    // candidates from keys that START WITH the query, so partial input
    // can surface longer words.
    //
    // Dict: "application" stored at its full T9 key (11 bytes). User
    // types only the first 3 bytes — the word should still be reachable.
    std::vector<uint8_t> dat, val;
    // application T9 key: a-p-p-l-i-c-a-t-i-o-n
    //   a → key A (slot 10) = 0x2B
    //   p → key O (slot 9)  = 0x2A
    //   l → key L (slot 14) = 0x2F
    //   i → key U (slot 8)  = 0x29
    //   c → key C (slot 16) = 0x31
    //   t → key T (slot 7)  = 0x28
    //   n → key B (slot 17) = 0x32
    build_single({ { "\x2B\x2A\x2A\x2F\x29\x31\x2B\x28\x29\x2A\x32", 11,
                     "application", 7420, 0 } }, dat, val);
    TrieSearcher en;
    ASSERT_TRUE(en.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    TrieSearcher zh;
    ImeLogic ime(zh, &en);

    press(ime, MOKYA_KEY_MODE);   // SmartZh → SmartEn
    press(ime, MOKYA_KEY_A);      // "a"
    press(ime, MOKYA_KEY_O);      // "p" (primary letter on key O)
    press(ime, MOKYA_KEY_O);      // "p"
    ASSERT_GT(ime.candidate_count(), 0);
    EXPECT_STREQ(ime.candidate(0).word, "application");
}

TEST(SmartEn, TABAdvancesPage) {
    // Use a letter key (Q = slot 5 → byte 0x26) for dict lookup; row-0
    // digit keys don't hit the dict in SmartEn.
    std::vector<std::tuple<const char*, uint16_t, uint8_t>> words;
    const char* w[6] = { "a", "b", "c", "d", "e", "f" };
    for (int i = 0; i < 6; ++i) words.emplace_back(w[i], (uint16_t)(100 - i), (uint8_t)0);

    std::vector<uint8_t> dat, val;
    build_multi("\x26", 1, words, dat, val);
    TrieSearcher en;
    ASSERT_TRUE(en.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    TrieSearcher zh;
    ImeLogic ime(zh, &en);
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_Q);
    ASSERT_EQ(ime.candidate_count(), 6);
    EXPECT_EQ(ime.page(), 0);
    press(ime, MOKYA_KEY_TAB);
    EXPECT_EQ(ime.page(), 1);
    press(ime, MOKYA_KEY_TAB);
    EXPECT_EQ(ime.page(), 0);  // wraps
}

} // namespace
