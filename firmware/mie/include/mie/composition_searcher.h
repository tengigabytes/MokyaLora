// composition_searcher.h - MokyaInput Engine v4 Composition Searcher
// SPDX-License-Identifier: MIT
//
// Looks up user phoneme key sequences in a MIED v4 dict (single binary
// file). Unlike TrieSearcher, the dict does NOT store pre-computed
// abbreviation keys -- each multi-char word is stored as a sequence of
// char_ids + per-char reading_idxs. At search time, composition_matches()
// derives possible key sequences from the chars' readings and matches
// against user input.
//
// This eliminates the abbreviation cartesian explosion that bloated v2
// dict_dat.bin: typing 1-byte first-key-only abbreviation no longer
// requires the dict to enumerate all per-syllable prefix combinations.
//
// Bucket dispatch (1/2/3/4-char + 5+) is implemented via target_char_count
// search filter, NOT separate dict files. ImeLogic's count_positions()
// computes target_char_count and 0-result fallback chain handles edge
// cases.
//
// ── v4 binary file layout (see firmware/mie/tools/gen_dict.py for builder) ──
//
//  Header (0x30 bytes, little-endian):
//    Offset  Size  Field
//     0x00     4   magic = "MIE4"
//     0x04     2   version u16 = 4
//     0x06     2   flags u16
//     0x08     4   char_count u32
//     0x0C     4   word_count u32
//     0x10     4   char_table_off u32
//     0x14     4   word_table_off u32
//     0x18     4   first_char_idx_off u32
//     0x1C     4   key_to_char_idx_off u32
//     0x20     4   total_size u32  (whole file including header)
//     0x24     4   char_offsets_off u32  (pre-computed u32[char_count+1])
//     0x28     4   word_offsets_off u32  (pre-computed u32[word_count+1])
//     0x2C     4   reserved (zero)
//
//  char_table (sorted by char_id 0..char_count-1):
//    Per char:  utf8_len(u8) | utf8 bytes | reading_count(u8)
//                | per reading: klen(u8) | key bytes | tone(u8) | base_freq(u16)
//
//  word_table:
//    8 group headers (char_count 1..8): u32 group_count | u32 start_word_id
//    Then word_count records in group order:
//      char_count(u8) | flags(u8) | freq(u16) | char_id[N](u16 each)
//        | reading_idx[N](u8 each)  IF flags & 1
//
//  first_char_idx:
//    u32 offsets[char_count + 1] (prefix sum)
//    u32 word_ids[total]  (sorted by word.freq desc within each char_id)
//
//  key_to_char_idx:
//    u32 offsets[25]  (one slot per phoneme key byte 0x20..0x37, NUM_SLOTS = 24)
//    u16 char_ids[total]

#pragma once
#include <cstddef>
#include <cstdint>

#include <mie/trie_searcher.h>  // for Candidate + kCandidateMaxBytes

namespace mie {

class CompositionSearcher {
public:
    CompositionSearcher() = default;
    ~CompositionSearcher();

    CompositionSearcher(const CompositionSearcher&)            = delete;
    CompositionSearcher& operator=(const CompositionSearcher&) = delete;

    /// Load a v4 dict from a single binary file (slurps to heap).
    bool load_from_file(const char* path);

    /// Load a v4 dict from an in-memory buffer. The buffer must remain valid
    /// and unmodified for the lifetime of this object; no internal copy is made.
    bool load_from_memory(const uint8_t* buf, size_t size);

    /// Long-press disambiguation hint sentinel: matches any phoneme
    /// position. Pass 0xFF for bytes the user typed without long-press
    /// intent (or where the engine should not filter on phoneme position).
    static constexpr uint8_t kPhonemeHintAny = 0xFF;

