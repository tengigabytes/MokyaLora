// test_composition_searcher.cpp - Unit tests for CompositionSearcher (MIED v4)
// SPDX-License-Identifier: MIT
//
// These tests construct minimal v4 blobs in-memory and exercise the reader
// + composition_matches + search() path, keeping the scope independent of
// the full dict file. Integration with real libchewing data is tested in
// mie_repl via the --v4-output build path.

#include <gtest/gtest.h>
#include <mie/composition_searcher.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Tiny v4 blob builder for tests. Produces a minimal, well-formed binary
// covering at least one multi-char word so composition_matches can exercise.
struct TinyV4Builder {
    struct Reading {
        std::vector<uint8_t> keyseq;
        uint8_t              tone;
        uint16_t             freq;
        // Phase 1.4 long-press filtering: per-byte phoneme position
        // (0 = primary, 1 = secondary, 2 = tertiary). Only emitted when
        // TinyV4Builder::include_phoneme_pos is true. Leave empty for the
        // legacy (bit0=0) code path.
        std::vector<uint8_t> phoneme_pos;
    };
    struct CharEntry {
        std::string          utf8;
        std::vector<Reading> readings;
    };
    struct WordEntry {
        std::vector<uint16_t> char_ids;
        std::vector<uint8_t>  reading_idxs;  // same size as char_ids; 0 = default
        uint16_t              freq;
    };

    std::vector<CharEntry> chars;
    std::vector<WordEntry> words;
    bool include_phoneme_pos = false;   // emit header.flags bit 0 + extra byte

