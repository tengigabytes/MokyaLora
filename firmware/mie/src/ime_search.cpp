// SPDX-License-Identifier: MIT
// ime_search.cpp — Greedy prefix dictionary search + display rebuild.

#include "ime_internal.h"
#include <mie/composition_searcher.h>
#include <algorithm>
#include <cstring>

// Optional opt-in performance trace for MokyaLora Core 1 builds. PC tests
// and other consumers leave MOKYA_MIE_PERF_TRACE undefined, so the macros
// expand to no-ops and MIE stays decoupled from any platform header.
#ifdef MOKYA_MIE_PERF_TRACE
#include "mokya_trace.h"
#define MIE_TRACE(src, ev, fmt, ...) TRACE(src, ev, fmt, ##__VA_ARGS__)
#define MIE_TRACE_BARE(src, ev)      TRACE_BARE(src, ev)
#else
#define MIE_TRACE(src, ev, fmt, ...) ((void)0)
#define MIE_TRACE_BARE(src, ev)      ((void)0)
#endif

namespace mie {

// Top-level dispatcher: routes between v4 CompositionSearcher (when attached)
// and the legacy v2 TrieSearcher path. Both implementations populate
// candidates_, matched_prefix_keys_, and call rebuild_display_smart at the end.
void ImeLogic::run_search() {
    cand_count_           = 0;
    selected_             = 0;
    matched_prefix_bytes_ = 0;
    matched_prefix_keys_  = 0;

    if (key_seq_len_ == 0) {
        rebuild_display_smart();
        return;
    }

    if (composition_searcher_ && composition_searcher_->is_loaded() &&
        mode_ == InputMode::SmartZh) {
        run_search_v4();
        rebuild_display_smart();
        return;
    }

    run_search_v2_legacy();
}

// Legacy: original TrieSearcher path. Unchanged from the pre-v4 implementation
// except for the rename. Kept while v4 rolls out (Phase 5 may remove this
// once Core 1 firmware loads dict_mie.bin exclusively).
void ImeLogic::run_search_v2_legacy() {
    if (key_seq_len_ == 0) {
        rebuild_display_smart();
        return;
    }

    // Build a 0x20-stripped copy for dictionary lookup.
    //
    // gen_dict.py skips tone 1 from the encoded dict key (it occupies no
    // byte because it's the implicit / unmarked tone). User input, by
    // contrast, inserts 0x20 as the first-tone marker between syllables
    // when the user hits SPACE. Stripping 0x20 bytes before searching
    // lets multi-syllable words still match — e.g. 開始 (dict key
    // \x2C\x25\x2D\x22) matches when the user types
    // ㄎ ㄞ SPACE ㄕ ˇ (raw key_seq \x2C\x25\x20\x2D\x22).
    //
    // strip_to_orig[i] records the index in key_seq_ that each stripped
    // byte came from, so tone-intent extraction and matched_prefix_keys_
    // can still refer back to the original user input.
    char stripped[kMaxKeySeq + 1];
    int  strip_to_orig[kMaxKeySeq + 1];
    int  stripped_len = 0;
    for (int i = 0; i < key_seq_len_; ++i) {
        if ((uint8_t)key_seq_[i] != 0x20) {
            stripped[stripped_len]      = key_seq_[i];
            strip_to_orig[stripped_len] = i;
            ++stripped_len;
        }
    }
    stripped[stripped_len] = '\0';

    if (stripped_len == 0) {
        // Only 0x20 bytes — nothing to search on.
        rebuild_display_smart();
        return;
    }

    // Tone-1 priority guard (single-syllable, "user changed their mind"):
    //   original ends with 0x20 AND preceding stripped byte is a tone key
    //   (ˇ/ˋ = 0x22/0x23). Skip the full-stripped match so the bare
    //   phoneme search runs first with tone-1 intent.
    int skip_slen = -1;
    if ((uint8_t)key_seq_[key_seq_len_ - 1] == 0x20 &&
        stripped_len >= 2 &&
        ((uint8_t)stripped[stripped_len - 1] == 0x22 ||
         (uint8_t)stripped[stripped_len - 1] == 0x23) &&
        // The tone byte is immediately before the final 0x20 in orig.
        strip_to_orig[stripped_len - 1] + 1 == key_seq_len_ - 1) {
        skip_slen = stripped_len;
    }

    // Pick the active searcher for this mode.
    TrieSearcher* searcher = nullptr;
    if (mode_ == InputMode::SmartZh && zh_searcher_.is_loaded()) {
        searcher = &zh_searcher_;
    } else if (mode_ == InputMode::SmartEn && en_searcher_ && en_searcher_->is_loaded()) {
        searcher = en_searcher_;
    }

    if (!searcher) {
        rebuild_display_smart();
        return;
    }

    Candidate tmp[kMaxCandidates];

    MIE_TRACE("mie", "rs_start", "kseq=%d,strip=%d",
              key_seq_len_, stripped_len);

    // Iterate from the longest prefix down. Candidates from longer
    // matches are more constrained and rank first; shorter-match
    // candidates fill the remainder so the user can commit at any
    // sub-phrase boundary (6-key input might return "半票價" at slen=6
    // and also "白天" at slen=2 for partial commit). Each candidate
    // remembers its own prefix length for commit_selected_candidate.
    for (int slen = stripped_len; slen >= 1; --slen) {
        if (cand_count_ >= kMaxCandidates) break;
        if (slen == skip_slen) continue;

        char saved      = stripped[slen];
        stripped[slen]  = '\0';
        MIE_TRACE("mie", "search_call", "slen=%d", slen);
        int n           = searcher->search(stripped, tmp, kMaxCandidates);
        MIE_TRACE("mie", "search_done", "slen=%d,n=%d,cum=%d",
                  slen, n, cand_count_);
        stripped[slen]  = saved;
        if (n == 0) continue;

        int orig_prefix_end = strip_to_orig[slen - 1] + 1;
        int intent = extract_tone_intent(key_seq_, key_seq_len_, orig_prefix_end);

        auto tone_sort = [intent](const Candidate& a, const Candidate& b) {
            int ta = tone_tier(a, intent), tb = tone_tier(b, intent);
            if (ta != tb) return ta < tb;
            return a.freq > b.freq;
        };
        if (n > 1) std::stable_sort(tmp, tmp + n, tone_sort);

        // Strict tone filter (only applies when user typed a tone marker).
        if (intent != 0) {
            int keep = 0;
            for (int i = 0; i < n; ++i)
                if (tone_tier(tmp[i], intent) < 2)
                    tmp[keep++] = tmp[i];
            if (keep > 0) n = keep;
        }

        // Record the longest matched prefix (first slen that hits) for
        // display bold/normal split. Later shorter matches do not
        // shrink this indicator.
        if (matched_prefix_keys_ == 0) matched_prefix_keys_ = orig_prefix_end;

        // Merge into candidates_, dedup by word string — the longest-
        // match instance is kept because it was inserted first.
        for (int i = 0; i < n && cand_count_ < kMaxCandidates; ++i) {
            bool dup = false;
            for (int j = 0; j < cand_count_; ++j) {
                if (std::strcmp(candidates_[j].word, tmp[i].word) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            candidates_[cand_count_]             = tmp[i];
            candidates_prefix_keys_[cand_count_] = (uint8_t)orig_prefix_end;
            ++cand_count_;
        }
    }

    MIE_TRACE("mie", "rs_end", "total=%d", cand_count_);
    rebuild_display_smart();
}

// ── v4 dispatch ──────────────────────────────────────────────────────────
// Position-based bucket dispatch over CompositionSearcher. Architecture
// from docs/plan: count_positions(key_seq_) yields the user's intended word
// length; that drives target_char_count. 0-result fallback chains adjacent
// buckets (1-4) or truncated prefixes (5+).
//
// Caller (run_search) has already zeroed candidates_ / matched_prefix_keys_.
void ImeLogic::run_search_v4() {
    // Prefer the data-driven syllable parser (uses the dict's own reading
    // table as ground truth) when the searcher is loaded; fall back to the
    // hand-rolled phonotactic state machine for callers that haven't built
    // a prefix table (PC tests with synthetic dicts).
    int positions = composition_searcher_
        ? composition_searcher_->count_syllables(
              reinterpret_cast<const uint8_t*>(key_seq_), key_seq_len_)
        : count_positions(key_seq_, key_seq_len_);
    if (positions <= 0) {
        positions = count_positions(key_seq_, key_seq_len_);
    }
    MIE_TRACE("v4", "rs_dispatch", "pos=%d,kseq_len=%d", positions, key_seq_len_);
    if (positions <= 0) return;

    int n = 0;

    if (positions <= 4) {
        // Main: search the matching word-length bucket with full user keys.
        n = composition_searcher_->search(
            reinterpret_cast<const uint8_t*>(key_seq_),
            phoneme_hint_, key_seq_len_,
            positions, candidates_, kMaxCandidates);

        // Always augment with adjacent buckets (not only when n==0). The
        // position counter's H2 heuristic is ambiguous around ㄧ/ㄨ/ㄩ
        // (Medial role, but can also be a syllable initial — e.g. ㄍㄓㄩㄓ
        // for 高瞻遠矚 abbrev, where ㄩ is actually an initial). Merging
        // +1/-1 buckets tolerates both over- and under-count without the
        // primary dropping high-rank results.
        //
        // Order: +1 first so any missed longer match comes right after the
        // primary set; then -1 for shorter matches. Dedup by word string.
        const int adj[4] = {positions + 1, positions - 1,
                            positions + 2, positions - 2};
        // Static to avoid blowing the IME task's 8 KB stack — at
        // kMaxCandidates=100 a stack-local Candidate[100] is ~3.6 KB.
        // Safe because the IME task is the only caller of run_search().
        static Candidate tmp[kMaxCandidates];
        for (int k = 0; k < 4 && n < kMaxCandidates; ++k) {
            int t = adj[k];
            if (t < 1 || t > 4) continue;
            int m = composition_searcher_->search(
                reinterpret_cast<const uint8_t*>(key_seq_),
                phoneme_hint_, key_seq_len_,
                t, tmp, kMaxCandidates);
            for (int i = 0; i < m && n < kMaxCandidates; ++i) {
                bool dup = false;
                for (int j = 0; j < n; ++j) {
                    if (std::strcmp(candidates_[j].word, tmp[i].word) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                candidates_[n++] = tmp[i];
            }
        }

        // Whole user input was the search prefix in all attempts above.
        for (int i = 0; i < n; ++i) {
            candidates_prefix_keys_[i] = (uint8_t)key_seq_len_;
        }
        matched_prefix_keys_ = key_seq_len_;
        cand_count_ = n;
        return;
    }

    // positions >= 5: char-by-char commit mode with optional long-phrase
    // suggestion appended.
    //
    // Rationale: long inputs (5+ initials) are typically the user typing a
    // SENTENCE one initial per character, intending to commit char-by-char.
    // Presenting 1-char candidates for the FIRST initial lets them pick
    // immediately; commit consumes 1 byte and the remaining 4+ positions
    // re-dispatch as normal 4-char (or shorter) bucket search.
    //
    // We ALSO append any 5+ char phrase matches (target=-1) at the end of
    // the candidate list so the rare case where the user IS typing a known
    // long phrase still surfaces it. The phrase candidates rank below the
    // 1-char ones because that matches the much-more-common usage pattern.

    // Primary: 1-char candidates for first initial. Only the first byte's
    // hint is relevant here (commit consumes exactly 1 byte).
    uint8_t first_byte  = (uint8_t)key_seq_[0];
    uint8_t first_hint  = phoneme_hint_[0];
    n = composition_searcher_->search(
        &first_byte, &first_hint, 1, /*target=*/1, candidates_, kMaxCandidates);
    for (int i = 0; i < n; ++i) {
        candidates_prefix_keys_[i] = 1;  // commit consumes 1 byte
    }

    // Append optional 5+ phrase match (commits the full key_seq if picked).
    int phrase_capacity = kMaxCandidates - n;
    int phrase_n = 0;
    if (phrase_capacity > 0) {
        phrase_n = composition_searcher_->search(
            reinterpret_cast<const uint8_t*>(key_seq_),
            phoneme_hint_, key_seq_len_,
            /*target_char_count=*/ -1,
            candidates_ + n, phrase_capacity);
        for (int i = 0; i < phrase_n; ++i) {
            candidates_prefix_keys_[n + i] = (uint8_t)key_seq_len_;
        }
    }

    MIE_TRACE("v4", "rs5_charmode", "char_n=%d,phrase_n=%d", n, phrase_n);

    cand_count_ = n + phrase_n;
    matched_prefix_keys_ = (n > 0) ? 1 : (phrase_n > 0 ? key_seq_len_ : 0);
}

// Rebuild display_ from key_seq_. Shared between SmartZh and SmartEn.
//   SmartZh: key groups rendered as compound (e.g. "ㄅㄉ" or "ㄞㄢㄦ"),
//            separated by ", ". First-tone marker byte (0x20) renders as ˉ.
//   SmartEn: primary letter per key, no separators (e.g. "we").
// Also updates matched_prefix_bytes_ to mirror matched_prefix_keys_ in bytes.
void ImeLogic::rebuild_display_smart() {
    display_len_          = 0;
    display_[0]           = '\0';
    matched_prefix_bytes_ = 0;
    pending_style_        = (key_seq_len_ == 0) ? PendingStyle::None
                                                 : PendingStyle::PrefixBold;

    auto append = [this](const char* s) {
        if (!s) return;
        int n = (int)std::strlen(s);
        if (display_len_ + n >= kMaxDisplayBytes) return;
        std::memcpy(display_ + display_len_, s, (size_t)n);
        display_len_ += n;
        display_[display_len_] = '\0';
    };

    const bool is_zh = (mode_ == InputMode::SmartZh);

    for (int i = 0; i < key_seq_len_; ++i) {
        if (i == matched_prefix_keys_) {
            matched_prefix_bytes_ = display_len_;
        }

        uint8_t b = (uint8_t)key_seq_[i];

        if (b == 0x20) {
            // First-tone marker → "ˉ" (U+02C9, 2 UTF-8 bytes).
            if (is_zh && i > 0) append(", ");
            append("\xcb\x89");
            continue;
        }

        int slot = (int)b - 0x21;
        if (slot < 0 || slot >= 20) continue;
        const KeyEntry& e = kKeyTable[slot];

        if (is_zh) {
            if (i > 0) append(", ");
            // Phase 1.4: when the user disambiguated this byte via long-
            // press (hint = 1 secondary, 2 tertiary, 0 primary), render
            // only the chosen phoneme so the pending row reflects what
            // the engine is actually filtering on. Hint 0xFF (any) keeps
            // the legacy compound-phoneme rendering — used for tone
            // markers, the SPACE first-tone byte, and SmartEn.
            uint8_t hint = phoneme_hint_[i];
            if (hint <= 2) {
                int p_count = 0;
                for (int k = 0; k < 3 && e.phonemes[k]; ++k) ++p_count;
                int p = (int)hint;
                if (p >= p_count) p = p_count - 1;
                if (p < 0) p = 0;
                append(e.phonemes[p]);
            } else {
                for (int p = 0; p < 3 && e.phonemes[p]; ++p)
                    append(e.phonemes[p]);
            }
        } else {
            // SmartEn: primary lowercase letter (or primary digit for row 0).
            if (e.letter_slots[0])     append(e.letter_slots[0]);
            else if (e.digit_slots[0]) append(e.digit_slots[0]);
        }
    }

    // Full-match case: loop didn't hit the `i == matched_prefix_keys_` line.
    if (matched_prefix_keys_ >= key_seq_len_)
        matched_prefix_bytes_ = display_len_;
}

} // namespace mie
