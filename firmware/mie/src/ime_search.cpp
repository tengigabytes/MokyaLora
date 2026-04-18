// SPDX-License-Identifier: MIT
// ime_search.cpp — Greedy prefix dictionary search + display rebuild.

#include "ime_internal.h"
#include <algorithm>
#include <cstring>

namespace mie {

// Greedy prefix search: try decreasing key_seq_ lengths until the dict
// returns results. Fill candidates_, matched_prefix_keys_, and rebuild
// display_ + matched_prefix_bytes_ via rebuild_display_smart.
void ImeLogic::run_search() {
    cand_count_           = 0;
    selected_             = 0;
    matched_prefix_bytes_ = 0;
    matched_prefix_keys_  = 0;

    if (key_seq_len_ == 0) {
        rebuild_display_smart();
        return;
    }

    // Tone-1 priority guard:
    //   key_seq = [K, 0x22|0x23, 0x20]  (len 3)  — tone-modifier + first-tone
    //   The greedy would otherwise match len=2 (tone 3/4) and shadow the
    //   tone-1 candidates for K alone. Skip len=2 so [K] (len 1) runs first.
    int skip_len = -1;
    if (key_seq_len_ == 3 && (uint8_t)key_seq_[2] == 0x20) {
        uint8_t pre = (uint8_t)key_seq_[1];
        if (pre == 0x22 || pre == 0x23) skip_len = 2;
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

    for (int len = key_seq_len_; len >= 1; --len) {
        if (len == skip_len) continue;

        char saved    = key_seq_[len];
        key_seq_[len] = '\0';
        int n         = searcher->search(key_seq_, tmp, kMaxCandidates);
        key_seq_[len] = saved;

        if (n > 0) {
            int intent = extract_tone_intent(key_seq_, key_seq_len_, len);

            auto tone_sort = [intent](const Candidate& a, const Candidate& b) {
                int ta = tone_tier(a, intent), tb = tone_tier(b, intent);
                if (ta != tb) return ta < tb;
                return a.freq > b.freq;
            };
            if (n > 1) std::stable_sort(tmp, tmp + n, tone_sort);

            // Strict tone filter: when intent != 0, keep only tier-0/1
            // (exact-tone matches). Fall back to the full list if that
            // produces nothing (v1 dicts with tone == 0).
            if (intent != 0) {
                int keep = 0;
                for (int i = 0; i < n; ++i)
                    if (tone_tier(tmp[i], intent) < 2)
                        tmp[keep++] = tmp[i];
                if (keep > 0) n = keep;
            }

            cand_count_ = n;
            std::memcpy(candidates_, tmp, (size_t)n * sizeof(Candidate));
            matched_prefix_keys_ = len;
            break;
        }
    }

    rebuild_display_smart();
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
            for (int p = 0; p < 3 && e.phonemes[p]; ++p)
                append(e.phonemes[p]);
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
