// composition_searcher.cpp - see composition_searcher.h
// SPDX-License-Identifier: MIT

#include <mie/composition_searcher.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace mie {

// ── File-format constants ─────────────────────────────────────────────────
static constexpr uint8_t  kMagic[4]  = { 'M', 'I', 'E', '4' };
static constexpr uint16_t kVersion   = 4;
static constexpr uint32_t kHeaderSize = 0x30;
static constexpr int      kKeyByteMin = 0x20;
static constexpr int      kKeyByteMax = 0x38;  // exclusive
static constexpr int      kKeySlots   = kKeyByteMax - kKeyByteMin;  // 24
static constexpr int      kMaxGroups  = 8;

// ── Little-endian load helpers ────────────────────────────────────────────
static inline uint16_t load_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t load_u32(const uint8_t* p) {
    return (uint32_t)(p[0]
                    | ((uint32_t)p[1] << 8)
                    | ((uint32_t)p[2] << 16)
                    | ((uint32_t)p[3] << 24));
}

// ── Destructor ────────────────────────────────────────────────────────────
// char_offsets_ and word_offsets_ are read-only pointers into buf_ (in the
// v4 file); they are NOT heap-allocated so only owned_buf_ (file-loaded
// buffer, when applicable) needs freeing.
CompositionSearcher::~CompositionSearcher() {
    delete[] owned_buf_;
}

// ── Reading-prefix table (for count_syllables) ────────────────────────────
// Packs (length, bytes) into a single uint64_t for fast comparison.
static inline uint64_t pack_prefix(const uint8_t* b, int len) {
    uint64_t v = (uint64_t)(uint8_t)len << 32;
    for (int i = 0; i < len && i < 4; ++i) {
        v |= (uint64_t)b[i] << (i * 8);
    }
    return v;
}

bool CompositionSearcher::has_reading_prefix(const uint8_t* b, int len) const {
    if (!prefix_table_ || len <= 0 || len > 4) return false;
    uint64_t key = pack_prefix(b, len);
    int lo = 0, hi = prefix_table_size_;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (prefix_table_[mid] < key) lo = mid + 1;
        else                          hi = mid;
    }
    return lo < prefix_table_size_ && prefix_table_[lo] == key;
}

int CompositionSearcher::count_syllables(const uint8_t* keys, int len) const {
    if (!keys || len <= 0) return 0;
    int count = 0;
    int i = 0;
    while (i < len) {
        int max_try = len - i;
        if (max_try > 4) max_try = 4;
        int best = 0;
        for (int L = max_try; L >= 1; --L) {
            if (has_reading_prefix(keys + i, L)) {
                best = L;
                break;
            }
        }
        if (best == 0) {
            // No valid prefix anywhere; treat the byte as noise.
            ++i;
        } else {
            ++count;
            i += best;
        }
    }
    return count;
}

// ── File loading ──────────────────────────────────────────────────────────
bool CompositionSearcher::load_from_file(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    auto* buf = new (std::nothrow) uint8_t[static_cast<size_t>(sz)];
    if (!buf) { std::fclose(f); return false; }
    if (std::fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
        std::fclose(f);
        delete[] buf;
        return false;
    }
    std::fclose(f);
    delete[] owned_buf_;
    owned_buf_ = buf;
    return load_from_memory(buf, static_cast<size_t>(sz));
}

bool CompositionSearcher::load_from_memory(const uint8_t* buf, size_t size) {
    loaded_ = false;
    char_offsets_ = nullptr;
    word_offsets_ = nullptr;

    if (!buf || size < kHeaderSize) return false;
    buf_ = buf;
    buf_size_ = size;

    if (!parse_header()) return false;
    // Offset arrays and the syllable-prefix table are read directly from
    // the v4 file — no heap alloc.
    loaded_ = true;
    return true;
}

