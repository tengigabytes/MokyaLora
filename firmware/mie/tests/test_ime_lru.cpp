// test_ime_lru.cpp — Integration tests for ImeLogic ⇄ LruCache wiring
//                    (Phase 1.6 Step 2).
// SPDX-License-Identifier: MIT
//
// Exercises commit → re-search → LRU rank promotion through the full
// ImeLogic pipeline with an attached CompositionSearcher. Dict blobs are
// built in-memory with a minimal v4 layout (chars only, no multi-char
// words, no phoneme_pos section) so the test is decoupled from the real
// 2 MB dict file.

#include <gtest/gtest.h>
#include <mie/composition_searcher.h>
#include <mie/ime_logic.h>
#include <mie/lru_cache.h>
#include <mie/trie_searcher.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ── Minimal v4 blob builder (flat chars only) ────────────────────────────
//
// Matches the layout assumed by CompositionSearcher; reduced to what the
// LRU tests need: char_table with readings, no multi-char words, no
// phoneme_pos (header.flags=0 → long-press hints ignored by the dict, but
// LruCache still honours them independently).
struct FlatV4Builder {
    struct Reading {
        std::vector<uint8_t> keyseq;
        uint8_t  tone = 0;
        uint16_t freq = 0;
    };
    struct CharEntry {
        std::string           utf8;
        std::vector<Reading>  readings;
    };
    std::vector<CharEntry> chars;

    std::vector<uint8_t> build() {
        constexpr int kMaxGroup = 8;
        // No words — all group_headers zero.
        std::vector<std::pair<uint32_t, uint32_t>> group_headers(kMaxGroup, {0, 0});

        std::vector<uint8_t>  char_section;
        std::vector<uint32_t> char_offsets;
        for (const auto& ch : chars) {
            char_offsets.push_back((uint32_t)char_section.size());
            char_section.push_back((uint8_t)ch.utf8.size());
            for (char c : ch.utf8) char_section.push_back((uint8_t)c);
            char_section.push_back((uint8_t)ch.readings.size());
            for (const auto& r : ch.readings) {
                char_section.push_back((uint8_t)r.keyseq.size());
                for (uint8_t b : r.keyseq) char_section.push_back(b);
                char_section.push_back(r.tone);
                char_section.push_back((uint8_t)(r.freq & 0xFF));
                char_section.push_back((uint8_t)(r.freq >> 8));
            }
        }
        char_offsets.push_back((uint32_t)char_section.size());

        std::vector<uint8_t> word_section;
        for (const auto& gh : group_headers) {
            for (int i = 0; i < 4; ++i)
                word_section.push_back((uint8_t)((gh.first >> (i * 8)) & 0xFF));
            for (int i = 0; i < 4; ++i)
                word_section.push_back((uint8_t)((gh.second >> (i * 8)) & 0xFF));
        }

        std::vector<uint32_t> word_offsets;
        word_offsets.push_back((uint32_t)word_section.size());  // sentinel

        std::vector<uint8_t> char_offs_section;
        for (uint32_t o : char_offsets)
            for (int i = 0; i < 4; ++i)
                char_offs_section.push_back((uint8_t)((o >> (i * 8)) & 0xFF));
        std::vector<uint8_t> word_offs_section;
        for (uint32_t o : word_offsets)
            for (int i = 0; i < 4; ++i)
                word_offs_section.push_back((uint8_t)((o >> (i * 8)) & 0xFF));

        uint32_t C = (uint32_t)chars.size();
        std::vector<uint8_t>  first_section;
        std::vector<uint32_t> first_offsets(C + 1, 0);
        for (auto off : first_offsets)
            for (int i = 0; i < 4; ++i)
                first_section.push_back((uint8_t)((off >> (i * 8)) & 0xFF));

        constexpr int kKeySlots = 24;
        constexpr uint8_t kKeyMin = 0x20;
        std::vector<std::vector<uint16_t>> by_key(kKeySlots);
        for (uint32_t cid = 0; cid < C; ++cid) {
            std::vector<uint8_t> seen_first;
            for (const auto& r : chars[cid].readings) {
                if (!r.keyseq.empty()) {
                    uint8_t fb = r.keyseq[0];
                    if (std::find(seen_first.begin(), seen_first.end(), fb) ==
                        seen_first.end()) {
                        seen_first.push_back(fb);
                        if (fb >= kKeyMin && fb < kKeyMin + kKeySlots)
                            by_key[fb - kKeyMin].push_back((uint16_t)cid);
                    }
                }
            }
        }
        std::vector<uint8_t> key_section;
        std::vector<uint32_t> key_offsets(kKeySlots + 1, 0);
        for (int i = 0; i < kKeySlots; ++i)
            key_offsets[i + 1] = key_offsets[i] + (uint32_t)by_key[i].size();
        for (auto off : key_offsets)
            for (int i = 0; i < 4; ++i)
                key_section.push_back((uint8_t)((off >> (i * 8)) & 0xFF));
        for (int i = 0; i < kKeySlots; ++i)
            for (uint16_t cid : by_key[i]) {
                key_section.push_back((uint8_t)(cid & 0xFF));
                key_section.push_back((uint8_t)(cid >> 8));
            }

        constexpr uint32_t kHeaderSize = 0x30;
        uint32_t char_off      = kHeaderSize;
        uint32_t word_off      = char_off + (uint32_t)char_section.size();
        uint32_t first_off     = word_off + (uint32_t)word_section.size();
        uint32_t key_off       = first_off + (uint32_t)first_section.size();
        uint32_t char_offs_off = key_off + (uint32_t)key_section.size();
        uint32_t word_offs_off = char_offs_off + (uint32_t)char_offs_section.size();
        uint32_t total_size    = word_offs_off + (uint32_t)word_offs_section.size();

        std::vector<uint8_t> header(kHeaderSize, 0);
        std::memcpy(header.data(), "MIE4", 4);
        uint16_t ver = 4, flg = 0;
        std::memcpy(header.data() + 4,  &ver, 2);
        std::memcpy(header.data() + 6,  &flg, 2);
        uint32_t W32 = 0;
        std::memcpy(header.data() +  8, &C,   4);
        std::memcpy(header.data() + 12, &W32, 4);
        std::memcpy(header.data() + 16, &char_off,  4);
        std::memcpy(header.data() + 20, &word_off,  4);
        std::memcpy(header.data() + 24, &first_off, 4);
        std::memcpy(header.data() + 28, &key_off,   4);
        std::memcpy(header.data() + 0x20, &total_size,    4);
        std::memcpy(header.data() + 0x24, &char_offs_off, 4);
        std::memcpy(header.data() + 0x28, &word_offs_off, 4);

        std::vector<uint8_t> out;
        out.insert(out.end(), header.begin(),           header.end());
        out.insert(out.end(), char_section.begin(),     char_section.end());
        out.insert(out.end(), word_section.begin(),     word_section.end());
        out.insert(out.end(), first_section.begin(),    first_section.end());
        out.insert(out.end(), key_section.begin(),      key_section.end());
        out.insert(out.end(), char_offs_section.begin(), char_offs_section.end());
        out.insert(out.end(), word_offs_section.begin(), word_offs_section.end());
        return out;
    }
};

