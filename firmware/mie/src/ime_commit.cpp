// ime_commit.cpp — Commit callbacks and post-commit state updates.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <cstring>

namespace mie {

// Full commit: fire callback, then clear all input state.
void ImeLogic::do_commit(const char* utf8, int lang_hint) {
    if (utf8 && *utf8) {
        if (lang_hint == 0) context_lang_ = ZH;
        else if (lang_hint == 1) context_lang_ = EN;
        did_commit(utf8);
        if (commit_cb_) commit_cb_(utf8, commit_ctx_);
    }
    key_seq_len_        = 0;
    key_seq_buf_[0]     = '\0';
    input_len_          = 0;
    input_buf_[0]       = '\0';
    zh_cand_count_      = 0;
    en_cand_count_      = 0;
    merged_count_       = 0;
    merged_sel_         = 0;
    matched_prefix_len_ = 0;
}

// Partial commit: fire callback for utf8, remove the first prefix_len bytes
// from key_seq_buf_, then re-run greedy search on the remainder.
// Used by OK and SPACE in Smart Mode for continuous multi-word input.
void ImeLogic::do_commit_partial(const char* utf8, int lang_hint, int prefix_len) {
    if (utf8 && *utf8) {
        if (lang_hint == 0) context_lang_ = ZH;
        else if (lang_hint == 1) context_lang_ = EN;
        did_commit(utf8);
        if (commit_cb_) commit_cb_(utf8, commit_ctx_);
    }
    int remove = (prefix_len > 0 && prefix_len <= key_seq_len_) ? prefix_len : key_seq_len_;
    memmove(key_seq_buf_, key_seq_buf_ + remove, (size_t)(key_seq_len_ - remove + 1));
    key_seq_len_ -= remove;
    // Strip a leading first-tone marker (0x20) that was appended for the
    // now-committed word; without this it would bleed into the next phoneme.
    if (key_seq_len_ > 0 && (uint8_t)key_seq_buf_[0] == 0x20) {
        memmove(key_seq_buf_, key_seq_buf_ + 1, (size_t)key_seq_len_);
        key_seq_len_--;
    }
    rebuild_input_buf();
    zh_cand_count_      = 0;
    en_cand_count_      = 0;
    merged_count_       = 0;
    merged_sel_         = 0;
    matched_prefix_len_ = 0;
    run_search();
}

// Update en_capitalize_next_ based on what was just committed.
void ImeLogic::did_commit(const char* utf8) {
    if (!utf8 || !utf8[0]) return;
    const int len = (int)strlen(utf8);
    // Spaces (ASCII or U+3000 ideographic) do not change the flag.
    if (len == 1 && utf8[0] == ' ')  return;
    if (len == 3 && memcmp(utf8, "\xe3\x80\x80", 3) == 0) return;
    // Sentence-ending punctuation → capitalize the next EN word.
    const char* e = utf8 + len;
    bool ends_sentence =
        (e[-1] == '.' || e[-1] == '?' || e[-1] == '!') ||
        (len >= 3 && (memcmp(e - 3, "\xe3\x80\x82", 3) == 0 ||  // 。
                      memcmp(e - 3, "\xef\xbc\x9f", 3) == 0 ||  // ？
                      memcmp(e - 3, "\xef\xbc\x81", 3) == 0));   // ！
    en_capitalize_next_ = ends_sentence;
}

} // namespace mie
