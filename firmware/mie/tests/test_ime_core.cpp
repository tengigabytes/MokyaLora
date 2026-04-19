// SPDX-License-Identifier: MIT
// test_ime_core.cpp — Top-level routing: MODE, DEL, DPAD, abort, BACK
//                      filtering, listener event firing.

#include "test_helpers.h"

namespace {

using mie::ImeLogic;
using mie::InputMode;
using mie::NavDir;
using mie::PendingStyle;
using mie::TrieSearcher;

// ── Initial state ───────────────────────────────────────────────────────────

TEST(Core, InitialStateIsSmartZh) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    EXPECT_EQ(ime.mode(), InputMode::SmartZh);
    EXPECT_FALSE(ime.has_pending());
    EXPECT_FALSE(ime.has_candidates());
    EXPECT_EQ(pending_bytes(ime), 0);
    EXPECT_EQ(pending_style(ime), PendingStyle::None);
    EXPECT_EQ(ime.candidate_count(), 0);
    EXPECT_EQ(ime.selected(), 0);
}

TEST(Core, ModeIndicator) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    EXPECT_STREQ(ime.mode_indicator(), "\xe4\xb8\xad");  // 中
    press(ime, MOKYA_KEY_MODE);
    EXPECT_STREQ(ime.mode_indicator(), "EN");
    press(ime, MOKYA_KEY_MODE);
    EXPECT_STREQ(ime.mode_indicator(), "ABC");
    press(ime, MOKYA_KEY_MODE);
    EXPECT_STREQ(ime.mode_indicator(), "\xe4\xb8\xad");
}

// ── MODE cycling ────────────────────────────────────────────────────────────

TEST(Core, ModeCyclesThreeModes) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_MODE); EXPECT_EQ(ime.mode(), InputMode::SmartEn);
    press(ime, MOKYA_KEY_MODE); EXPECT_EQ(ime.mode(), InputMode::Direct);
    press(ime, MOKYA_KEY_MODE); EXPECT_EQ(ime.mode(), InputMode::SmartZh);
}

TEST(Core, ModeKeyClearsPending) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);
    EXPECT_TRUE(ime.has_pending());
    press(ime, MOKYA_KEY_MODE);
    EXPECT_FALSE(ime.has_pending());
}

// ── BACK / NONE / LIMIT ─────────────────────────────────────────────────────

TEST(Core, BackNotConsumed) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    EXPECT_FALSE(press(ime, MOKYA_KEY_BACK));
    EXPECT_EQ(L.delete_before_count, 0);
    EXPECT_TRUE(L.cursor_moves.empty());
    EXPECT_TRUE(L.committed.empty());
    EXPECT_EQ(L.composition_changed_count, 0);
}

TEST(Core, KeyNoneIgnored) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    EXPECT_FALSE(ime.process_key(kev(MOKYA_KEY_NONE)));
}

TEST(Core, KeyAtLimitIgnored) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    EXPECT_FALSE(ime.process_key(kev((mokya_keycode_t)0x40)));
    EXPECT_FALSE(ime.process_key(kev((mokya_keycode_t)0xFF)));
}

TEST(Core, KeyUpIgnoredExceptSym1) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    EXPECT_FALSE(ime.process_key(kev(MOKYA_KEY_1, /*pressed=*/false)));
    EXPECT_FALSE(ime.has_pending());
}

// ── DEL priority: pending → listener on_delete_before ──────────────────────

TEST(Core, DELWithSmartPendingRemovesLastKey) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_Q);
    int len_before = pending_bytes(ime);
    press(ime, MOKYA_KEY_DEL);
    EXPECT_TRUE(ime.has_pending());
    EXPECT_LT(pending_bytes(ime), len_before);
    EXPECT_EQ(L.delete_before_count, 0);  // did not forward
}

TEST(Core, DELEmptyFiresOnDeleteBefore) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    EXPECT_TRUE(press(ime, MOKYA_KEY_DEL));
    EXPECT_EQ(L.delete_before_count, 1);
}

