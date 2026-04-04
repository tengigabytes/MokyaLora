// trie_searcher.h — MokyaInput Engine Trie-Searcher public API
// SPDX-License-Identifier: MIT
//
// Looks up a Bopomofo phoneme sequence in the compiled dictionary and returns
// frequency-ranked candidate words.
//
// Phase 1 implementation: sorted-key binary search on the MIED binary format.
// Phase 2: replace internals with a Double-Array Trie for PSRAM efficiency;
//          the public API and binary format header remain unchanged.
//
// ── Binary format: dict_dat.bin ──────────────────────────────────────────
//
//  Header (16 bytes, all fields little-endian):
//    Offset  Size  Field
//     0       4    magic         = "MIED"
//     4       2    version       = 1
//     6       2    flags         = 0
//     8       4    key_count     — number of unique Bopomofo keys
//    12       4    keys_data_off — byte offset to keys-data section
//
//  Index table (key_count × 8 bytes, starts at offset 16):
//    Offset  Size  Field
//     0       4    key_data_off  — offset of this key within keys-data section
//     4       4    val_data_off  — byte offset in dict_values.bin
//
//  Keys-data section (at keys_data_off, variable-length records, sorted lex.):
//    Offset  Size  Field
//     0       1    key_len   — byte length of UTF-8 Bopomofo key
//     1      N     key_utf8  — key_len bytes (NOT null-terminated)
//
// ── Binary format: dict_values.bin ───────────────────────────────────────
//
//  ValueRecord at val_data_off:
//    Offset  Size  Field
//     0       2    word_count
//    Per word (sorted by freq descending):
//     0       2    freq
//     2       1    word_len
//     3      N     word_utf8  — word_len bytes (NOT null-terminated)

#pragma once
#include <cstddef>
#include <cstdint>

namespace mie {

/// Maximum UTF-8 byte length of a candidate word including the null terminator.
/// Supports up to 10 CJK characters (3 bytes each) + '\0'.
static constexpr int kCandidateMaxBytes = 32;

/// A single search result returned by TrieSearcher::search().
struct Candidate {
    char     word[kCandidateMaxBytes]; ///< UTF-8, null-terminated
    uint16_t freq;                      ///< Frequency weight (higher = more common)
    uint8_t  tone;                      ///< Bopomofo tone 1-5; 0 = unknown/unspecified
};

/// Trie-Searcher: looks up a Bopomofo phoneme sequence in the compiled
/// dictionary and returns candidates sorted by frequency (highest first).
///
/// Thread safety: all public methods are read-only after load_from_*();
/// concurrent reads from multiple threads are safe without locking.
class TrieSearcher {
public:
    TrieSearcher()  = default;
    ~TrieSearcher();

    TrieSearcher(const TrieSearcher&)            = delete;
    TrieSearcher& operator=(const TrieSearcher&) = delete;

    /// Load dictionary from files on disk (PC / host mode).
    /// @param dat_path  Path to dict_dat.bin
    /// @param val_path  Path to dict_values.bin
    /// @return true on success; false if files cannot be read or are malformed.
    bool load_from_file(const char* dat_path, const char* val_path);

    /// Load dictionary from in-memory buffers (embedded / PSRAM mode).
    /// The buffers must remain valid and unmodified for the lifetime of this
    /// object.  No internal copy is made.
    /// @param dat_buf   Pointer to dict_dat.bin data
    /// @param dat_size  Byte length of dat_buf
    /// @param val_buf   Pointer to dict_values.bin data
    /// @param val_size  Byte length of val_buf
    /// @return true on success; false if the header is malformed.
    bool load_from_memory(const uint8_t* dat_buf, size_t dat_size,
                          const uint8_t* val_buf, size_t val_size);

    /// Search for candidates matching a Bopomofo phoneme sequence.
    /// @param key_utf8    Null-terminated UTF-8 Bopomofo key (e.g. "ㄐㄧㄣ").
    /// @param out         Caller-supplied output array.
    /// @param max_results Capacity of out[]; at most this many entries written.
    /// @return Number of candidates written (0 if key not found or not loaded).
    int search(const char* key_utf8, Candidate* out, int max_results) const;

    bool     is_loaded()  const { return loaded_; }
    uint32_t key_count()  const { return key_count_; }
    uint16_t dict_version() const { return version_; }

private:
    // Heap-owned buffers used when loading from files (nullptr in memory mode).
    uint8_t* owned_dat_ = nullptr;
    uint8_t* owned_val_ = nullptr;

    // Active working pointers (may alias owned_* or external buffers).
    const uint8_t* dat_      = nullptr;
    size_t         dat_size_ = 0;
    const uint8_t* val_      = nullptr;
    size_t         val_size_ = 0;

    uint32_t key_count_     = 0;
    uint32_t keys_data_off_ = 0;
    uint16_t version_       = 0;   ///< Dict version: 1 = no tone, 2 = tone byte per word
    bool     loaded_        = false;

    /// Compare the query string with the stored key at sorted index position idx.
    /// Returns <0, 0, or >0 (same semantics as strcmp / memcmp).
    int compare_key(uint32_t idx, const char* query) const;

    /// Return the val_data_off field from index entry idx.
    uint32_t get_val_off(uint32_t idx) const;
};

} // namespace mie