bool CompositionSearcher::parse_header() {
    if (std::memcmp(buf_, kMagic, 4) != 0) return false;
    version_ = load_u16(buf_ + 4);
    if (version_ != kVersion) return false;
    // flags at +6 (unused)
    char_count_ = load_u32(buf_ + 8);
    word_count_ = load_u32(buf_ + 12);

    uint32_t char_off       = load_u32(buf_ + 16);
    uint32_t word_off       = load_u32(buf_ + 20);
    uint32_t first_off      = load_u32(buf_ + 24);
    uint32_t key_off        = load_u32(buf_ + 28);
    // total_size at 0x20 (validated if present).
    uint32_t char_offs_off  = load_u32(buf_ + 0x24);
    uint32_t word_offs_off  = load_u32(buf_ + 0x28);
    uint32_t prefix_off     = load_u32(buf_ + 0x2C);   // 0 = not present

    if (char_off      >= buf_size_ ||
        word_off      >= buf_size_ ||
        first_off     >= buf_size_ ||
        key_off       >= buf_size_ ||
        char_offs_off >= buf_size_ ||
        word_offs_off >= buf_size_) {
        return false;
    }

    char_table_      = buf_ + char_off;
    word_table_      = buf_ + word_off;
    first_char_idx_  = buf_ + first_off;
    key_to_char_idx_ = buf_ + key_off;

    char_offsets_ = reinterpret_cast<const uint32_t*>(buf_ + char_offs_off);
    word_offsets_ = reinterpret_cast<const uint32_t*>(buf_ + word_offs_off);

    // Syllable-prefix table: u32 count + count × u64. The header slot is
    // optional (zero if the builder didn't emit the section — older dicts).
    if (prefix_off != 0 && prefix_off + 4 <= buf_size_) {
        uint32_t cnt = load_u32(buf_ + prefix_off);
        // Sanity: each u64 is 8 B; guard against a bogus cnt that would
        // read past end of buffer.
        if (prefix_off + 4 + (uint64_t)cnt * 8 <= buf_size_) {
            prefix_table_      = reinterpret_cast<const uint64_t*>(
                                     buf_ + prefix_off + 4);
            prefix_table_size_ = cnt;
        }
    }

    // Read 8 group headers from word_table_.
    for (int g = 0; g < kMaxGroups; ++g) {
        groups_[g].count         = load_u32(word_table_ + 8 * g);
        groups_[g].start_word_id = load_u32(word_table_ + 8 * g + 4);
    }

    return true;
}

// ── Char/word data access ─────────────────────────────────────────────────
const uint8_t* CompositionSearcher::char_data_at(
    uint32_t char_id,
    const uint8_t** out_utf8,
    uint8_t* out_utf8_len,
    uint8_t* out_reading_count) const
{
    const uint8_t* p = char_table_ + char_offsets_[char_id];
    *out_utf8_len = *p++;
    *out_utf8 = p;
    p += *out_utf8_len;
    *out_reading_count = *p++;
    return p;  // points at first reading record
}

bool CompositionSearcher::get_reading(uint32_t char_id, uint8_t reading_idx,
                                       const uint8_t** out_keyseq,
                                       uint8_t* out_klen,
                                       uint8_t* out_tone) const {
    const uint8_t* utf8;
    uint8_t utf8_len, rcount;
    const uint8_t* p = char_data_at(char_id, &utf8, &utf8_len, &rcount);
    if (reading_idx >= rcount) return false;
    for (uint8_t r = 0; r < rcount; ++r) {
        uint8_t klen = *p++;
        const uint8_t* kbytes = p;
        p += klen;
        uint8_t tone = *p++;
        p += 2;  // freq
        if (r == reading_idx) {
            *out_keyseq = kbytes;
            *out_klen   = klen;
            *out_tone   = tone;
            return true;
        }
    }
    return false;
}

bool CompositionSearcher::get_word(uint32_t word_id, WordView* out) const {
    if (word_id >= word_count_) return false;
    const uint8_t* p = word_table_ + word_offsets_[word_id];
    out->char_count   = p[0];
    out->flags        = p[1];
    out->freq         = load_u16(p + 2);
    out->char_ids_le  = p + 4;
    if (out->flags & 1) {
        out->reading_idxs = p + 4 + 2 * out->char_count;
    } else {
        out->reading_idxs = nullptr;
    }
    return true;
}

