// SPDX-License-Identifier: MIT
// test_ime_direct.cpp — Direct mode multi-tap + SYM1 / SYM2 punctuation.

#include "test_helpers.h"

namespace {

using mie::ImeLogic;
using mie::InputMode;
using mie::PendingStyle;
using mie::TrieSearcher;

// Helpers to put the IME into Direct mode.
static void enter_direct(ImeLogic& ime) {
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_MODE);
}

// ══════════════════════════════════════════════════════════════════════════
// Direct — multi-tap cycling
// ══════════════════════════════════════════════════════════════════════════

TEST(Direct, ModeIndicator) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    EXPECT_EQ(ime.mode(), InputMode::Direct);
    EXPECT_STREQ(ime.mode_indicator(), "ABC");
}

TEST(Direct, FirstPressIsLowercase) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);  // A/S key
    EXPECT_STREQ(pending_str(ime), "a");
    EXPECT_EQ(pending_style(ime), PendingStyle::Inverted);
}

TEST(Direct, CycleFourSlotsAtoS) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100); EXPECT_STREQ(pending_str(ime), "a");
    press(ime, MOKYA_KEY_A, 200); EXPECT_STREQ(pending_str(ime), "s");
    press(ime, MOKYA_KEY_A, 300); EXPECT_STREQ(pending_str(ime), "A");
    press(ime, MOKYA_KEY_A, 400); EXPECT_STREQ(pending_str(ime), "S");
    press(ime, MOKYA_KEY_A, 500); EXPECT_STREQ(pending_str(ime), "a");   // wraps
}

TEST(Direct, LKeyCyclesTwoSlots) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    press(ime, MOKYA_KEY_L, 100); EXPECT_STREQ(pending_str(ime), "l");
    press(ime, MOKYA_KEY_L, 200); EXPECT_STREQ(pending_str(ime), "L");
    press(ime, MOKYA_KEY_L, 300); EXPECT_STREQ(pending_str(ime), "l");  // wraps
}

TEST(Direct, MKeyCyclesTwoSlots) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    press(ime, MOKYA_KEY_M, 100); EXPECT_STREQ(pending_str(ime), "m");
    press(ime, MOKYA_KEY_M, 200); EXPECT_STREQ(pending_str(ime), "M");
}

TEST(Direct, BackslashIsNoOp) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    EXPECT_FALSE(press(ime, MOKYA_KEY_BACKSLASH));
    EXPECT_FALSE(ime.has_pending());
}

TEST(Direct, DifferentKeyAutoCommitsPrior) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);    // pending "a"
    press(ime, MOKYA_KEY_D, 200);    // different key → commits "a", starts "d"
    EXPECT_EQ(L.committed, "a");
    EXPECT_STREQ(pending_str(ime), "d");
}

TEST(Direct, OKCommitsPending) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);
    press(ime, MOKYA_KEY_A, 200);   // pending "s"
    press(ime, MOKYA_KEY_OK);
    EXPECT_EQ(L.committed, "s");
    EXPECT_FALSE(ime.has_pending());
}

TEST(Direct, SpaceCommitsPendingAndSpace) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);   // pending "a"
    press(ime, MOKYA_KEY_SPACE);
    EXPECT_EQ(L.committed, "a ");   // commit + space
    EXPECT_FALSE(ime.has_pending());
}

TEST(Direct, SpaceIdleEmitsSpaceOnly) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_SPACE);
    EXPECT_EQ(L.committed, " ");
}

TEST(Direct, DigitKeyCyclesDigits) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    press(ime, MOKYA_KEY_1, 100); EXPECT_STREQ(pending_str(ime), "1");
    press(ime, MOKYA_KEY_1, 200); EXPECT_STREQ(pending_str(ime), "2");
    press(ime, MOKYA_KEY_1, 300); EXPECT_STREQ(pending_str(ime), "1");  // wraps
}

TEST(Direct, DigitCommitBeforeLetter) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_1, 100);   // digit "1"
    press(ime, MOKYA_KEY_A, 200);   // letter "a" — commits "1" first
    EXPECT_EQ(L.committed, "1");
    EXPECT_STREQ(pending_str(ime), "a");
}

