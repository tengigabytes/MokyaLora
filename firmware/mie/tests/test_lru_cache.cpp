// test_lru_cache.cpp — Unit tests for LruCache (Phase 1.6).
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <mie/lru_cache.h>

#include <cstring>
#include <vector>

using mie::Candidate;
using mie::LruCache;
using mie::LruEntry;
using mie::kLruPackedPositions;
using mie::lru_pack_phoneme_hints;
using mie::lru_unpack_phoneme_hint;

namespace {

// Helper: build a kbytes array from a brace-initialised list.
struct Keys {
    uint8_t bytes[8];
    int     len;
    Keys(std::initializer_list<uint8_t> init) : len((int)init.size()) {
        int i = 0;
        for (uint8_t b : init) bytes[i++] = b;
        for (; i < 8; ++i) bytes[i] = 0;
    }
};

// Convenience: upsert with an inline Keys + all-ANY phoneme hints.
void upsert_any(LruCache& c, const Keys& k, const char* utf8, uint32_t now_ms,
                uint8_t tone = 0) {
    c.upsert(k.bytes, k.len,
             lru_pack_phoneme_hints(nullptr, 0),
             tone, utf8, now_ms);
}

// Fixed "any" hint array, long enough for any test.
uint8_t g_any[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

} // namespace

TEST(LruPackHints, PrimaryOnly) {
    uint8_t hints[4] = { 0, 0, 0, 0 };
    uint8_t p = lru_pack_phoneme_hints(hints, 4);
    EXPECT_EQ(p, 0x00);
    for (int k = 0; k < 4; ++k)
        EXPECT_EQ(lru_unpack_phoneme_hint(p, k), 0u);
}

TEST(LruPackHints, MixedRoundTrip) {
    uint8_t hints[4] = { 0, 1, 2, 0xFF };
    uint8_t p = lru_pack_phoneme_hints(hints, 4);
    EXPECT_EQ(lru_unpack_phoneme_hint(p, 0), 0u);
    EXPECT_EQ(lru_unpack_phoneme_hint(p, 1), 1u);
    EXPECT_EQ(lru_unpack_phoneme_hint(p, 2), 2u);
    EXPECT_EQ(lru_unpack_phoneme_hint(p, 3), 0xFFu);
    // Beyond packed range always returns ANY.
    EXPECT_EQ(lru_unpack_phoneme_hint(p, 5), 0xFFu);
}

TEST(LruCacheBasic, EmptyLookupReturnsZero) {
    LruCache c;
    Candidate out[8];
    Keys k{0x21, 0x28};
    EXPECT_EQ(c.lookup(k.bytes, k.len, g_any, out, 8), 0);
    EXPECT_EQ(c.count(), 0);
}

TEST(LruCacheBasic, UpsertThenLookupExact) {
    LruCache c;
    Keys k{0x21, 0x28};
    upsert_any(c, k, "吸", 1000);
    EXPECT_EQ(c.count(), 1);

    Candidate out[8];
    int n = c.lookup(k.bytes, k.len, g_any, out, 8);
    ASSERT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "吸");
}

TEST(LruCacheBasic, UpsertSameTripleBumpsUseCount) {
    LruCache c;
    Keys k{0x21, 0x28};
    upsert_any(c, k, "吸", 1000);
    upsert_any(c, k, "吸", 2000);
    EXPECT_EQ(c.count(), 1);
    EXPECT_EQ(c.entry(0).use_count, 2);
    EXPECT_EQ(c.entry(0).last_used_ms, 2000u);
}

TEST(LruCacheBasic, DifferentUtf8SameKeyAreSeparate) {
    // Homophones should legitimately coexist under the same reading.
    LruCache c;
    Keys k{0x21, 0x28};
    upsert_any(c, k, "吸", 1000);
    upsert_any(c, k, "希", 2000);
    EXPECT_EQ(c.count(), 2);

    Candidate out[8];
    int n = c.lookup(k.bytes, k.len, g_any, out, 8);
    ASSERT_EQ(n, 2);
    // Most-recent first.
    EXPECT_STREQ(out[0].word, "希");
    EXPECT_STREQ(out[1].word, "吸");
}