    // Serialise to MIED v4 binary.
    std::vector<uint8_t> build() {
        // Group words by char_count (1..8+) and sort within group by freq desc.
        // Then assign word_ids in group order.
        constexpr int kMaxGroup = 8;
        std::vector<std::vector<int>> grouped(kMaxGroup);  // indices into `words`
        for (int i = 0; i < (int)words.size(); ++i) {
            int n = (int)words[i].char_ids.size();
            int bucket = n < kMaxGroup ? n - 1 : kMaxGroup - 1;
            grouped[bucket].push_back(i);
        }
        for (auto& g : grouped) {
            std::sort(g.begin(), g.end(), [this](int a, int b) {
                return words[a].freq > words[b].freq;
            });
        }
        std::vector<int> flat_order;
        std::vector<std::pair<uint32_t, uint32_t>> group_headers(kMaxGroup);
        for (int g = 0; g < kMaxGroup; ++g) {
            group_headers[g] = {(uint32_t)grouped[g].size(),
                                (uint32_t)flat_order.size()};
            for (int idx : grouped[g]) flat_order.push_back(idx);
        }

        // char_table section + record char_offsets while writing
        std::vector<uint8_t> char_section;
        std::vector<uint32_t> char_offsets;
        for (const auto& ch : chars) {
            char_offsets.push_back((uint32_t)char_section.size());
            char_section.push_back((uint8_t)ch.utf8.size());
            for (char c : ch.utf8) char_section.push_back((uint8_t)c);
            char_section.push_back((uint8_t)ch.readings.size());
            for (const auto& r : ch.readings) {
                char_section.push_back((uint8_t)r.keyseq.size());
                for (uint8_t b : r.keyseq) char_section.push_back(b);
                if (include_phoneme_pos) {
                    uint8_t packed = 0;
                    for (int i = 0; i < (int)r.phoneme_pos.size() && i < 4; ++i)
                        packed |= (uint8_t)((r.phoneme_pos[i] & 0x3) << (2 * i));
                    char_section.push_back(packed);
                }
                char_section.push_back(r.tone);
                char_section.push_back((uint8_t)(r.freq & 0xFF));
                char_section.push_back((uint8_t)(r.freq >> 8));
            }
        }
        char_offsets.push_back((uint32_t)char_section.size());

        // word_table section: 8 group headers then records in flat_order,
        // also record word_offsets (measured from word_table start).
        std::vector<uint8_t> word_section;
        std::vector<uint32_t> word_offsets;
        for (const auto& gh : group_headers) {
            for (int i = 0; i < 4; ++i)
                word_section.push_back((uint8_t)((gh.first >> (i * 8)) & 0xFF));
            for (int i = 0; i < 4; ++i)
                word_section.push_back((uint8_t)((gh.second >> (i * 8)) & 0xFF));
        }
        for (int idx : flat_order) {
            word_offsets.push_back((uint32_t)word_section.size());
            const auto& w = words[idx];
            uint8_t n = (uint8_t)w.char_ids.size();
            bool has_overrides = false;
            for (uint8_t r : w.reading_idxs) if (r != 0) { has_overrides = true; break; }
            uint8_t flags = has_overrides ? 1 : 0;
            word_section.push_back(n);
            word_section.push_back(flags);
            word_section.push_back((uint8_t)(w.freq & 0xFF));
            word_section.push_back((uint8_t)(w.freq >> 8));
            for (uint16_t cid : w.char_ids) {
                word_section.push_back((uint8_t)(cid & 0xFF));
                word_section.push_back((uint8_t)(cid >> 8));
            }
            if (has_overrides)
                for (uint8_t r : w.reading_idxs) word_section.push_back(r);
        }
        word_offsets.push_back((uint32_t)word_section.size());

        // Serialize the two offset sections (u32 LE).
        std::vector<uint8_t> char_offs_section;
        for (uint32_t o : char_offsets) {
            for (int i = 0; i < 4; ++i)
                char_offs_section.push_back((uint8_t)((o >> (i * 8)) & 0xFF));
        }
        std::vector<uint8_t> word_offs_section;
        for (uint32_t o : word_offsets) {
            for (int i = 0; i < 4; ++i)
                word_offs_section.push_back((uint8_t)((o >> (i * 8)) & 0xFF));
        }

        // first_char_idx: offsets[C+1] + word_ids[total]
        // For each char_id, collect word_ids from flat_order whose first char_id matches
        uint32_t C = (uint32_t)chars.size();
        std::vector<std::vector<uint32_t>> by_first(C);
        for (uint32_t wid = 0; wid < flat_order.size(); ++wid) {
            const auto& w = words[flat_order[wid]];
            if (!w.char_ids.empty() && w.char_ids[0] < C)
                by_first[w.char_ids[0]].push_back(wid);
        }
        std::vector<uint8_t> first_section;
        std::vector<uint32_t> first_offsets(C + 1, 0);
        for (uint32_t i = 0; i < C; ++i)
            first_offsets[i + 1] = first_offsets[i] + (uint32_t)by_first[i].size();
        for (auto off : first_offsets) {
            for (int i = 0; i < 4; ++i)
                first_section.push_back((uint8_t)((off >> (i * 8)) & 0xFF));
        }
        for (uint32_t i = 0; i < C; ++i)
            for (uint32_t wid : by_first[i])
                for (int k = 0; k < 4; ++k)
                    first_section.push_back((uint8_t)((wid >> (k * 8)) & 0xFF));

        // key_to_char_idx: offsets[25] + char_ids[total]
        // For each phoneme key byte (0x20..0x37), list of char_ids whose first reading's
        // first byte matches.
        constexpr int kKeySlots = 24;
        constexpr uint8_t kKeyMin = 0x20;
        std::vector<std::vector<uint16_t>> by_key(kKeySlots);
        for (uint32_t cid = 0; cid < C; ++cid) {
            const auto& ch = chars[cid];
            std::vector<uint8_t> seen_first;
            for (const auto& r : ch.readings) {
                if (!r.keyseq.empty()) {
                    uint8_t fb = r.keyseq[0];
                    if (std::find(seen_first.begin(), seen_first.end(), fb)
                            == seen_first.end()) {
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
        for (auto off : key_offsets) {
            for (int i = 0; i < 4; ++i)
                key_section.push_back((uint8_t)((off >> (i * 8)) & 0xFF));
        }
        for (int i = 0; i < kKeySlots; ++i)
            for (uint16_t cid : by_key[i]) {
                key_section.push_back((uint8_t)(cid & 0xFF));
                key_section.push_back((uint8_t)(cid >> 8));
            }

        // Header
        constexpr uint32_t kHeaderSize = 0x30;
        uint32_t char_off       = kHeaderSize;
        uint32_t word_off       = char_off + (uint32_t)char_section.size();
        uint32_t first_off      = word_off + (uint32_t)word_section.size();
        uint32_t key_off        = first_off + (uint32_t)first_section.size();
        uint32_t char_offs_off  = key_off + (uint32_t)key_section.size();
        uint32_t word_offs_off  = char_offs_off + (uint32_t)char_offs_section.size();
        uint32_t total_size     = word_offs_off + (uint32_t)word_offs_section.size();

        std::vector<uint8_t> header(kHeaderSize, 0);
        const char* magic = "MIE4";
        std::memcpy(header.data(), magic, 4);
        uint16_t ver = 4;
        uint16_t flg = include_phoneme_pos ? 0x0001 : 0;
        std::memcpy(header.data() + 4, &ver, 2);
        std::memcpy(header.data() + 6, &flg, 2);
        uint32_t C32 = C, W32 = (uint32_t)flat_order.size();
        std::memcpy(header.data() +  8, &C32, 4);
        std::memcpy(header.data() + 12, &W32, 4);
        std::memcpy(header.data() + 16, &char_off, 4);
        std::memcpy(header.data() + 20, &word_off, 4);
        std::memcpy(header.data() + 24, &first_off, 4);
        std::memcpy(header.data() + 28, &key_off, 4);
        std::memcpy(header.data() + 0x20, &total_size, 4);
        std::memcpy(header.data() + 0x24, &char_offs_off, 4);
        std::memcpy(header.data() + 0x28, &word_offs_off, 4);

        std::vector<uint8_t> out;
        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), char_section.begin(), char_section.end());
        out.insert(out.end(), word_section.begin(), word_section.end());
        out.insert(out.end(), first_section.begin(), first_section.end());
        out.insert(out.end(), key_section.begin(), key_section.end());
        out.insert(out.end(), char_offs_section.begin(), char_offs_section.end());
        out.insert(out.end(), word_offs_section.begin(), word_offs_section.end());
        return out;
    }
};

// Phoneme key byte helpers mirror the PHONEME_TO_KEY layout in gen_dict.py.
// Kept local to the test to avoid pulling in gen_dict.py logic.
static inline uint8_t key_b(int idx) { return (uint8_t)(idx + 0x21); }

// Helper to make a single-syllable reading from key indices.
static TinyV4Builder::Reading make_reading(std::initializer_list<int> idxs,
                                            uint8_t tone, uint16_t freq) {
    TinyV4Builder::Reading r;
    for (int i : idxs) r.keyseq.push_back(key_b(i));
    r.tone = tone;
    r.freq = freq;
    return r;
}

} // namespace

// ── Loader tests ──────────────────────────────────────────────────────────

TEST(CompositionSearcher, LoadValidMinimal) {
    TinyV4Builder b;
    // One char ("好" = ㄏㄠˇ = keys [16, 14, ˇ_slot]). For the test,
    // just use key bytes 16/14/1 (ˇ is on slot 1 in the keymap).
    b.chars.push_back({"好", {make_reading({16, 14, 1}, 3, 1000)}});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));
    EXPECT_TRUE(cs.is_loaded());
    EXPECT_EQ(cs.char_count(), 1u);
    EXPECT_EQ(cs.word_count(), 0u);
    EXPECT_EQ(cs.version(), 4);
}

