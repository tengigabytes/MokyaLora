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

namespace mie {

// ── File-format constants ─────────────────────────────────────────────────

static constexpr uint8_t  kMagic[4]   = { 'M', 'I', 'E', 'D' };
static constexpr uint16_t kVersion    = 1;
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
    if (ver != kVersion) return false;

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

    dat_          = dat_buf;
    dat_size_     = dat_size;
    val_          = val_buf;
    val_size_     = val_size;
    key_count_    = kc;
    keys_data_off_ = kdo;
    loaded_       = true;
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

    // Binary search over the sorted index table.
    int lo = 0, hi = static_cast<int>(key_count_) - 1, found = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const int cmp = compare_key(static_cast<uint32_t>(mid), key_utf8);
        if (cmp == 0) { found = mid; break; }
        else if (cmp < 0) lo = mid + 1;
        else              hi = mid - 1;
    }
    if (found < 0) return 0;

    // Decode the ValueRecord in dict_values.bin.
    uint32_t vo = get_val_off(static_cast<uint32_t>(found));
    if (vo + 2u > val_size_) return 0;

    uint16_t word_count = 0;
    memcpy(&word_count, val_ + vo, 2);
    vo += 2;

    int n = 0;
    for (int i = 0; i < static_cast<int>(word_count) && n < max_results; ++i) {
        if (vo + 3u > val_size_) break;

        uint16_t freq = 0;
        memcpy(&freq, val_ + vo, 2);
        const uint8_t wlen = val_[vo + 2];
        vo += 3;

        if (vo + wlen > val_size_) break;

        if (wlen == 0 || wlen >= static_cast<uint8_t>(kCandidateMaxBytes)) {
            vo += wlen;  // skip oversized or zero-length entries
            continue;
        }

        memcpy(out[n].word, val_ + vo, wlen);
        out[n].word[wlen] = '\0';
        out[n].freq       = freq;
        vo += wlen;
        ++n;
    }
    return n;
}

} // namespace mie