static inline uint8_t k_b(int slot) { return (uint8_t)(slot + 0x21); }

// Build a dict with four single-char entries whose first reading byte is
// slot 16 (ㄏ) and second byte varies. Frequencies control the default
// rank order. kc for slot 16 is MOKYA_KEY_C (see ime_smart.cpp's slot map).
std::vector<uint8_t> build_four_char_dict() {
    FlatV4Builder b;
    auto mk = [](std::initializer_list<int> slots, uint16_t freq) {
        FlatV4Builder::Reading r;
        for (int s : slots) r.keyseq.push_back(k_b(s));
        r.tone = 3; r.freq = freq;
        return r;
    };
    b.chars.push_back({"和", {mk({16, 14, 1}, 1000)}});  // rank 0
    b.chars.push_back({"好", {mk({16, 14, 1}, 900)}});   // rank 1
    b.chars.push_back({"黑", {mk({16, 14, 1}, 800)}});   // rank 2
    b.chars.push_back({"海", {mk({16, 14, 1}, 700)}});   // rank 3
    return b.build();
}

// Find the rank (index) of `utf8` in the current candidate list; -1 if
// absent.
int rank_of(const mie::ImeLogic& ime, const char* utf8) {
    for (int i = 0; i < ime.candidate_count(); ++i) {
        if (std::strcmp(ime.candidate(i).word, utf8) == 0) return i;
    }
    return -1;
}

struct QuietListener : mie::IImeListener {
    void on_commit(const char*) override {}
    void on_composition_changed() override {}
};

// Send a single key press to ImeLogic. Helper for tests.
void press(mie::ImeLogic& ime, mokya_keycode_t kc, uint32_t ms,
           uint8_t flags = 0) {
    mie::KeyEvent ev{};
    ev.keycode = kc;
    ev.pressed = true;
    ev.now_ms  = ms;
    ev.flags   = flags;
    ime.process_key(ev);
}

} // namespace