TEST(CompositionSearcher, LoadRejectsBadMagic) {
    std::vector<uint8_t> bad(0x30, 0);
    bad[0] = 'M'; bad[1] = 'I'; bad[2] = 'E'; bad[3] = '2';  // wrong magic
    mie::CompositionSearcher cs;
    EXPECT_FALSE(cs.load_from_memory(bad.data(), bad.size()));
    EXPECT_FALSE(cs.is_loaded());
}

TEST(CompositionSearcher, LoadRejectsTooShort) {
    std::vector<uint8_t> tiny(8, 0);  // smaller than header
    mie::CompositionSearcher cs;
    EXPECT_FALSE(cs.load_from_memory(tiny.data(), tiny.size()));
}

// ── Single-char search tests ──────────────────────────────────────────────

TEST(CompositionSearcher, SearchSingleChar_FullReadingMatch) {
    TinyV4Builder b;
    // 好 with reading [ㄏ ㄠ ˇ] — key bytes per gen_dict.py keymap
    // ㄏ is on slot 16 -> key byte 0x31
    // ㄠ is on slot 14 -> key byte 0x2F
    // ˇ  is on slot 1  -> key byte 0x22
    b.chars.push_back({"好", {make_reading({16, 14, 1}, 3, 1000)}});
    b.words.push_back({{0, 0}, {0, 0}, 500});  // "好好" reduplication
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    uint8_t user_keys[] = {key_b(16), key_b(14), key_b(1)};  // ㄏㄠˇ
    int n = cs.search(user_keys, 3, /*target=*/1, out, 5);
    // target=1 routes through search_chars(): walks char_table directly.
    // 好 has reading [16, 14, 1] matching the user prefix exactly.
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "好");

    // With target=2, should match "好好"
    n = cs.search(user_keys, 3, /*target=*/2, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "好好");
    EXPECT_EQ(out[0].freq, 500);
    EXPECT_EQ(out[0].tone, 3);
}

