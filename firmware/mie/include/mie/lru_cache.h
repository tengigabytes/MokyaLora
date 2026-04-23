// SPDX-License-Identifier: MIT
// MokyaInput Engine — Personalised LRU cache (Phase 1.6)
//
// Caches recently committed (reading, phoneme-position, utf8) triples so a
// repeat-typer's second use of a rare character / name surfaces near rank 0.
// Lives in the MIE library so the host REPL and unit tests share the same
// implementation as Core 1 firmware; persistence glue (LittleFS) is added
// separately by the Core 1 side.
//
// Heap-free: all state is embedded in a fixed-capacity array. sizeof(LruCache)
// is ~6 KB + 8 B for metadata; the entire engine still fits in Core 1's BSS
// budget without touching the FreeRTOS heap.

#pragma once

#include <stdint.h>
#include <mie/trie_searcher.h>  // for Candidate

namespace mie {

// 2-bit packed phoneme-position hint. Packs up to kLruPackedPositions byte
// positions into a single byte (LSB = byte 0). Values:
//   0b00 = primary (hint 0)
//   0b01 = secondary (hint 1)
//   0b10 = tertiary (hint 2)
//   0b11 = any / unspecified (hint 0xFF)
// Byte positions beyond kLruPackedPositions are treated as "any" at match
// time. Matches the dict's phoneme_pos packing (composition_searcher.h).
static constexpr int kLruPackedPositions = 4;

/// Pack a phoneme-hint array (uint8_t values 0/1/2/0xFF) into a single byte.
/// Positions >= kLruPackedPositions are silently dropped (encoded as ANY at
/// match time).
uint8_t lru_pack_phoneme_hints(const uint8_t* hints, int n);

/// Unpack position k (0..7) from pos_packed. Returns 0, 1, 2, or 0xFF (ANY).
/// k >= kLruPackedPositions always returns 0xFF.
uint8_t lru_unpack_phoneme_hint(uint8_t pos_packed, int k);

struct LruEntry {
    uint8_t  kbytes[8];         ///< reading keyseq, LSB-aligned
    uint8_t  klen;              ///< 1..8 active bytes
    uint8_t  pos_packed;        ///< packed 2-bit phoneme positions (see above)
    uint8_t  tone;              ///< 0 = unspecified, 1..5
    uint8_t  utf8_len;          ///< bytes in utf8[]
    char     utf8[24];          ///< committed word, matches Candidate width
    uint32_t last_used_ms;      ///< eviction key
    uint16_t use_count;         ///< weak bonus; breaks last-used ties
    uint16_t reserved0;         ///< pad
    uint32_t reserved1;         ///< pad to 48 B
};
// Pinned size: 48 B per entry. 128 × 48 = 6 144 B RAM.
static_assert(sizeof(LruEntry) == 48, "LruEntry must be 48 bytes for LittleFS persistence");

class LruCache {
public:
    static constexpr int kCap = 64;

    LruCache() { reset(); }

    void reset();
    int  count() const { return count_; }
    int  capacity() const { return kCap; }

    // Read-only peek for tests / persistence iteration.
    const LruEntry& entry(int i) const { return entries_[i]; }

    /// Upsert an exact (kbytes, pos_packed, utf8) triple. Bumps last_used_ms
    /// and use_count on hit; evicts the oldest entry (min last_used_ms,
    /// then min use_count) on miss.
    void upsert(const uint8_t* kbytes, int klen, uint8_t pos_packed,
                uint8_t tone, const char* utf8, uint32_t now_ms);

    /// Populate `out` with entries whose kbytes is a prefix of user_keys and
    /// whose stored pos hints agree with user_phoneme_hints. Entries are
    /// returned in (last_used_ms desc, use_count desc) order, deduped by
    /// utf8 string. Returns the number written (<= max_results).
    ///
    /// Matching rule:
    ///   - Exact: entry.klen == user_n AND entry.kbytes == user_keys[0..klen)
    ///   - Prefix: entry.klen < user_n AND entry.kbytes is a prefix of
    ///             user_keys; entry.klen == 1 is excluded from prefix match
    ///             to avoid a single-ㄅ entry dominating every ㄅ-initial
    ///             input.
    ///   - Phoneme-hint filter: for each byte k < entry.klen with
    ///             user_phoneme_hints[k] != 0xFF, require entry.pos(k) ==
    ///             user_phoneme_hints[k] OR entry.pos(k) == 0xFF.
    ///
    /// user_phoneme_hints may be nullptr (equivalent to all-0xFF).
    /// out_prefix_keys (optional) receives the matched klen per emitted
    /// candidate; the caller uses it for ImeLogic::candidates_prefix_keys_
    /// so a prefix-length LRU hit consumes only its own klen bytes on commit.
    int  lookup(const uint8_t* user_keys, int user_n,
                const uint8_t* user_phoneme_hints,
                Candidate* out, int max_results,
                uint8_t* out_prefix_keys = nullptr) const;

    // ── Persistence ──────────────────────────────────────────────────────
    // Binary layout:
    //   magic     4 B "LRU1"
    //   version   2 B u16 = 1
    //   count     2 B u16
    //   entries   count * 48 B
    static constexpr int kHeaderSize = 8;
    static constexpr uint16_t kSerialVersion = 1;

    int  serialized_size() const { return kHeaderSize + count_ * (int)sizeof(LruEntry); }
    int  serialize(uint8_t* buf, int cap) const;
    bool deserialize(const uint8_t* buf, int len);

private:
    LruEntry entries_[kCap];
    int      count_;

    int  find_exact(const uint8_t* kbytes, int klen,
                    uint8_t pos_packed, const char* utf8) const;
    int  find_evict_victim() const;
};

} // namespace mie