    /// Search for words matching the given user phoneme key sequence,
    /// optionally filtered by char_count.
    ///
    /// @param user_keys          Phoneme key bytes (NOT null-terminated).
    /// @param user_n             Length of user_keys in bytes.
    /// @param target_char_count  Word length filter:
    ///                             == 1      : single chars via char_table
    ///                             2..7      : only multi-char words with that count
    ///                             == -1     : long words (char_count >= 5),
    ///                                         i.e. union of buckets 5/6/7/8+
    ///                             == 0      : no filter (any length 2..8+)
    /// @param out                Caller-supplied output array.
    /// @param max_results        Capacity of out[].
    /// @return Number of candidates written, freq-descending. 0 if no match.
    int search(const uint8_t* user_keys, int user_n,
               int target_char_count,
               Candidate* out, int max_results) const;

    /// Variant that accepts a per-byte phoneme-position hint array used
    /// by Phase 1.4 long-press disambiguation. user_phoneme_hints[i]
    /// values: 0 = primary, 1 = secondary, 2 = tertiary, 0xFF = any
    /// (no filter for that byte). nullptr (or all 0xFF) is equivalent to
    /// the no-hint search() above. Hints are ignored when the dict
    /// header.flags bit 0 is unset (legacy MIE4 dict).
    int search(const uint8_t* user_keys,
               const uint8_t* user_phoneme_hints,
               int user_n,
               int target_char_count,
               Candidate* out, int max_results) const;

    /// True if the loaded dict carries per-reading phoneme_pos bytes
    /// (header.flags bit 0). When false, all search() phoneme-hint
    /// arguments are silently ignored.
    bool has_phoneme_pos() const { return has_phoneme_pos_; }

    bool     is_loaded()  const { return loaded_; }
    uint32_t char_count() const { return char_count_; }
    uint32_t word_count() const { return word_count_; }
    uint16_t version()    const { return version_; }

    /// Data-driven syllable parser. Splits `keys` into the minimum number of
    /// "syllable units" using only the dict's own list of valid reading
    /// prefixes — at each position, consumes the longest sub-sequence that
    /// matches any reading prefix, falling back to a single-byte step when
    /// nothing matches. Replaces the hand-rolled phonotactic state machine
    /// in ImeLogic::count_positions: the dict is the single source of truth
    /// for what counts as a syllable.
    int  count_syllables(const uint8_t* keys, int len) const;

private:
    // Heap-owned buffer when loaded from file (nullptr in memory mode).
    uint8_t* owned_buf_ = nullptr;

    // Working pointers (may alias owned_buf_ or external buffer).
    const uint8_t* buf_      = nullptr;
    size_t         buf_size_ = 0;

    // Header fields cached after load.
    uint16_t version_    = 0;
    uint16_t flags_      = 0;
    bool     has_phoneme_pos_ = false;   ///< mirrors flags_ bit 0
    uint32_t char_count_ = 0;
    uint32_t word_count_ = 0;

    // Section base pointers (computed during load).
    const uint8_t* char_table_      = nullptr;
    const uint8_t* word_table_      = nullptr;
    const uint8_t* first_char_idx_  = nullptr;
    const uint8_t* key_to_char_idx_ = nullptr;

    // Read-only pointers into the v4 file's pre-computed offset sections.
    // NO heap allocation — critical for Core 1 which has only 48 KB FreeRTOS
    // heap. On host builds the v4 file lives in RAM anyway, so pointing
    // into it costs nothing. The offsets sections are ~75 KB + ~515 KB for
    // a typical 18K-char / 128K-word dict.
    //
    //   char_offsets_[cid]  byte offset of char_id `cid` within char_table_
    //   word_offsets_[wid]  byte offset of word_id `wid` within word_table_
    //                       (NB: absolute offset from word_table_ base, which
    //                       includes the 8 group-header records)
    const uint32_t* char_offsets_ = nullptr;  // size: char_count_ + 1
    const uint32_t* word_offsets_ = nullptr;  // size: word_count_ + 1

    // Group header table: 8 entries (one per char_count bucket 1..8+).
    struct GroupHeader {
        uint32_t count;
        uint32_t start_word_id;
    };
    GroupHeader groups_[8];