TEST(LruCacheEviction, EvictsOldestWhenFull) {
    LruCache c;
    Keys base{0x21, 0x28};

    // Fill to capacity with synthetic distinct utf8 strings.
    char buf[8];
    for (int i = 0; i < LruCache::kCap; ++i) {
        snprintf(buf, sizeof(buf), "W%04d", i);
        upsert_any(c, base, buf, 1000 + i);
    }
    EXPECT_EQ(c.count(), LruCache::kCap);

    // Oldest entry is W0000 @ 1000. Add a new entry and expect W0000 gone.
    upsert_any(c, base, "NEW", 9999);
    EXPECT_EQ(c.count(), LruCache::kCap);

    Candidate out[LruCache::kCap];
    int n = c.lookup(base.bytes, base.len, g_any, out, LruCache::kCap);
    bool saw_new = false, saw_zero = false;
    for (int i = 0; i < n; ++i) {
        if (strcmp(out[i].word, "NEW")    == 0) saw_new  = true;
        if (strcmp(out[i].word, "W0000")  == 0) saw_zero = true;
    }
    EXPECT_TRUE(saw_new);
    EXPECT_FALSE(saw_zero);
}

TEST(LruCachePrefix, PrefixMatchSurfacesShorterEntry) {
    LruCache c;
    Keys stored{0x21, 0x28};                    // 2-byte reading
    upsert_any(c, stored, "吸", 1000);

    Keys user{0x21, 0x28, 0x22};                // user typed stored + tone byte
    Candidate out[8];
    int n = c.lookup(user.bytes, user.len, g_any, out, 8);
    ASSERT_EQ(n, 1);
    EXPECT_STREQ(out[0].word, "吸");
}

TEST(LruCachePrefix, LengthOneEntriesDoNotMatchAsPrefix) {
    LruCache c;
    Keys stored{0x21};                          // single-byte "ㄅ"
    upsert_any(c, stored, "不", 1000);

    // Exact match on the same single byte still works.
    Candidate out[8];
    int n_exact = c.lookup(stored.bytes, 1, g_any, out, 8);
    EXPECT_EQ(n_exact, 1);

    // But a longer input must NOT pick the len-1 entry as a prefix hit.
    Keys longer{0x21, 0x28};
    int n_pref = c.lookup(longer.bytes, longer.len, g_any, out, 8);
    EXPECT_EQ(n_pref, 0);
}

TEST(LruCacheHints, WrongHintRejected) {
    LruCache c;
    Keys k{0x21, 0x28};
    // Store with hint[0] = primary (0), hint[1] = secondary (1).
    uint8_t stored_hints[2] = { 0, 1 };
    c.upsert(k.bytes, k.len,
             lru_pack_phoneme_hints(stored_hints, 2),
             /*tone=*/1, "希", 1000);

    Candidate out[8];

    // Query with hints that match → returned.
    uint8_t q_match[2] = { 0, 1 };
    EXPECT_EQ(c.lookup(k.bytes, k.len, q_match, out, 8), 1);

    // Query with a conflicting hint at position 1 → rejected.
    uint8_t q_bad[2] = { 0, 0 };
    EXPECT_EQ(c.lookup(k.bytes, k.len, q_bad, out, 8), 0);

    // Query with ANY hint → matches.
    uint8_t q_any[2] = { 0xFF, 0xFF };
    EXPECT_EQ(c.lookup(k.bytes, k.len, q_any, out, 8), 1);
}

TEST(LruCacheHints, StoredAnyMatchesAnyQuery) {
    // Legacy entries saved with all-ANY hints must still surface when the
    // user now provides a concrete hint (backward-compat).
    LruCache c;
    Keys k{0x21, 0x28};
    upsert_any(c, k, "吸", 1000);   // pos_packed == 0xFF (all ANY)

    Candidate out[8];
    uint8_t q[2] = { 0, 1 };
    EXPECT_EQ(c.lookup(k.bytes, k.len, q, out, 8), 1);
}