TEST(CompositionSearcher, SearchAbbreviation_FirstKeyOnly) {
    TinyV4Builder b;
    // 好 [ㄏ ㄠ ˇ], 人 [ㄖ ㄣ ˊ]
    // ㄖ on slot 17 -> 0x32, ㄣ on slot 9 -> 0x2A, ˊ on slot 2 -> 0x23 (tone 2 marker)
    b.chars.push_back({"好", {make_reading({16, 14, 1}, 3, 1000)}});
    b.chars.push_back({"人", {make_reading({17, 9, 2}, 2, 900)}});
    b.words.push_back({{0, 1}, {0, 0}, 750});  // "好人"
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    // Type just ㄏ + ㄖ (initials only for 好人)
    mie::Candidate out[5];
    uint8_t user_keys[] = {key_b(16), key_b(17)};
    int n = cs.search(user_keys, 2, /*target=*/2, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "好人");
}

TEST(CompositionSearcher, SearchEmptyOrBadInputReturnsZero) {
    TinyV4Builder b;
    b.chars.push_back({"好", {make_reading({16, 14, 1}, 3, 1000)}});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    EXPECT_EQ(cs.search(nullptr, 0, 0, out, 5), 0);
    uint8_t k[] = {0xFF};  // out-of-range key byte
    EXPECT_EQ(cs.search(k, 1, 0, out, 5), 0);
}

TEST(CompositionSearcher, SearchGroupFilter) {
    TinyV4Builder b;
    b.chars.push_back({"好", {make_reading({16, 14, 1}, 3, 1000)}});
    b.chars.push_back({"人", {make_reading({17, 9, 2}, 2, 900)}});
    // 2-char "好人", 3-char "好人好"
    b.words.push_back({{0, 1}, {0, 0}, 800});
    b.words.push_back({{0, 1, 0}, {0, 0, 0}, 600});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    uint8_t user_keys[] = {key_b(16), key_b(17)};  // ㄏ-ㄖ abbrev

    // target=2 -> only "好人"
    int n = cs.search(user_keys, 2, 2, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "好人");

    // target=3 -> only "好人好" (still matches because ㄏ-ㄖ is prefix)
    n = cs.search(user_keys, 2, 3, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "好人好");

    // target=0 -> both, freq desc
    n = cs.search(user_keys, 2, 0, out, 5);
    EXPECT_EQ(n, 2);
    EXPECT_STREQ(out[0].word, "好人");       // freq 800
    EXPECT_STREQ(out[1].word, "好人好");     // freq 600
}

TEST(CompositionSearcher, SearchTone1_TrailingSpaceRequiresTone1) {
    // Regression: user types phonemes + SPACE (explicit tone-1 marker).
    // Tone-1 readings have no tone byte in their keyseq, so a naive prefix
    // match with the space included would miss them. The fix strips the
    // trailing space and filters by reading.tone == 1.
    //
    // 八 has reading [ㄅ(0) ㄚ(3)] with tone 1 (no tone byte).
    // 拔 has reading [ㄅ(0) ㄚ(3) ˊ(2)] with tone 2.
    TinyV4Builder b;
    b.chars.push_back({"八", {make_reading({0, 3}, 1, 500)}});
    b.chars.push_back({"拔", {make_reading({0, 3, 2}, 2, 300)}});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    // Without space: both tone-1 八 and tone-2 拔 match the ㄅㄚ prefix.
    uint8_t no_space[] = {key_b(0), key_b(3)};
    int n = cs.search(no_space, 2, /*target=*/1, out, 5);
    EXPECT_EQ(n, 2);

    // With trailing space: only tone-1 八 should match.
    uint8_t with_space[] = {key_b(0), key_b(3), 0x20};
    n = cs.search(with_space, 3, /*target=*/1, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "八");
    EXPECT_EQ(out[0].tone, 1);
}