// ── Composition matching (backtracking) ───────────────────────────────────
bool CompositionSearcher::composition_recurse(
    const WordView& w,
    int char_idx, int key_idx,
    const uint8_t* user_keys, int user_n) const
{
    if (key_idx >= user_n) return true;            // user input fully consumed
    if (char_idx >= w.char_count) return false;    // no more chars

    uint16_t cid = load_u16(w.char_ids_le + 2 * char_idx);
    uint8_t  ridx = w.reading_idxs ? w.reading_idxs[char_idx] : 0;

    const uint8_t* keyseq;
    uint8_t klen, tone;
    if (!get_reading(cid, ridx, &keyseq, &klen, &tone)) return false;

    int remaining = user_n - key_idx;

    // Prefix lengths to try, descending, deduped.
    //   klen     full reading including the tone byte at the end
    //   klen-1   phonemes only (tone stripped) — the user typed the syllable
    //            without pressing the tone key, which is the dominant pattern
    //            for mid-word chars (e.g. ㄎㄨㄞ in 快樂 before ㄌ)
    //   2        initial + medial abbreviation (CV)
    //   1        initial-only abbreviation (C)
    // klen <= 4 in practice (Bopomofo max = I+M+F+T). Dedup keeps n_try small.
    int tried[4] = {0, 0, 0, 0};
    int n_try = 0;
    auto push_try = [&](int p) {
        if (p < 1 || p > (int)klen) return;
        for (int k = 0; k < n_try; ++k) if (tried[k] == p) return;
        tried[n_try++] = p;
    };
    push_try(klen);
    push_try(klen - 1);
    push_try(2);
    push_try(1);

    for (int i = 0; i < n_try; ++i) {
        int plen = tried[i];
        if (plen > remaining) {
            // User input ends mid-reading: check user_keys[key_idx:] prefix of reading.
            if (std::memcmp(keyseq, user_keys + key_idx,
                            (size_t)remaining) == 0) {
                return true;
            }
        } else if (std::memcmp(keyseq, user_keys + key_idx,
                                (size_t)plen) == 0) {
            // Tone-1 explicit delimiter: if the full reading matched AND the
            // char's tone is 1 (no tone byte stored in keyseq) AND the user's
            // next byte is Space (0x20) — the user's tone-1 marker — consume
            // it too so the next syllable can start cleanly.
            int consumed = plen;
            if (plen == (int)klen && tone == 1 &&
                key_idx + plen < user_n &&
                user_keys[key_idx + plen] == 0x20) {
                ++consumed;
            }
            if (composition_recurse(w, char_idx + 1, key_idx + consumed,
                                     user_keys, user_n)) {
                return true;
            }
        }
    }
    return false;
}

bool CompositionSearcher::composition_matches(
    const WordView& w,
    const uint8_t* user_keys, int user_n) const
{
    return composition_recurse(w, 0, 0, user_keys, user_n);
}

// ── Word -> Candidate string and tone ─────────────────────────────────────
void CompositionSearcher::build_word_str(const WordView& w, char* out_word) const {
    int written = 0;
    const int cap = kCandidateMaxBytes - 1;  // leave room for NUL
    for (int i = 0; i < w.char_count; ++i) {
        uint16_t cid = load_u16(w.char_ids_le + 2 * i);
        const uint8_t* utf8;
        uint8_t utf8_len, rcount;
        char_data_at(cid, &utf8, &utf8_len, &rcount);
        if (written + utf8_len > cap) break;
        std::memcpy(out_word + written, utf8, utf8_len);
        written += utf8_len;
    }
    out_word[written] = '\0';
}

uint8_t CompositionSearcher::get_word_tone(const WordView& w) const {
    if (w.char_count == 0) return 0;
    uint16_t last_cid = load_u16(w.char_ids_le + 2 * (w.char_count - 1));
    uint8_t  ridx = w.reading_idxs ? w.reading_idxs[w.char_count - 1] : 0;
    const uint8_t* keyseq;
    uint8_t klen, tone;
    if (get_reading(last_cid, ridx, &keyseq, &klen, &tone)) return tone;
    return 0;
}