TEST(LruCacheSerialize, RoundTripPreservesFields) {
    LruCache a;
    Keys k1{0x21, 0x28};
    Keys k2{0x22, 0x29, 0x22};
    uint8_t hints1[2] = { 0, 1 };
    a.upsert(k1.bytes, k1.len, lru_pack_phoneme_hints(hints1, 2), 1, "希", 1000);
    a.upsert(k2.bytes, k2.len, lru_pack_phoneme_hints(nullptr, 0), 3, "好",  2000);
    // Re-upsert first to bump use_count to 2.
    a.upsert(k1.bytes, k1.len, lru_pack_phoneme_hints(hints1, 2), 1, "希", 1500);

    std::vector<uint8_t> buf(a.serialized_size());
    ASSERT_EQ(a.serialize(buf.data(), (int)buf.size()),
              a.serialized_size());

    LruCache b;
    ASSERT_TRUE(b.deserialize(buf.data(), (int)buf.size()));
    EXPECT_EQ(b.count(), a.count());
    for (int i = 0; i < a.count(); ++i) {
        const LruEntry& ea = a.entry(i);
        const LruEntry& eb = b.entry(i);
        EXPECT_EQ(ea.klen, eb.klen);
        EXPECT_EQ(ea.pos_packed, eb.pos_packed);
        EXPECT_EQ(ea.tone, eb.tone);
        EXPECT_EQ(ea.utf8_len, eb.utf8_len);
        EXPECT_EQ(ea.last_used_ms, eb.last_used_ms);
        EXPECT_EQ(ea.use_count, eb.use_count);
        EXPECT_EQ(memcmp(ea.kbytes, eb.kbytes, ea.klen), 0);
        EXPECT_EQ(memcmp(ea.utf8, eb.utf8, ea.utf8_len), 0);
    }
}

TEST(LruCacheSerialize, RejectsBadMagic) {
    LruCache a;
    Keys k{0x21, 0x28};
    upsert_any(a, k, "吸", 1000);
    std::vector<uint8_t> buf(a.serialized_size());
    a.serialize(buf.data(), (int)buf.size());

    buf[0] = 'X';
    LruCache b;
    EXPECT_FALSE(b.deserialize(buf.data(), (int)buf.size()));
    EXPECT_EQ(b.count(), 0);
}

TEST(LruCacheSerialize, RejectsWrongVersion) {
    LruCache a;
    Keys k{0x21, 0x28};
    upsert_any(a, k, "吸", 1000);
    std::vector<uint8_t> buf(a.serialized_size());
    a.serialize(buf.data(), (int)buf.size());

    buf[4] = 0x02;   // version bump to 2
    LruCache b;
    EXPECT_FALSE(b.deserialize(buf.data(), (int)buf.size()));
}

TEST(LruCacheSerialize, RejectsTruncatedBuffer) {
    LruCache a;
    Keys k{0x21, 0x28};
    upsert_any(a, k, "吸", 1000);
    std::vector<uint8_t> buf(a.serialized_size());
    a.serialize(buf.data(), (int)buf.size());

    LruCache b;
    EXPECT_FALSE(b.deserialize(buf.data(), (int)buf.size() - 5));
}

TEST(LruCacheLookupOrder, TieBreakOnUseCount) {
    // Two entries with the SAME last_used_ms; the one with higher use_count
    // must come first.
    LruCache c;
    Keys k{0x21, 0x28};
    upsert_any(c, k, "A", 1000);
    upsert_any(c, k, "B", 1000);
    // Bump A's use_count without changing its timestamp.
    // upsert with the same last_used_ms should keep ms but increment count.
    upsert_any(c, k, "A", 1000);

    Candidate out[8];
    int n = c.lookup(k.bytes, k.len, g_any, out, 8);
    ASSERT_EQ(n, 2);
    EXPECT_STREQ(out[0].word, "A");
    EXPECT_STREQ(out[1].word, "B");
}