TEST(CompositionSearcher, SearchTone1_SpaceBetweenSyllables) {
    // Regression: mid-word space after a tone-1 syllable must be consumed
    // by composition_recurse so the next char can start cleanly.
    //
    // Word 八樂 = [八 (tone 1, no tone byte), 樂 (ㄌㄜˋ, tone 4)].
    TinyV4Builder b;
    b.chars.push_back({"八", {make_reading({0, 3}, 1, 500)}});
    b.chars.push_back({"樂", {make_reading({15, 13, 1}, 4, 900)}});
    b.words.push_back({{0, 1}, {0, 0}, 100});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    // User types ㄅㄚ + SPACE + ㄌㄜˋ
    uint8_t user_keys[] = {
        key_b(0), key_b(3), 0x20,
        key_b(15), key_b(13), key_b(1),
    };
    mie::Candidate out[5];
    int n = cs.search(user_keys, 6, /*target=*/2, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "八樂");
}

TEST(CompositionSearcher, SearchToneStripped_MidWordFullSyllable) {
    // Regression for "快樂 not found when typing ㄎㄨㄞㄌ".
    // First char (快) has a 4-byte reading (I+M+F+T); user types only the
    // 3 phoneme bytes (no tone) and then starts the next syllable with just
    // its initial. composition_recurse must try klen-1 as a prefix length.
    TinyV4Builder b;
    // 快 = [ㄎ(11) ㄨ(13) ㄞ(4) ˋ(tone-mark slot 1)], tone 4
    b.chars.push_back({"快", {make_reading({11, 13, 4, 1}, 4, 1000)}});
    // 樂 = [ㄌ(15) ㄜ(13) ˋ(1)], tone 4
    b.chars.push_back({"樂", {make_reading({15, 13, 1}, 4, 900)}});
    // 快樂 word
    b.words.push_back({{0, 1}, {0, 0}, 750});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    // User types ㄎ ㄨ ㄞ ㄌ (no tone keys pressed).
    uint8_t user_keys[] = {key_b(11), key_b(13), key_b(4), key_b(15)};
    int n = cs.search(user_keys, 4, /*target=*/2, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "快樂");
}

TEST(CompositionSearcher, ReadingOverride_PolyphonicChar) {
    // Test 多音字: char with multiple readings, word uses non-default reading.
    TinyV4Builder b;
    // 行 has 2 readings: [ㄒㄧㄥˊ] (primary) and [ㄏㄤˊ] (secondary)
    // ㄒ slot 16 tone 2 keybyte -> different from ㄏ
    // Simplified to avoid real bopomofo complexity: use fake key slots.
    TinyV4Builder::Reading r_primary   = make_reading({16, 8, 2}, 2, 900);
    TinyV4Builder::Reading r_secondary = make_reading({16, 14, 2}, 2, 100);
    b.chars.push_back({"行", {r_primary, r_secondary}});
    b.chars.push_back({"人", {make_reading({17, 9, 2}, 2, 900)}});

    // "行人" uses primary (ridx=0,0) -> ㄒㄧㄥˊ-ㄖㄣˊ
    b.words.push_back({{0, 1}, {0, 0}, 700});
    // "銀行" uses secondary for 行: fake 銀=char 2
    b.chars.push_back({"銀", {make_reading({15, 13, 2}, 2, 500)}});
    // "銀行" = [銀, 行] with ridx [0, 1]
    b.words.push_back({{2, 0}, {0, 1}, 800});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    // Type ㄒㄧㄥˊ-ㄖㄣˊ full (行人)
    uint8_t k_xing_ren[] = {key_b(16), key_b(8), key_b(2),
                             key_b(17), key_b(9), key_b(2)};
    int n = cs.search(k_xing_ren, 6, 2, out, 5);
    EXPECT_GE(n, 1);
    bool found_xingren = false;
    for (int i = 0; i < n; ++i)
        if (std::strcmp(out[i].word, "行人") == 0) found_xingren = true;
    EXPECT_TRUE(found_xingren);

    // Type 銀行 full (using 行 secondary reading): 銀=[15,13,2], 行=[16,14,2]
    uint8_t k_yinhang[] = {key_b(15), key_b(13), key_b(2),
                            key_b(16), key_b(14), key_b(2)};
    n = cs.search(k_yinhang, 6, 2, out, 5);
    bool found_yinhang = false;
    for (int i = 0; i < n; ++i)
        if (std::strcmp(out[i].word, "銀行") == 0) found_yinhang = true;
    EXPECT_TRUE(found_yinhang);
}

