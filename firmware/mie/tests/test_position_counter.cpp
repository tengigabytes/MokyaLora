// test_position_counter.cpp - Unit tests for ImeLogic::count_positions and
//                              ImeLogic::first_n_positions_bytes.
// SPDX-License-Identifier: MIT
//
// The position counter classifies each phoneme key byte by its phonological
// role (Initial / Medial / Final / Tone) and counts syllable positions.
// See ime_keys.cpp for the slot role table.

#include <gtest/gtest.h>
#include <mie/ime_logic.h>

namespace {

// Phoneme key encoding mirror of gen_dict.py's PHONEME_TO_KEY:
//   slot 0  ㄅㄉ  -> key byte 0x21 ('!')
//   slot 5  ㄆㄊ  -> 0x26 ('&')
//   slot 10 ㄇㄋ  -> 0x2B ('+')
//   slot 16 ㄏㄒ  -> 0x31 ('1')
//   slot 1  ˇ ˋ  -> 0x22 (tone)
//   slot 8  ㄧㄛ  -> 0x29 (medial)
//   slot 14 ㄠㄤ  -> 0x2F (final)
//   ...
static inline char k(int slot) { return (char)(0x21 + slot); }
static inline char kSpace() { return (char)0x20; }   // tone-1 marker
static inline char kTone() { return (char)0x22; }    // ˇ/ˋ slot 1

} // namespace

// ── count_positions ───────────────────────────────────────────────────────

TEST(PositionCounter, EmptyAndNull) {
    EXPECT_EQ(mie::ImeLogic::count_positions(nullptr, 0), 0);
    EXPECT_EQ(mie::ImeLogic::count_positions("", 0), 0);
    char one = k(0);
    EXPECT_EQ(mie::ImeLogic::count_positions(&one, 0), 0);  // len=0 wins
}

TEST(PositionCounter, SingleInitial_OnePosition) {
    char seq[] = {k(0)};  // ㄅ alone
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 1), 1);
}

TEST(PositionCounter, FullSyllableWithTone_OnePosition) {
    // ㄋ ㄧ ˇ — slot 10 (Initial), slot 8 (Medial), slot 1 (Tone)
    char seq[] = {k(10), k(8), kTone()};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 3), 1);
}

TEST(PositionCounter, TwoInitialsBareAbbrev_TwoPositions) {
    // ㄋ ㄏ — abbrev for 你好 (initials only)
    char seq[] = {k(10), k(16)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 2), 2);
}

TEST(PositionCounter, FullThenAbbrev_TwoPositions) {
    // ㄋ ㄧ ˇ ㄏ — full first syllable + bare second initial
    char seq[] = {k(10), k(8), kTone(), k(16)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 4), 2);
}

TEST(PositionCounter, FourInitials_FourPositions) {
    // ㄆ ㄈ ㄔ ㄓ — 4-initial abbrev (e.g., 破釜沉舟)
    // slot 5 ㄆㄊ, slot 15 ㄈㄌ, slot 7 ㄔㄗ, slot 2 ㄓˊ
    char seq[] = {k(5), k(15), k(7), k(2)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 4), 4);
}

TEST(PositionCounter, SpaceTerminatesSyllable) {
    // ㄋ ㄧ ' ' ㄏ — ㄋㄧ + tone-1 SPACE + ㄏ = 2 positions
    char seq[] = {k(10), k(8), kSpace(), k(16)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 4), 2);
}

TEST(PositionCounter, MedialOrFinalAlone_OnePosition) {
    // ㄚ alone (slot 3 Final, treated as starting a vowel-initial syllable)
    char seq[] = {k(3)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 1), 1);
}

TEST(PositionCounter, FivePositions_FivePositions) {
    // 5 initials in a row
    char seq[] = {k(0), k(5), k(10), k(15), k(16)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 5), 5);
}

TEST(PositionCounter, InitialAfterMedialFinal_StartsNewPosition) {
    // Regression: user types I+M+F+I (full tone-less syllable then a bare
    // initial for an abbreviation). Old logic kept last_role=Final and
    // missed the second position. Slots: 11 ㄎ(Init), 13 ㄨ(Medial),
    // 4 ㄞ(Final), 15 ㄈ/ㄌ(Init) — mimics ㄎㄨㄞ + ㄌ (快樂).
    char seq[] = {k(11), k(13), k(4), k(15)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 4), 2);
}

TEST(PositionCounter, InitialAfterMedial_StartsNewPosition) {
    // Initial appearing after a single Medial (no Final) should still
    // close the syllable and start a new position.
    //  slot 0 ㄅ(Init) + slot 8 ㄧ(Medial) + slot 10 ㄇ(Init) = 2 positions
    char seq[] = {k(0), k(8), k(10)};
    EXPECT_EQ(mie::ImeLogic::count_positions(seq, 3), 2);
}

// ── first_n_positions_bytes ───────────────────────────────────────────────

TEST(FirstNPosBytes, EmptyAndZero) {
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(nullptr, 0, 1), 0);
    char seq[] = {k(0)};
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 1, 0), 0);
}

TEST(FirstNPosBytes, SingleByteFirstPosition) {
    char seq[] = {k(0)};
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 1, 1), 1);
}

TEST(FirstNPosBytes, FullSyllableIncludesTone) {
    // ㄋ ㄧ ˇ — 1 position spanning all 3 bytes
    char seq[] = {k(10), k(8), kTone()};
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 3, 1), 3);
}

TEST(FirstNPosBytes, AbbrevTwoInitialsTakeFirst) {
    // ㄋ ㄏ — 2 positions; first 1 position = first byte
    char seq[] = {k(10), k(16)};
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 2, 1), 1);
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 2, 2), 2);
}

TEST(FirstNPosBytes, FullThenAbbrev_FirstPositionIs3Bytes) {
    // ㄋ ㄧ ˇ ㄏ — position 1 covers bytes 0..2, position 2 covers byte 3
    char seq[] = {k(10), k(8), kTone(), k(16)};
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 4, 1), 3);
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 4, 2), 4);
}

TEST(FirstNPosBytes, FivePositions_TruncateToFour) {
    // ㄆ ㄈ ㄔ ㄓ ㄉ — 5 positions, take first 4
    char seq[] = {k(5), k(15), k(7), k(2), k(0)};
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 5, 4), 4);
}

TEST(FirstNPosBytes, RequestMoreThanAvailable_ReturnsAll) {
    char seq[] = {k(10), k(16)};  // 2 positions
    EXPECT_EQ(mie::ImeLogic::first_n_positions_bytes(seq, 2, 5), 2);
}