    // Sorted array of every distinct reading-prefix byte sequence. Built
    // at gen_dict.py time from the raw libchewing phonetic data and shipped
    // as a section inside the v4 file — the runtime never needs to re-derive
    // the ruleset, preserving Core 1's 48 KB FreeRTOS heap. Pointer aliases
    // into buf_; no separate allocation.
    //   File layout: u32 count | count × u64 packed entries
    //   Packing:     bits 32..39 = length (1..4)
    //                bits  0..31 = prefix bytes, little-endian
    const uint64_t* prefix_table_      = nullptr;
    uint32_t        prefix_table_size_ = 0;

    bool loaded_ = false;

    // ── Internal helpers ─────────────────────────────────────────────
    bool parse_header();

    /// Returns true if the byte sequence keys[0..len) appears as the prefix
    /// of any reading in the dict. Used by count_syllables.
    bool has_reading_prefix(const uint8_t* keys, int len) const;

    /// Fetch raw pointer + len for a char's UTF-8 bytes and reading_count.
    /// Returns pointer just past the reading_count byte (start of readings).
    const uint8_t* char_data_at(uint32_t char_id,
                                const uint8_t** out_utf8,
                                uint8_t* out_utf8_len,
                                uint8_t* out_reading_count) const;

    /// Get the keyseq pointer + length + tone for a specific reading of a char.
    /// When the dict carries phoneme_pos bytes (has_phoneme_pos()), also
    /// fills *out_phoneme_pos_packed with the packed byte (2 bits per kbyte
    /// position, LSB = byte 0). Otherwise *out_phoneme_pos_packed is set
    /// to 0. Pass nullptr to skip.
    /// Returns false if reading_idx is out of range.
    bool get_reading(uint32_t char_id, uint8_t reading_idx,
                     const uint8_t** out_keyseq,
                     uint8_t* out_klen,
                     uint8_t* out_tone,
                     uint8_t* out_phoneme_pos_packed = nullptr) const;

    /// Decode a word record. Sets fields from the v4 format. char_ids and
    /// reading_idxs (if any) point into buf_. reading_idxs may be nullptr
    /// when flags & 1 == 0 (all-zero overrides).
    struct WordView {
        uint8_t         char_count;
        uint8_t         flags;
        uint16_t        freq;
        const uint8_t*  char_ids_le;     // little-endian u16 array, char_count entries
        const uint8_t*  reading_idxs;    // nullptr if all-zero
    };
    bool get_word(uint32_t word_id, WordView* out) const;

    /// Composition match: check if the given word's chars can be composed
    /// into a key sequence whose prefix equals user_keys[0..user_n).
    /// When user_phoneme_hints is non-null, also requires each consumed
    /// reading byte's phoneme position to match the hint at the same user
    /// offset (or kPhonemeHintAny=0xFF to skip that byte). Hints are
    /// silently ignored when the dict has no phoneme_pos section.
    bool composition_matches(const WordView& w,
                             const uint8_t* user_keys,
                             const uint8_t* user_phoneme_hints,
                             int user_n) const;

    bool composition_recurse(const WordView& w,
                             int char_idx, int key_idx,
                             const uint8_t* user_keys,
                             const uint8_t* user_phoneme_hints,
                             int user_n) const;

    /// Build the UTF-8 word string from char_ids into Candidate.word.
    /// Truncates if total UTF-8 length would exceed kCandidateMaxBytes-1.
    void build_word_str(const WordView& w, char* out_word) const;

    /// Get the word's overall tone (last char's reading's tone).
    uint8_t get_word_tone(const WordView& w) const;

    /// Search char_table directly (1-char results). word_table's char_count=1
    /// group is empty by design — gen_dict.py only emits multi-char words to
    /// word_table; single-char "candidates" come from char_table via this
    /// path. Walks key_to_char_idx + each char's readings, collects matches
    /// (user_keys is a prefix of any of the char's reading's key sequence),
    /// sorts by per-reading freq desc.
    int search_chars(const uint8_t* user_keys,
                     const uint8_t* user_phoneme_hints,
                     int user_n,
                     Candidate* out, int max_results) const;
};

} // namespace mie