// ── Test 1: first commit promotes word to rank 0 on next identical query ─
TEST(ImeLru, CommitPromotesToRankZero) {
    auto blob = build_four_char_dict();
    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::TrieSearcher dummy_v2;
    mie::ImeLogic ime(dummy_v2, nullptr);
    ime.attach_composition_searcher(&cs);
    QuietListener L; ime.set_listener(&L);

    // First query: user types ㄏ (slot 16 → MOKYA_KEY_C). count_positions=1
    // dispatches to target=1 char search. Default rank is (和, 好, 黑, 海)
    // by descending freq; 海 sits at rank 3.
    press(ime, MOKYA_KEY_C, 100);
    ASSERT_GT(ime.candidate_count(), 0);
    EXPECT_EQ(rank_of(ime, "海"), 3);

    // User navigates to 海 and commits.
    ime.set_selected(rank_of(ime, "海"));
    mie::KeyEvent ok{};
    ok.keycode = MOKYA_KEY_OK; ok.pressed = true; ok.now_ms = 120;
    ime.process_key(ok);
    EXPECT_EQ(ime.lru_count(), 1);

    // Second query with the same single byte: 海 should now rank 0.
    press(ime, MOKYA_KEY_C, 200);
    ASSERT_GT(ime.candidate_count(), 0);
    EXPECT_EQ(rank_of(ime, "海"), 0);

    // Dict results remain available behind the LRU hit (deduped by utf8).
    EXPECT_GE(rank_of(ime, "和"), 1);
    EXPECT_GE(rank_of(ime, "好"), 1);
}

// ── Test 2: different phoneme hint at query time rejects the LRU entry ───
TEST(ImeLru, DifferentHintRejectsLruEntry) {
    auto blob = build_four_char_dict();
    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::TrieSearcher dummy_v2;
    mie::ImeLogic ime(dummy_v2, nullptr);
    ime.attach_composition_searcher(&cs);
    QuietListener L; ime.set_listener(&L);

    // Long-press the first key → phoneme_hint_[0] = 0 (primary); append
    // second key with ANY so pos_packed bits for byte 1 stay ANY.
    press(ime, MOKYA_KEY_C, 100, mie::KEY_FLAG_LONG_PRESS);  // long-press → hint 0
    press(ime, MOKYA_KEY_L, 110);                        // short-tap → ANY
    ASSERT_GT(ime.candidate_count(), 0);

    // Commit 好 (rank 1 by default freq) with pos_packed = (byte0=primary).
    ime.set_selected(rank_of(ime, "好"));
    mie::KeyEvent ok{};
    ok.keycode = MOKYA_KEY_OK; ok.pressed = true; ok.now_ms = 120;
    ime.process_key(ok);
    ASSERT_EQ(ime.lru_count(), 1);

    // Abort to clear state, then re-query with hint 1 (secondary) on byte 0.
    ime.abort();
    // First long-press: hint=0 primary. Second long-press within
    // kMultiTapTimeoutMs cycles to hint=1 secondary (phoneme_idx = 1 % N).
    press(ime, MOKYA_KEY_C, 200, mie::KEY_FLAG_LONG_PRESS);   // primary
    press(ime, MOKYA_KEY_C, 210, mie::KEY_FLAG_LONG_PRESS);   // cycle → secondary (slot 16 has 2 phonemes → 1 % 2 = 1)

    // The cycle replaced the same byte in place, so key_seq_ is still 1 byte
    // long with hint=1 (secondary). Append byte 2 to form a 2-byte query.
    press(ime, MOKYA_KEY_L, 220);

    // LRU stored (hint_byte0 = primary=0); query has hint_byte0 = secondary=1.
    // The LRU hit must be rejected; 好 falls back to its dict rank.
    EXPECT_NE(rank_of(ime, "好"), 0);
}

