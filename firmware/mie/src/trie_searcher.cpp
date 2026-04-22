// trie_searcher.cpp — TrieSearcher implementation
// SPDX-License-Identifier: MIT
//
// Phase 1: sorted-key binary search.
// The index table in dict_dat.bin is already sorted lexicographically by the
// Python build tool, so a standard binary search gives O(log N) lookup.
//
// See trie_searcher.h for the complete binary format specification.

#include <mie/trie_searcher.h>

#include <algorithm>   // std::min
#include <cstdio>
#include <cstring>

// Optional opt-in performance trace (Core 1 RTT path). Default no-op.
#ifdef MOKYA_MIE_PERF_TRACE
#include "mokya_trace.h"
#define MIE_TRACE(src, ev, fmt, ...) TRACE(src, ev, fmt, ##__VA_ARGS__)
#else
#define MIE_TRACE(src, ev, fmt, ...) ((void)0)
#endif

namespace mie {

// ── File-format constants ─────────────────────────────────────────────────

static constexpr uint8_t  kMagic[4]   = { 'M', 'I', 'E', 'D' };
static constexpr uint16_t kVersionMin = 1;  // oldest supported format (no tone)
static constexpr uint16_t kVersionMax = 2;  // current format (tone byte per word)
static constexpr uint32_t kHeaderSize = 16;  // bytes before the index table
static constexpr uint32_t kEntrySize  = 8;   // bytes per IndexEntry

// ── Destructor ────────────────────────────────────────────────────────────

TrieSearcher::~TrieSearcher() {
    delete[] owned_dat_;
    delete[] owned_val_;
}

// ── File loading (PC / host mode) ─────────────────────────────────────────

static uint8_t* slurp_file(const char* path, size_t& out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    auto* buf = new uint8_t[static_cast<size_t>(sz)];
    if (fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        fclose(f);
        delete[] buf;
        return nullptr;
    }
    fclose(f);
    out_size = static_cast<size_t>(sz);
    return buf;
}

bool TrieSearcher::load_from_file(const char* dat_path, const char* val_path) {
    size_t ds = 0, vs = 0;
    uint8_t* db = slurp_file(dat_path, ds);
    uint8_t* vb = slurp_file(val_path, vs);
    if (!db || !vb) {
        delete[] db;
        delete[] vb;
        return false;
    }
    // Free previous owned buffers before replacing them.
    delete[] owned_dat_;
    delete[] owned_val_;
    owned_dat_ = db;
    owned_val_ = vb;
    return load_from_memory(db, ds, vb, vs);
}

// ── Memory loading ────────────────────────────────────────────────────────

bool TrieSearcher::load_from_memory(const uint8_t* dat_buf, size_t dat_size,
                                    const uint8_t* val_buf, size_t val_size) {
    loaded_ = false;
    if (!dat_buf) return false;
    if (!val_buf && val_size > 0) return false;  // null val is OK for empty dict
    if (dat_size < kHeaderSize) return false;

    // Magic
    if (memcmp(dat_buf, kMagic, 4) != 0) return false;

    // Version
    uint16_t ver = 0;
    memcpy(&ver, dat_buf + 4, 2);
    if (ver < kVersionMin || ver > kVersionMax) return false;

    // key_count
    uint32_t kc = 0;
    memcpy(&kc, dat_buf + 8, 4);

    // keys_data_off
    uint32_t kdo = 0;
    memcpy(&kdo, dat_buf + 12, 4);

    // Sanity: the full index table must fit before keys_data_off.
    // Use 64-bit arithmetic to avoid overflow with large key_count.
    if (static_cast<uint64_t>(kHeaderSize) + static_cast<uint64_t>(kc) * kEntrySize > kdo)
        return false;
    if (kdo > dat_size)
        return false;

    dat_           = dat_buf;
    dat_size_      = dat_size;
    val_           = val_buf;
    val_size_      = val_size;
    key_count_     = kc;
    keys_data_off_ = kdo;
    version_       = ver;
    loaded_        = true;
    return true;
}

// ── Index helpers ─────────────────────────────────────────────────────────

int TrieSearcher::compare_key(uint32_t idx, const char* query) const {
    // IndexEntry starts at kHeaderSize + idx * kEntrySize.
    const uint32_t entry_off = kHeaderSize + idx * kEntrySize;

    uint32_t key_data_off = 0;
    memcpy(&key_data_off, dat_ + entry_off, 4);

    // Key record: [key_len: uint8][key_utf8: key_len bytes]
    const uint32_t kpos_len = keys_data_off_ + key_data_off;
    if (kpos_len >= dat_size_) return 1;  // corrupt — treat as "greater"

    const uint8_t  klen  = dat_[kpos_len];
    const uint32_t kpos  = kpos_len + 1;
    if (kpos + klen > dat_size_) return 1;

    const size_t qlen = strlen(query);
    const size_t clen = std::min(static_cast<size_t>(klen), qlen);

    const int cmp = memcmp(dat_ + kpos, query, clen);
    if (cmp != 0) return cmp;
    if (static_cast<size_t>(klen) < qlen) return -1;
    if (static_cast<size_t>(klen) > qlen) return  1;
    return 0;
}

uint32_t TrieSearcher::get_val_off(uint32_t idx) const {
    // val_data_off is at bytes [4..7] of the IndexEntry.
    const uint32_t entry_off = kHeaderSize + idx * kEntrySize + 4;
    uint32_t vo = 0;
    memcpy(&vo, dat_ + entry_off, 4);
    return vo;
}

// ── Search ────────────────────────────────────────────────────────────────

int TrieSearcher::search(const char* key_utf8,
                         Candidate*  out,
                         int         max_results) const {
    if (!loaded_ || !key_utf8 || !out || max_results <= 0) return 0;
    const size_t qlen = strlen(key_utf8);
    if (qlen == 0) return 0;

    // Prefix search: return candidates whose dict key is equal to or
    // longer than (but starts with) `key_utf8`. The Chinese dict stores
    // explicit per-syllable abbreviation entries, so an exact match at
    // short keys is common; the English dict stores only full-word keys,
    // so the prefix scan is what lets partial input ("aoo" → "application")
    // surface candidates incrementally.
    //
    // Algorithm:
    //   1. Binary search for the lowest index `lo` where key[lo] >= query.
    //   2. Scan forward while key[i] starts with query, inserting each
    //      word into a size-capped top-N buffer sorted by freq descending.
    //   3. Entries are lex-sorted, so the first key whose prefix diverges
    //      terminates the scan.
    MIE_TRACE("ts", "bin_start", "qlen=%u,kc=%u",
              (unsigned)qlen, (unsigned)key_count_);
    int lo = 0, hi = static_cast<int>(key_count_);
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (compare_key(static_cast<uint32_t>(mid), key_utf8) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    MIE_TRACE("ts", "bin_end", "lo=%d", lo);

    const bool     has_tone  = (version_ == 2);
    const uint32_t hdr_bytes = has_tone ? 4u : 3u;   // per-word header size

    /* Counters: n_scan = entries visited; n_match = entries with prefix
     * match (= n_scan in normal case, since we break on first non-match);
     * n_words = total word records decoded; n_dedup_cmp = strcmp/memcmp
     * compares done in the dedup inner loop. Reported once at search end. */
    int n_scan = 0, n_words = 0, n_dedup_cmp = 0;

    int collected = 0;
    for (int i = lo; i < static_cast<int>(key_count_); ++i) {
        ++n_scan;
        // Read this entry's key length + bytes.
        const uint32_t entry_off = kHeaderSize + static_cast<uint32_t>(i) * kEntrySize;
        uint32_t ko = 0;
        memcpy(&ko, dat_ + entry_off, 4);
        const uint32_t kpos_len = keys_data_off_ + ko;
        if (kpos_len + 1 > dat_size_) break;
        const uint8_t  klen = dat_[kpos_len];
        const uint32_t kpos = kpos_len + 1;
        if (kpos + klen > dat_size_) break;
        const uint8_t* kbuf = dat_ + kpos;

        // Prefix check. Keys sort lex, so once a key fails the prefix test
        // every later key also fails.
        if (static_cast<size_t>(klen) < qlen) break;
        if (memcmp(kbuf, key_utf8, qlen) != 0)  break;

        // Decode ValueRecord for this entry and merge words into the top-N.
        uint32_t vo = get_val_off(static_cast<uint32_t>(i));
        if (vo + 2u > val_size_) continue;
        uint16_t word_count = 0;
        memcpy(&word_count, val_ + vo, 2);
        vo += 2;

        for (int w = 0; w < static_cast<int>(word_count); ++w) {
            if (vo + hdr_bytes > val_size_) break;
            ++n_words;

            uint16_t freq = 0;
            memcpy(&freq, val_ + vo, 2);
            const uint8_t tone = has_tone ? val_[vo + 2] : 0;
            const uint8_t wlen = val_[vo + (has_tone ? 3u : 2u)];
            vo += hdr_bytes;

            if (vo + wlen > val_size_) break;
            if (wlen == 0 || wlen >= static_cast<uint8_t>(kCandidateMaxBytes)) {
                vo += wlen;
                continue;
            }

            // De-dup: gen_dict.py stores each word at every per-syllable
            // prefix combination, so the prefix scan encounters the same
            // word many times under different abbreviation keys. Skip if
            // we have already collected this word.
            bool is_dup = false;
            for (int j = 0; j < collected; ++j) {
                ++n_dedup_cmp;
                if (strlen(out[j].word) == static_cast<size_t>(wlen) &&
                    memcmp(out[j].word, val_ + vo, wlen) == 0) {
                    is_dup = true;
                    break;
                }
            }
            if (is_dup) { vo += wlen; continue; }

            // Insert (word, freq) into out[] keeping freq-descending order.
            // Ties preserve insertion (lex-key) order because the shift
            // condition uses strict <.
            if (collected < max_results) {
                int pos = collected;
                while (pos > 0 && out[pos - 1].freq < freq) {
                    out[pos] = out[pos - 1];
                    --pos;
                }
                memcpy(out[pos].word, val_ + vo, wlen);
                out[pos].word[wlen] = '\0';
                out[pos].freq = freq;
                out[pos].tone = tone;
                ++collected;
            } else if (freq > out[collected - 1].freq) {
                int pos = collected - 1;
                while (pos > 0 && out[pos - 1].freq < freq) {
                    out[pos] = out[pos - 1];
                    --pos;
                }
                memcpy(out[pos].word, val_ + vo, wlen);
                out[pos].word[wlen] = '\0';
                out[pos].freq = freq;
                out[pos].tone = tone;
            }
            vo += wlen;
        }
    }
    MIE_TRACE("ts", "search_stats",
              "scan=%d,words=%d,dedup=%d,collected=%d",
              n_scan, n_words, n_dedup_cmp, collected);
    return collected;
}

} // namespace mie