TEST(Direct, DELCancelsPending) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);
    press(ime, MOKYA_KEY_DEL);
    EXPECT_FALSE(ime.has_pending());
    EXPECT_TRUE(L.committed.empty());
}

// ══════════════════════════════════════════════════════════════════════════
// SYM1 — short press / long-press
// ══════════════════════════════════════════════════════════════════════════

TEST(Sym1, ShortPressZhEmitsFullWidthComma) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    // SmartZh default.
    ime.process_key(kev(MOKYA_KEY_SYM1, true, 100));
    ime.process_key(kev(MOKYA_KEY_SYM1, false, 150));  // 50 ms hold
    EXPECT_EQ(L.committed, "\xef\xbc\x8c");  // ，
}

TEST(Sym1, ShortPressEnEmitsAsciiCommaWithTrailingSpace) {
    // SmartEn punctuation follows English sentence convention: ',' is
    // immediately followed by a space so the next word joins cleanly.
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_MODE);  // SmartEn
    ime.process_key(kev(MOKYA_KEY_SYM1, true, 100));
    ime.process_key(kev(MOKYA_KEY_SYM1, false, 150));
    EXPECT_EQ(L.committed, ", ");
}

TEST(Sym1, LongPressTickOpensPicker) {
    // Opens a picker via tick() after 500 ms hold. Phase A ships only the
    // open-flag; picker navigation is Phase A follow-up. We assert that
    // on_commit is NOT fired and that tick() reports state changed.
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    ime.process_key(kev(MOKYA_KEY_SYM1, true, 0));
    EXPECT_FALSE(ime.tick(100));   // too early
    EXPECT_FALSE(ime.tick(400));
    EXPECT_TRUE(ime.tick(600));    // past threshold
    // Release after long-press: no short-press emission.
    ime.process_key(kev(MOKYA_KEY_SYM1, false, 800));
    EXPECT_TRUE(L.committed.empty());
}

TEST(Sym1, ShortPressCommitsMultitapPending) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);  // Direct pending "a"
    ime.process_key(kev(MOKYA_KEY_SYM1, true, 200));
    ime.process_key(kev(MOKYA_KEY_SYM1, false, 250));
    // Commits "a" first, then emits "," (Direct = EN punctuation).
    EXPECT_EQ(L.committed, "a,");
}

// ══════════════════════════════════════════════════════════════════════════
// SYM2 — multi-tap cycling
// ══════════════════════════════════════════════════════════════════════════

TEST(Sym2, ZhCyclesPeriodQuestionBang) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_SYM2, 100); EXPECT_STREQ(pending_str(ime), "\xe3\x80\x82"); // 。
    press(ime, MOKYA_KEY_SYM2, 200); EXPECT_STREQ(pending_str(ime), "\xef\xbc\x9f"); // ？
    press(ime, MOKYA_KEY_SYM2, 300); EXPECT_STREQ(pending_str(ime), "\xef\xbc\x81"); // ！
    press(ime, MOKYA_KEY_SYM2, 400); EXPECT_STREQ(pending_str(ime), "\xe3\x80\x82"); // wraps 。
}

TEST(Sym2, EnCyclesPeriodQuestionBang) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_MODE);  // SmartEn
    press(ime, MOKYA_KEY_SYM2, 100); EXPECT_STREQ(pending_str(ime), ".");
    press(ime, MOKYA_KEY_SYM2, 200); EXPECT_STREQ(pending_str(ime), "?");
    press(ime, MOKYA_KEY_SYM2, 300); EXPECT_STREQ(pending_str(ime), "!");
}

TEST(Sym2, InvertedStyle) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_SYM2, 100);
    EXPECT_EQ(pending_style(ime), PendingStyle::Inverted);
}

TEST(Sym2, TimeoutCommits) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_SYM2, 100);
    EXPECT_TRUE(ime.tick(100 + 800));   // exactly at timeout
    EXPECT_EQ(L.committed, "\xe3\x80\x82");  // 。
    EXPECT_FALSE(ime.has_pending());
}

TEST(Sym2, DifferentKeyCommitsPrior) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_SYM2, 100);                 // 。 pending
    press(ime, MOKYA_KEY_OK);                         // OK commits
    EXPECT_EQ(L.committed, "\xe3\x80\x82");
}

} // namespace