// ── Test 3: prefix LRU hit still ranks ahead of dict results ─────────────
// Stored klen < user_n prefix path (LruCache lookup allows it when
// stored klen >= 2). Requires chars with tone=1 readings so a trailing
// SPACE (first-tone marker) doesn't zero out the dict search.
TEST(ImeLru, PrefixLruEntryRanksAhead) {
    FlatV4Builder b;
    auto mk = [](std::initializer_list<int> slots, uint16_t freq) {
        FlatV4Builder::Reading r;
        for (int s : slots) r.keyseq.push_back(k_b(s));
        r.tone = 1; r.freq = freq;
        return r;
    };
    b.chars.push_back({"夯", {mk({16, 14}, 1000)}});   // rank 0
    b.chars.push_back({"烘", {mk({16, 14}, 900)}});    // rank 1
    b.chars.push_back({"轟", {mk({16, 14}, 800)}});    // rank 2
    b.chars.push_back({"齁", {mk({16, 14}, 700)}});    // rank 3
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::TrieSearcher dummy_v2;
    mie::ImeLogic ime(dummy_v2, nullptr);
    ime.attach_composition_searcher(&cs);
    QuietListener L; ime.set_listener(&L);

    // Commit 齁 via the 2-byte reading [ㄏ, ㄠ] → stored LRU klen = 2.
    press(ime, MOKYA_KEY_C, 100);  // slot 16 (ㄏ)
    press(ime, MOKYA_KEY_L, 110);  // slot 14 (ㄠ)
    ASSERT_GT(ime.candidate_count(), 0);
    ime.set_selected(rank_of(ime, "齁"));
    mie::KeyEvent ok{};
    ok.keycode = MOKYA_KEY_OK; ok.pressed = true; ok.now_ms = 120;
    ime.process_key(ok);
    ASSERT_EQ(ime.lru_count(), 1);

    // Re-query with an extra trailing SPACE (first-tone marker 0x20).
    // key_seq_ = {ㄏ, ㄠ, 0x20}, length 3. Stored klen = 2 < user_n = 3,
    // so LruCache's prefix rule kicks in. Dict search_chars strips the
    // trailing SPACE and requires tone==1 — which matches our tone=1
    // dict, so we still see dict results behind the LRU hit.
    press(ime, MOKYA_KEY_C,     200);
    press(ime, MOKYA_KEY_L,     210);
    press(ime, MOKYA_KEY_SPACE, 220);
    ASSERT_GT(ime.candidate_count(), 0);
    EXPECT_EQ(rank_of(ime, "齁"), 0);
}

// ── Test 4: second identical commit bumps the entry above a recently ─────
//            committed alternative (tie-break on use_count).
TEST(ImeLru, UseCountTieBreak) {
    auto blob = build_four_char_dict();
    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::TrieSearcher dummy_v2;
    mie::ImeLogic ime(dummy_v2, nullptr);
    ime.attach_composition_searcher(&cs);
    QuietListener L; ime.set_listener(&L);

    auto commit_char = [&](const char* utf8, uint32_t ms) {
        press(ime, MOKYA_KEY_C, ms);
        press(ime, MOKYA_KEY_L, ms + 5);
        ime.set_selected(rank_of(ime, utf8));
        mie::KeyEvent ok{};
        ok.keycode = MOKYA_KEY_OK; ok.pressed = true; ok.now_ms = ms + 10;
        ime.process_key(ok);
    };

    // Commit 海 at time 100, then 好 at time 200 (both now in LRU; 好 is
    // most-recent so last_used_ms 210 > 海's 110).
    commit_char("海", 100);
    commit_char("好", 200);

    // Commit 海 AGAIN at time 200 — same ms as 好's entry to force the
    // tie-break onto use_count. 海's use_count goes 1 → 2.
    commit_char("海", 200);

    // Re-query. Both entries tie on last_used_ms = 210 (海 last commit bumped
    // to 210); 海.use_count = 2 outranks 好.use_count = 1.
    press(ime, MOKYA_KEY_C, 300);
    press(ime, MOKYA_KEY_L, 305);
    EXPECT_EQ(rank_of(ime, "海"), 0);
    EXPECT_EQ(rank_of(ime, "好"), 1);
}

// ── Test 5: serialize → deserialize through ImeLogic preserves promotion ─
TEST(ImeLru, SerializeDeserializeThroughImeLogic) {
    auto blob = build_four_char_dict();
    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::TrieSearcher dummy_v2;
    mie::ImeLogic first(dummy_v2, nullptr);
    first.attach_composition_searcher(&cs);
    QuietListener L1; first.set_listener(&L1);

    press(first, MOKYA_KEY_C, 100);
    press(first, MOKYA_KEY_L, 110);
    first.set_selected(rank_of(first, "海"));
    mie::KeyEvent ok{};
    ok.keycode = MOKYA_KEY_OK; ok.pressed = true; ok.now_ms = 120;
    first.process_key(ok);
    ASSERT_EQ(first.lru_count(), 1);

    std::vector<uint8_t> buf(first.lru_serialized_size());
    ASSERT_EQ(first.serialize_lru(buf.data(), (int)buf.size()),
              first.lru_serialized_size());

    // "Reboot": fresh ImeLogic, same dict, load persisted LRU.
    mie::ImeLogic second(dummy_v2, nullptr);
    second.attach_composition_searcher(&cs);
    QuietListener L2; second.set_listener(&L2);
    ASSERT_TRUE(second.load_lru(buf.data(), (int)buf.size()));
    EXPECT_EQ(second.lru_count(), 1);

    press(second, MOKYA_KEY_C, 500);
    press(second, MOKYA_KEY_L, 510);
    EXPECT_EQ(rank_of(second, "海"), 0);
}