// ── Single-char search (target_char_count == 1) ─────────────────────────
// Walks char_table directly because word_table's char_count=1 group is
// always empty (gen_dict.py only emits multi-char entries to word_table).
int CompositionSearcher::search_chars(const uint8_t* user_keys, int user_n,
                                       Candidate* out, int max_results) const
{
    uint8_t first_byte = user_keys[0];
    if (first_byte < kKeyByteMin || first_byte >= kKeyByteMax) return 0;
    int slot = first_byte - kKeyByteMin;
    uint32_t cs = load_u32(key_to_char_idx_ + 4 * slot);
    uint32_t ce = load_u32(key_to_char_idx_ + 4 * (slot + 1));
    const uint8_t* cand_chars_base =
        key_to_char_idx_ + 4 * (kKeySlots + 1);

    // Trailing SPACE (0x20) is the user's explicit tone-1 marker. Tone-1
    // readings in the dict have no tone byte in their keyseq (tone field=1,
    // keyseq is phonemes only), so we strip the space and require tone==1
    // for the match — otherwise tone-1 chars like 八 (ㄅㄚ) would be missed
    // when the user types ㄅㄚ + Space.
    int  eff_n           = user_n;
    bool require_tone_1  = false;
    if (user_n > 0 && user_keys[user_n - 1] == 0x20) {
        eff_n          = user_n - 1;
        require_tone_1 = true;
        if (eff_n <= 0) return 0;
    }

    // Top-K by freq over the full matching set. Previous impl capped the
    // working pool at 256 and stopped iterating once full — low-freq chars
    // whose cid was visited late (e.g. 雞 for ㄐㄧ tone 1) were silently
    // dropped even if they outranked entries in the early pool. Walk ALL
    // candidates, keep the top max_results by freq via a running min-slot
    // replacement (same pattern as search()).
    struct Pick { uint16_t char_id; uint8_t reading_idx; uint16_t freq; uint8_t tone; };
    Pick out_pool[256];
    int top_n = 0;
    int min_idx = 0;
    int cap = max_results < 256 ? max_results : 256;

    auto emit = [&](uint16_t cid, uint8_t r, uint16_t freq, uint8_t tone) {
        if (top_n < cap) {
            out_pool[top_n] = { cid, r, freq, tone };
            if (top_n == 0 || freq < out_pool[min_idx].freq) min_idx = top_n;
            ++top_n;
            if (top_n == cap) {
                min_idx = 0;
                for (int i = 1; i < top_n; ++i)
                    if (out_pool[i].freq < out_pool[min_idx].freq) min_idx = i;
            }
        } else if (freq > out_pool[min_idx].freq) {
            out_pool[min_idx] = { cid, r, freq, tone };
            int nm = 0;
            for (int i = 1; i < top_n; ++i)
                if (out_pool[i].freq < out_pool[nm].freq) nm = i;
            min_idx = nm;
        }
    };

    for (uint32_t i = cs; i < ce; ++i) {
        uint16_t cid = load_u16(cand_chars_base + 2 * i);

        const uint8_t* p = char_table_ + char_offsets_[cid];
        uint8_t utf8_len = *p++;
        p += utf8_len;
        uint8_t rcount = *p++;
        for (uint8_t r = 0; r < rcount; ++r) {
            uint8_t klen = *p++;
            const uint8_t* kbytes = p;
            p += klen;
            uint8_t tone = *p++;
            uint16_t freq = load_u16(p); p += 2;

            if (require_tone_1 && tone != 1) continue;
            if (klen >= eff_n &&
                std::memcmp(kbytes, user_keys, (size_t)eff_n) == 0) {
                emit(cid, r, freq, tone);
                break;  // one entry per char
            }
        }
    }

    // Final freq-desc sort for the kept top-K.
    std::sort(out_pool, out_pool + top_n,
              [](const Pick& a, const Pick& b) { return a.freq > b.freq; });

    for (int i = 0; i < top_n; ++i) {
        const uint8_t* utf8;
        uint8_t utf8_len, rcount;
        char_data_at(out_pool[i].char_id, &utf8, &utf8_len, &rcount);
        if (utf8_len > kCandidateMaxBytes - 1) utf8_len = kCandidateMaxBytes - 1;
        std::memcpy(out[i].word, utf8, utf8_len);
        out[i].word[utf8_len] = '\0';
        out[i].freq = out_pool[i].freq;
        out[i].tone = out_pool[i].tone;
    }
    return top_n;
}

