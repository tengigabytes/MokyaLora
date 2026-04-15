// ime_search.cpp — Greedy prefix search, candidate merge, input buffer rebuild.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <algorithm>
#include <cstring>

namespace mie {

void ImeLogic::run_search() {
    zh_cand_count_      = 0;
    en_cand_count_      = 0;
    merged_count_       = 0;
    merged_sel_         = 0;
    matched_prefix_len_ = 0;
    if (key_seq_len_ == 0) return;

    // Greedy prefix: try decreasing prefix lengths until candidates are found.
    //
    // Tone-1 priority: when key_seq is exactly [K, tone_byte, 0x20] (length 3),
    // the greedy would otherwise stop at [K, tone_byte] (len=2) and return
    // tone-3/4 candidates instead of tone-1 words for K alone.
    // We skip len=2 so the bare-phoneme search [K] (len=1) runs first.
    // Only applies to the single-key case (key_seq_len_==3); multi-key
    // abbreviated sequences that end in a tone byte are NOT skipped so that
    // abbreviated-input dict entries are still reachable.
    int skip_len = -1;
    if (key_seq_len_ == 3 && (uint8_t)key_seq_buf_[2] == 0x20) {
        uint8_t pre = (uint8_t)key_seq_buf_[1];
        if (pre == 0x22 || pre == 0x23)
            skip_len = 2;
    }

    for (int len = key_seq_len_; len >= 1; --len) {
        if (len == skip_len) continue;
        char saved = key_seq_buf_[len];
        key_seq_buf_[len] = '\0';

        Candidate zh_tmp[kMaxCandidates];
        Candidate en_tmp[kMaxCandidates];
        int zh_n = (mode_ == InputMode::SmartZh && zh_searcher_.is_loaded())
                   ? zh_searcher_.search(key_seq_buf_, zh_tmp, kMaxCandidates) : 0;
        int en_n = (mode_ == InputMode::SmartEn && en_searcher_ && en_searcher_->is_loaded())
                   ? en_searcher_->search(key_seq_buf_, en_tmp, kMaxCandidates) : 0;

        key_seq_buf_[len] = saved;

        if (zh_n > 0 || en_n > 0) {
            int intent = extract_tone_intent(key_seq_buf_, key_seq_len_, len);
            auto tone_sort = [intent](const Candidate& a, const Candidate& b) {
                int ta = tone_tier(a, intent), tb = tone_tier(b, intent);
                if (ta != tb) return ta < tb;
                return a.freq > b.freq;
            };
            if (zh_n > 1) std::stable_sort(zh_tmp, zh_tmp + zh_n, tone_sort);
            if (en_n > 1) std::stable_sort(en_tmp, en_tmp + en_n, tone_sort);
            // Strict tone filter: when tone intent is specified, keep only
            // tier-0/1 candidates.  Fall back to full list only if none match
            // (handles v1 dicts with tone==0).
            if (intent != 0) {
                int keep = 0;
                for (int i = 0; i < zh_n; ++i)
                    if (tone_tier(zh_tmp[i], intent) < 2)
                        zh_tmp[keep++] = zh_tmp[i];
                if (keep > 0) zh_n = keep;
            }
            zh_cand_count_ = zh_n;
            en_cand_count_ = en_n;
            memcpy(zh_candidates_, zh_tmp, (size_t)zh_n * sizeof(Candidate));
            memcpy(en_candidates_, en_tmp, (size_t)en_n * sizeof(Candidate));
            matched_prefix_len_ = len;
            break;
        }
    }
    build_merged();
}

void ImeLogic::build_merged() {
    merged_count_ = 0;
    int zi = 0, ei = 0;
    while (merged_count_ < kMaxMerged && (zi < zh_cand_count_ || ei < en_cand_count_)) {
        if (zi < zh_cand_count_)
            merged_[merged_count_++] = { &zh_candidates_[zi++], 0 };
        if (merged_count_ < kMaxMerged && ei < en_cand_count_)
            merged_[merged_count_++] = { &en_candidates_[ei++], 1 };
    }
}

// Rebuild input_buf_ from key_seq_buf_: primary phonemes for SmartZh, primary
// letters for SmartEn.  The first-tone marker (0x20) is skipped in the
// primary display (it appears only in compound_input_str() as "ˉ").
void ImeLogic::rebuild_input_buf() {
    input_len_    = 0;
    input_buf_[0] = '\0';
    for (int i = 0; i < key_seq_len_; ++i) {
        uint8_t b = (uint8_t)key_seq_buf_[i];
        if (b == 0x20) continue;
        int slot = (int)b - 0x21;
        if (slot < 0 || slot >= 20) continue;
        mokya_keycode_t kc = input_slot_to_keycode(slot);
        if (mode_ == InputMode::SmartEn) {
            const char* lt = key_to_direct_label(kc, 3);
            if (lt) append_to_display(lt);
        } else {
            const char* ph = key_to_phoneme(kc);
            if (ph) append_to_display(ph);
        }
    }
}

// Bytes of input_buf_ corresponding to the matched-prefix keys.
int ImeLogic::matched_prefix_display_bytes() const {
    int bytes = 0;
    for (int i = 0; i < matched_prefix_len_ && i < key_seq_len_; ++i) {
        uint8_t b = (uint8_t)key_seq_buf_[i];
        if (b == 0x20) continue;
        int slot = (int)b - 0x21;
        if (slot < 0 || slot >= 20) continue;
        mokya_keycode_t kc = input_slot_to_keycode(slot);
        if (mode_ == InputMode::SmartEn) {
            const char* lt = key_to_direct_label(kc, 3);
            if (lt) bytes += (int)strlen(lt);
        } else {
            const char* ph = key_to_phoneme(kc);
            if (ph) bytes += (int)strlen(ph);
        }
    }
    return bytes;
}

} // namespace mie
