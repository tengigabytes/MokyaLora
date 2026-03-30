// ime_smart.cpp — SmartZh and SmartEn input processing.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <cstring>

namespace mie {

bool ImeLogic::process_smart(const KeyEvent& ev) {
    // BACK (2,5) / DEL (3,5): remove last key from both buffers.
    // Use rebuild_input_buf() so removing an invisible 0x20 first-tone marker
    // does not accidentally erase the preceding phoneme from the display.
    if ((ev.row == 2 && ev.col == 5) || (ev.row == 3 && ev.col == 5)) {
        if (key_seq_len_ > 0) {
            --key_seq_len_;
            key_seq_buf_[key_seq_len_] = '\0';
            rebuild_input_buf();
            run_search();
        }
        return true;
    }

    // SPACE (4,2):
    //   SmartZh, no input  → output full-width space U+3000.
    //   SmartZh, pending   → append first-tone marker (0x20); second press = no-op.
    //   SmartEn, no input  → output half-width space.
    //   SmartEn, pending   → commit first candidate + auto-append space.
    if (ev.row == 4 && ev.col == 2) {
        if (key_seq_len_ == 0) {
            const char* sp = (mode_ == InputMode::SmartEn || context_lang_ == EN)
                             ? " " : "\xe3\x80\x80";  // U+3000 ideographic space
            did_commit(sp);
            if (commit_cb_) commit_cb_(sp, commit_ctx_);
            return true;
        }
        if (mode_ == InputMode::SmartZh) {
            if ((uint8_t)key_seq_buf_[key_seq_len_ - 1] != 0x20
                    && key_seq_len_ < kMaxKeySeq) {
                key_seq_buf_[key_seq_len_++] = 0x20;
                key_seq_buf_[key_seq_len_]   = '\0';
                run_search();
            }
            return true;
        }
        // SmartEn: commit first candidate + auto-space.
        const char* word = (merged_count_ > 0) ? merged_[0].cand->word : input_buf_;
        int  lang  = (merged_count_ > 0) ? merged_[0].lang : 2;
        int  plen  = (merged_count_ > 0) ? matched_prefix_len_ : key_seq_len_;
        char cap[kCandidateMaxBytes] = {};
        if (en_capitalize_next_) {
            strncpy(cap, word, sizeof(cap) - 1);
            if ((unsigned char)cap[0] >= 'a' && (unsigned char)cap[0] <= 'z')
                cap[0] = (char)(cap[0] - 'a' + 'A');
            word = cap;
        }
        do_commit_partial(word, lang, plen);
        did_commit(" ");
        if (commit_cb_) commit_cb_(" ", commit_ctx_);
        return true;
    }

    // OK (5,4): commit the currently navigated candidate (partial commit).
    if (ev.row == 5 && ev.col == 4) {
        if (key_seq_len_ == 0) return false;
        const char* word;
        int lang, plen;
        if (merged_count_ > 0) {
            int sel = (merged_sel_ < merged_count_) ? merged_sel_ : 0;
            word = merged_[sel].cand->word;
            lang = merged_[sel].lang;
            plen = matched_prefix_len_;
        } else {
            word = input_buf_;
            lang = 2;
            plen = key_seq_len_;
        }
        char cap[kCandidateMaxBytes] = {};
        if (mode_ == InputMode::SmartEn && en_capitalize_next_) {
            strncpy(cap, word, sizeof(cap) - 1);
            if ((unsigned char)cap[0] >= 'a' && (unsigned char)cap[0] <= 'z')
                cap[0] = (char)(cap[0] - 'a' + 'A');
            word = cap;
        }
        do_commit_partial(word, lang, plen);
        if (mode_ == InputMode::SmartEn) {
            did_commit(" ");
            if (commit_cb_) commit_cb_(" ", commit_ctx_);
        }
        return true;
    }

    // TAB (4,1): advance to start of next candidate page.
    if (ev.row == 4 && ev.col == 1) {
        if (merged_count_ > 0) {
            int next = (cand_page() + 1) * kCandPageSize;
            merged_sel_ = (next >= merged_count_) ? 0 : next;
            return true;
        }
        return false;
    }

    // UP (5,0): previous candidate (wraps).
    if (ev.row == 5 && ev.col == 0) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ - 1 + merged_count_) % merged_count_;
            return true;
        }
        return false;
    }

    // DOWN (5,1): next candidate (wraps).
    if (ev.row == 5 && ev.col == 1) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ + 1) % merged_count_;
            return true;
        }
        return false;
    }

    // LEFT (5,2): previous candidate (wraps).
    if (ev.row == 5 && ev.col == 2) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ - 1 + merged_count_) % merged_count_;
            return true;
        }
        return false;
    }

    // RIGHT (5,3): next candidate (wraps).
    if (ev.row == 5 && ev.col == 3) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ + 1) % merged_count_;
            return true;
        }
        return false;
    }

    // Input keys (rows 0-3, col 0-4): append to key sequence and re-run search.
    if (ev.row <= 3 && ev.col <= 4) {
        if (key_seq_len_ < kMaxKeySeq) {
            uint8_t key_index = ev.row * 5 + ev.col;
            char    new_byte  = (char)(key_index + 0x21);
            key_seq_buf_[key_seq_len_++] = new_byte;
            key_seq_buf_[key_seq_len_]   = '\0';
            if (mode_ == InputMode::SmartEn) {
                const char* lt = key_to_direct_label(ev.row, ev.col, 3);
                if (lt) append_to_display(lt);
            } else {
                const char* ph = key_to_phoneme(ev.row, ev.col);
                if (ph) append_to_display(ph);
            }
            run_search();
        }
        return true;
    }

    return false;
}

} // namespace mie