// ── Phase 1.4 long-press phoneme-position filter tests ──────────────────
//
// The dict now carries a per-byte "which phoneme of the half-key produced
// this byte" index. User input can pair each byte with a hint (0 = primary
// only, 1 = secondary only, 0xFF = any) and the searcher rejects readings
// whose authored phoneme doesn't match. Expected outcome: on a key that
// collides two phonemes (e.g. slot 0 = ㄅ/ㄉ), typing with hint=1 prunes
// all primary matches; with hint=0 prunes all secondary.

TEST(CompositionSearcher, PhonemePos_HasPhonemePosFlagWhenFlagSet) {
    TinyV4Builder b;
    b.include_phoneme_pos = true;
    // 八 (ㄅㄚ tone 1) → kbytes [0x21, 0x24], pos [0, 1]
    b.chars.push_back({"八",
                       {{{0x21, 0x24}, 1, 500, {0, 1}}}});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));
    EXPECT_TRUE(cs.has_phoneme_pos());
}

TEST(CompositionSearcher, PhonemePos_NoFlagWhenLegacyBlob) {
    TinyV4Builder b;
    // include_phoneme_pos stays false (default)
    b.chars.push_back({"八", {make_reading({0, 3}, 1, 500)}});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));
    EXPECT_FALSE(cs.has_phoneme_pos());

    // Hints on a legacy blob are silently ignored — the search returns the
    // same results as the no-hint path.
    mie::Candidate out[5];
    uint8_t user_keys[]  = {key_b(0), key_b(3)};
    uint8_t bad_hints[]  = {1, 1};   // demand ㄉ + ㄢ (neither produced 八)
    int n_no_hint = cs.search(user_keys, nullptr, 2, /*target=*/1, out, 5);
    int n_hints   = cs.search(user_keys, bad_hints, 2, /*target=*/1, out, 5);
    EXPECT_EQ(n_no_hint, 1);
    EXPECT_EQ(n_hints,   1);   // legacy blob: hints ignored
}

TEST(CompositionSearcher, PhonemePos_FilterSingleCharByPrimary) {
    // 八 (ㄅㄚ) → byte [0x21, 0x24], phoneme_pos [0, 1]
    //   ㄅ is primary of slot 0, ㄚ is secondary of slot 3.
    // 搭 (ㄉㄚ) → same bytes, phoneme_pos [1, 1]
    //   ㄉ is secondary of slot 0, ㄚ is secondary of slot 3.
    TinyV4Builder b;
    b.include_phoneme_pos = true;
    b.chars.push_back({"八",
                       {{{0x21, 0x24}, 1, 500, {0, 1}}}});
    b.chars.push_back({"搭",
                       {{{0x21, 0x24}, 1, 300, {1, 1}}}});
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    uint8_t keys[]       = {0x21, 0x24};

    // No hints: both chars match.
    int n = cs.search(keys, nullptr, 2, /*target=*/1, out, 5);
    EXPECT_EQ(n, 2);

    // Primary-only on byte 0 (ㄅ): only 八 remains.
    uint8_t hint_primary[]   = {0, 0xFFu};
    n = cs.search(keys, hint_primary, 2, /*target=*/1, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "八");

    // Secondary-only on byte 0 (ㄉ): only 搭 remains.
    uint8_t hint_secondary[] = {1, 0xFFu};
    n = cs.search(keys, hint_secondary, 2, /*target=*/1, out, 5);
    EXPECT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "搭");
}