TEST(Core, DELCancelsDirectPendingNoCommit) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_MODE);
    press(ime, MOKYA_KEY_MODE);   // Direct
    press(ime, MOKYA_KEY_A);
    EXPECT_TRUE(ime.has_pending());
    L.reset();
    press(ime, MOKYA_KEY_DEL);
    EXPECT_FALSE(ime.has_pending());
    EXPECT_TRUE(L.committed.empty());
}

// ── DPAD routing ────────────────────────────────────────────────────────────

TEST(Core, DPADNoCandidatesFiresCursorMove) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    EXPECT_TRUE(press(ime, MOKYA_KEY_LEFT));
    EXPECT_TRUE(press(ime, MOKYA_KEY_RIGHT));
    EXPECT_TRUE(press(ime, MOKYA_KEY_UP));
    EXPECT_TRUE(press(ime, MOKYA_KEY_DOWN));
    ASSERT_EQ(L.cursor_moves.size(), 4U);
    EXPECT_EQ(L.cursor_moves[0], NavDir::Left);
    EXPECT_EQ(L.cursor_moves[1], NavDir::Right);
    EXPECT_EQ(L.cursor_moves[2], NavDir::Up);
    EXPECT_EQ(L.cursor_moves[3], NavDir::Down);
}

TEST(Core, DPADWithCandidatesSelects) {
    std::vector<uint8_t> dat, val;
    build_two_zh(dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);

    press(ime, MOKYA_KEY_1);
    ASSERT_EQ(ime.candidate_count(), 2);
    EXPECT_EQ(ime.selected(), 0);

    press(ime, MOKYA_KEY_RIGHT);
    EXPECT_EQ(ime.selected(), 1);
    press(ime, MOKYA_KEY_LEFT);
    EXPECT_EQ(ime.selected(), 0);
    EXPECT_TRUE(L.cursor_moves.empty());  // none fired — consumed for candidate nav
}

TEST(Core, DPADLeftWrapsFromZero) {
    std::vector<uint8_t> dat, val;
    build_two_zh(dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_LEFT);  // wraps to last
    EXPECT_EQ(ime.selected(), 1);
}

TEST(Core, DPADRightWrapsAtEnd) {
    std::vector<uint8_t> dat, val;
    build_two_zh(dat, val);
    TrieSearcher ts;
    ASSERT_TRUE(ts.load_from_memory(dat.data(), dat.size(), val.data(), val.size()));
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_RIGHT);  // 0 -> 1
    press(ime, MOKYA_KEY_RIGHT);  // 1 -> 0 (wraps)
    EXPECT_EQ(ime.selected(), 0);
}

// ── abort() ────────────────────────────────────────────────────────────────

TEST(Core, AbortClearsPending) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_1);
    L.reset();
    ime.abort();
    EXPECT_FALSE(ime.has_pending());
    EXPECT_EQ(L.composition_changed_count, 1);
}

TEST(Core, AbortIdleNoFire) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    ime.abort();
    EXPECT_EQ(L.composition_changed_count, 0);
}

// ── Listener lifecycle ─────────────────────────────────────────────────────

TEST(Core, ListenerDetach) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_1);
    EXPECT_GT(L.composition_changed_count, 0);

    ime.set_listener(nullptr);
    L.reset();
    press(ime, MOKYA_KEY_Q);
    EXPECT_EQ(L.composition_changed_count, 0);
}

TEST(Core, NoListenerNoPanic) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    press(ime, MOKYA_KEY_1);
    press(ime, MOKYA_KEY_DEL);
    press(ime, MOKYA_KEY_MODE);
    SUCCEED();
}

TEST(Core, CompositionChangedFiresOnInput) {
    TrieSearcher ts;
    ImeLogic ime(ts);
    MockListener L; ime.set_listener(&L);
    press(ime, MOKYA_KEY_1);
    EXPECT_EQ(L.composition_changed_count, 1);
}

} // namespace
