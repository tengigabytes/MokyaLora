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

    for (int slen = stripped_len; slen >= 1; --slen) {
        if (slen == skip_slen) continue;

        char saved      = stripped[slen];
        stripped[slen]  = '\0';
        int n           = searcher->search(stripped, tmp, kMaxCandidates);
        stripped[slen]  = saved;

        if (n > 0) {
            // Map the matched stripped-prefix length back to the original
            // key_seq_ offset (one past the last orig byte consumed).
            int orig_prefix_end = strip_to_orig[slen - 1] + 1;
            int intent = extract_tone_intent(key_seq_, key_seq_len_, orig_prefix_end);

            auto tone_sort = [intent](const Candidate& a, const Candidate& b) {
                int ta = tone_tier(a, intent), tb = tone_tier(b, intent);
                if (ta != tb) return ta < tb;
                return a.freq > b.freq;
            };
            if (n > 1) std::stable_sort(tmp, tmp + n, tone_sort);

            if (intent != 0) {
                int keep = 0;
                for (int i = 0; i < n; ++i)
                    if (tone_tier(tmp[i], intent) < 2)
                        tmp[keep++] = tmp[i];
                if (keep > 0) n = keep;
            }

            cand_count_ = n;
            std::memcpy(candidates_, tmp, (size_t)n * sizeof(Candidate));
            matched_prefix_keys_ = orig_prefix_end;
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