TEST(CompositionSearcher, PhonemePos_FilterMultiCharByPrimary) {
    // Construct two 2-char words that collide on the same key bytes but
    // were authored from different phonemes of slot 0.
    //   八爸: 八 + 爸  (ㄅㄚ + ㄅㄚˋ)  both initials = ㄅ (primary)
    //   大霸: 大 + 霸  (ㄉㄚˋ + ㄅㄚˋ)  slot-0 initial on first char = ㄉ (secondary)
    // User types [0x21, 0x24, 0x21, 0x24]; hint requiring secondary on
    // byte 0 and primary on byte 2 should drop 八爸 and keep 大霸.
    TinyV4Builder b;
    b.include_phoneme_pos = true;
    // 八  (ㄅㄚ tone1)   pos [0,1]
    // 爸  (ㄅㄚˋ tone4)  pos [0,1,1]
    // 大  (ㄉㄚˋ tone4)  pos [1,1,1]
    // 霸  (ㄅㄚˋ tone4)  pos [0,1,1]
    b.chars.push_back({"八",
                       {{{0x21, 0x24}, 1, 500, {0, 1}}}});
    b.chars.push_back({"爸",
                       {{{0x21, 0x24, 0x22}, 4, 400, {0, 1, 1}}}});
    b.chars.push_back({"大",
                       {{{0x21, 0x24, 0x22}, 4, 700, {1, 1, 1}}}});
    b.chars.push_back({"霸",
                       {{{0x21, 0x24, 0x22}, 4, 350, {0, 1, 1}}}});
    // 2-char words (full-key match, not abbreviation).
    // 八爸: char_ids [0,1], full keyseq = [0x21,0x24]+[0x21,0x24,0x22]
    // 大霸: char_ids [2,3], full keyseq = [0x21,0x24,0x22]+[0x21,0x24,0x22]
    b.words.push_back({{0, 1}, {0, 0}, 600});   // 八爸
    b.words.push_back({{2, 3}, {0, 0}, 650});   // 大霸
    auto blob = b.build();

    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];

    // No-hint abbreviated search on ㄅ+ㄅ-like initials: matches first two
    // bytes of each char's full reading, which for 大 is ㄉㄚ — same byte
    // pattern [0x21,0x24]. Both words surface.
    uint8_t keys[]    = {0x21, 0x24, 0x21, 0x24};
    int n = cs.search(keys, nullptr, 4, /*target=*/2, out, 5);
    EXPECT_EQ(n, 2);

    // Demand ㄉ on byte 0 (secondary), ㄅ on byte 2 (primary) — 大霸 only.
    uint8_t hint[]    = {1, 0xFFu, 0, 0xFFu};
    n = cs.search(keys, hint, 4, /*target=*/2, out, 5);
    ASSERT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "大霸");

    // Demand ㄅ on byte 0 (primary) — 八爸 only.
    uint8_t hint2[]   = {0, 0xFFu, 0xFFu, 0xFFu};
    n = cs.search(keys, hint2, 4, /*target=*/2, out, 5);
    ASSERT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "八爸");
}

TEST(CompositionSearcher, PhonemePos_AnySentinelEquivalentToNoHints) {
    TinyV4Builder b;
    b.include_phoneme_pos = true;
    b.chars.push_back({"八",
                       {{{0x21, 0x24}, 1, 500, {0, 1}}}});
    b.chars.push_back({"搭",
                       {{{0x21, 0x24}, 1, 300, {1, 1}}}});
    auto blob = b.build();
    mie::CompositionSearcher cs;
    ASSERT_TRUE(cs.load_from_memory(blob.data(), blob.size()));

    mie::Candidate out[5];
    uint8_t keys[]    = {0x21, 0x24};
    uint8_t all_any[] = {0xFFu, 0xFFu};
    int n_any = cs.search(keys, all_any, 2, /*target=*/1, out, 5);
    int n_non = cs.search(keys, nullptr, 2, /*target=*/1, out, 5);
    EXPECT_EQ(n_any, n_non);
    EXPECT_EQ(n_any, 2);
}