// ── Search entry point ────────────────────────────────────────────────────
int CompositionSearcher::search(const uint8_t* user_keys, int user_n,
                                int target_char_count,
                                Candidate* out, int max_results) const
{
    if (!loaded_ || !user_keys || user_n <= 0 ||
        !out || max_results <= 0) return 0;

    // target == 1: chars come from char_table, not word_table group 1
    // (which is always empty by design).
    if (target_char_count == 1) {
        return search_chars(user_keys, user_n, out, max_results);
    }

    // Phase 1: candidate first-chars whose first reading begins with user_keys[0].
    uint8_t first_byte = user_keys[0];
    if (first_byte < kKeyByteMin || first_byte >= kKeyByteMax) return 0;
    int slot = first_byte - kKeyByteMin;
    uint32_t cs = load_u32(key_to_char_idx_ + 4 * slot);
    uint32_t ce = load_u32(key_to_char_idx_ + 4 * (slot + 1));
    const uint8_t* cand_chars_base =
        key_to_char_idx_ + 4 * (kKeySlots + 1);

    int collected = 0;
    // Track the index of the current minimum-freq entry in out[], maintained
    // once the pool is full. Allows the walk to CONTINUE past max_results
    // and pull in high-freq matches from later first-char buckets — the
    // previous "return on full" policy missed high-freq words whose
    // first-char was iterated late in key_to_char_idx. (2026-04-22)
    int min_idx = 0;

    // Word_id range filter for target_char_count.
    // target == 0  -> any length (no filter)
    // target 1..7  -> only that group's word_ids (target==1 handled above via char_table)
    // target == -1 -> long words: union of buckets 5, 6, 7, 8+ (char_count >= 5)
    uint32_t allowed_min = 0, allowed_max = word_count_;
    if (target_char_count > 0 && target_char_count <= 7) {
        const GroupHeader& g = groups_[target_char_count - 1];
        allowed_min = g.start_word_id;
        allowed_max = g.start_word_id + g.count;
    } else if (target_char_count == -1) {
        allowed_min = groups_[4].start_word_id;
        allowed_max = groups_[7].start_word_id + groups_[7].count;
    }

    auto emit_match = [&](const WordView& w) {
        if (collected < max_results) {
            build_word_str(w, out[collected].word);
            out[collected].freq = w.freq;
            out[collected].tone = get_word_tone(w);
            if (collected == 0 || w.freq < out[min_idx].freq) {
                min_idx = collected;
            }
            ++collected;
            if (collected == max_results) {
                // Finalize min_idx scan once full.
                min_idx = 0;
                for (int i = 1; i < collected; ++i) {
                    if (out[i].freq < out[min_idx].freq) min_idx = i;
                }
            }
        } else if (w.freq > out[min_idx].freq) {
            build_word_str(w, out[min_idx].word);
            out[min_idx].freq = w.freq;
            out[min_idx].tone = get_word_tone(w);
            // Re-scan for new minimum.
            int nm = 0;
            for (int i = 1; i < collected; ++i) {
                if (out[i].freq < out[nm].freq) nm = i;
            }
            min_idx = nm;
        }
    };

    for (uint32_t i = cs; i < ce; ++i) {
        uint16_t cand_cid = load_u16(cand_chars_base + 2 * i);

        uint32_t ws = load_u32(first_char_idx_ + 4 * cand_cid);
        uint32_t we = load_u32(first_char_idx_ + 4 * (cand_cid + 1));
        const uint8_t* wid_base =
            first_char_idx_ + 4 * (char_count_ + 1);

        for (uint32_t j = ws; j < we; ++j) {
            uint32_t wid = load_u32(wid_base + 4 * j);
            if (wid < allowed_min || wid >= allowed_max) continue;

            WordView w;
            if (!get_word(wid, &w)) continue;

            if (composition_matches(w, user_keys, user_n)) {
                emit_match(w);
            }
        }
    }

    // Sort final top-K by freq desc.
    std::sort(out, out + collected,
              [](const Candidate& a, const Candidate& b) {
                  return a.freq > b.freq;
              });

    return collected;
}

} // namespace mie
