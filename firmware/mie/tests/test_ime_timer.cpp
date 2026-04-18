// SPDX-License-Identifier: MIT
// test_ime_timer.cpp — tick() timing: multi-tap timeout + SYM1 long-press.

#include "test_helpers.h"

namespace {

using mie::ImeLogic;
using mie::TrieSearcher;

static void enter_direct(ImeLogic& ime) {
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_MODE);
}

// ── tick with no state ─────────────────────────────────────────────────────

TEST(Timer, TickNoStateIsFalse) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    EXPECT_FALSE(ime.tick(100));
    EXPECT_FALSE(ime.tick(1000));
    EXPECT_FALSE(ime.tick(100000));
}

// ── Multi-tap timeout (800 ms) ─────────────────────────────────────────────

TEST(Timer, MultitapWithinTimeoutCycles) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);
    press(ime, MOKYA_KEY_A, 500);    // 400 ms < 800 ms → cycles
    EXPECT_STREQ(pending_str(ime), "s");
}

TEST(Timer, MultitapTimeoutCommits) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);    // pending "a"
    EXPECT_TRUE(ime.tick(900));      // 800 ms past last press
    EXPECT_EQ(L.committed, "a");
    EXPECT_FALSE(ime.has_pending());
}

TEST(Timer, MultitapBoundaryExactly800ms) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);
    // tick at exactly 900 ms (900 - 100 == 800) should trigger commit.
    EXPECT_TRUE(ime.tick(900));
    EXPECT_EQ(L.committed, "a");
}

TEST(Timer, MultitapBelow800msNoCommit) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);
    EXPECT_FALSE(ime.tick(899));     // 799 ms < 800 → no-op
    EXPECT_TRUE(ime.has_pending());
    EXPECT_TRUE(L.committed.empty());
}

TEST(Timer, MultitapExtendedByPressRefreshesClock) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    enter_direct(ime);
    press(ime, MOKYA_KEY_A, 100);
    press(ime, MOKYA_KEY_A, 500);    // refresh last_ms to 500
    EXPECT_FALSE(ime.tick(1000));    // only 500 ms elapsed since last press
    EXPECT_TRUE(ime.has_pending());
    EXPECT_TRUE(L.committed.empty());
}

// ── SYM1 long-press (500 ms) ───────────────────────────────────────────────

TEST(Timer, Sym1LongPressTriggersAt500ms) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    ime.process_key(kev(MOKYA_KEY_SYM1, true, 0));
    EXPECT_FALSE(ime.tick(499));
    EXPECT_TRUE(ime.tick(500));
}

TEST(Timer, Sym1ReleaseBeforeLongPressIsShort) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    ime.process_key(kev(MOKYA_KEY_SYM1, true, 0));
    ime.process_key(kev(MOKYA_KEY_SYM1, false, 100));   // short-press
    EXPECT_EQ(L.committed, "\xef\xbc\x8c");             // ，
}

TEST(Timer, Sym1TickIdempotentAfterLongPress) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    ime.process_key(kev(MOKYA_KEY_SYM1, true, 0));
    EXPECT_TRUE(ime.tick(600));                // fires once
    EXPECT_FALSE(ime.tick(700));               // stays silent
    EXPECT_FALSE(ime.tick(1500));
}

} // namespace
