// SPDX-License-Identifier: MIT
// ime_commit.cpp — Commit helpers. Fire the listener and update internal state.

#include "ime_internal.h"
#include <cstring>

namespace mie {

void ImeLogic::emit_commit(const char* utf8) {
    if (!utf8 || !*utf8) return;
    did_commit(utf8);
    if (listener_) listener_->on_commit(utf8);
}

void ImeLogic::commit_selected_candidate() {
    if (cand_count_ == 0) return;
    int sel = (selected_ < cand_count_) ? selected_ : 0;
    // Copy out — commit_partial mutates key_seq_ / display_ via rebuild.
    char word[kCandidateMaxBytes];
    std::strncpy(word, candidates_[sel].word, sizeof(word) - 1);
    word[sizeof(word) - 1] = '\0';

    // SmartEn auto-capitalize after sentence-ending punctuation. The flag
    // is set by did_commit() when the previous commit ended with ./?/!
    // (ASCII or full-width), and intervening spaces preserve it. did_commit
    // on this letter word will then clear the flag automatically because
    // the letter does not end in sentence punctuation.
    if (mode_ == InputMode::SmartEn && en_capitalize_next_ &&
        word[0] >= 'a' && word[0] <= 'z') {
        word[0] = (char)(word[0] - 'a' + 'A');
    }

    // Each candidate can match a different prefix length (longer-match
    // entries come from a longer slen than shorter-match ones); use the
    // per-candidate count instead of the global matched_prefix_keys_.
    int prefix = candidates_prefix_keys_[sel];
    commit_partial(word, prefix);
}

void ImeLogic::commit_partial(const char* utf8, int prefix_keys) {
    if (utf8 && *utf8) {
        did_commit(utf8);
        if (listener_) listener_->on_commit(utf8);
    }

    // Remove the matched prefix bytes from key_seq_.
    int remove = (prefix_keys > 0 && prefix_keys <= key_seq_len_)
                 ? prefix_keys : key_seq_len_;
    if (remove > 0) {
        std::memmove(key_seq_, key_seq_ + remove,
                     (size_t)(key_seq_len_ - remove + 1));
        key_seq_len_ -= remove;
    }

    // Strip any leading tone bytes that were meant for the committed word:
    //   0x20 — first-tone marker
    //   0x22 — ˇ/ˋ tone key
    while (key_seq_len_ > 0 &&
           ((uint8_t)key_seq_[0] == 0x20 || (uint8_t)key_seq_[0] == 0x22)) {
        std::memmove(key_seq_, key_seq_ + 1, (size_t)key_seq_len_);
        --key_seq_len_;
    }
    key_seq_[key_seq_len_] = '\0';

    cand_count_           = 0;
    selected_             = 0;
    matched_prefix_bytes_ = 0;
    matched_prefix_keys_  = 0;

    // Re-run search on the remainder (populates display_ via rebuild).
    run_search();
}

void ImeLogic::did_commit(const char* utf8) {
    if (!utf8 || !utf8[0]) return;
    const int len = (int)std::strlen(utf8);

    // Spaces don't change capitalization state.
    if (len == 1 && utf8[0] == ' ')                          return;
    if (len == 3 && std::memcmp(utf8, "\xe3\x80\x80", 3) == 0) return;  // U+3000

    // Sentence-ending punctuation → capitalize the next SmartEn word.
    const char* e = utf8 + len;
    bool ends_sentence =
        (e[-1] == '.' || e[-1] == '?' || e[-1] == '!') ||
        (len >= 3 && (std::memcmp(e - 3, "\xe3\x80\x82", 3) == 0 ||  // 。
                      std::memcmp(e - 3, "\xef\xbc\x9f", 3) == 0 ||  // ？
                      std::memcmp(e - 3, "\xef\xbc\x81", 3) == 0));   // ！
    en_capitalize_next_ = ends_sentence;
}

} // namespace mie
