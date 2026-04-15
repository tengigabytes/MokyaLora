// ime_smart.cpp — SmartZh and SmartEn input processing.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <cstring>

namespace mie {

bool ImeLogic::process_smart(const KeyEvent& ev) {
    const mokya_keycode_t kc = ev.keycode;

    // BACK / DEL: remove last key from both buffers.
    // Use rebuild_input_buf() so removing an invisible 0x20 first-tone marker
    // does not accidentally erase the preceding phoneme from the display.
    if (kc == MOKYA_KEY_BACK || kc == MOKYA_KEY_DEL) {
        if (key_seq_len_ > 0) {
            --key_seq_len_;
            key_seq_buf_[key_seq_len_] = '\0';
            rebuild_input_buf();
            run_search();
        }
        return true;
    }

    // SPACE:
    //   SmartZh, no input  → output full-width space U+3000.
    //   SmartZh, pending   → append first-tone marker (0x20); second press = no-op.
    //   SmartEn, no input  → output half-width space.
    //   SmartEn, pending   → commit first candidate + auto-append space.
    if (kc == MOKYA_KEY_SPACE) {
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

    // OK: commit the currently navigated candidate (partial commit).
    if (kc == MOKYA_KEY_OK) {
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

    // TAB: advance to start of next candidate page.
    if (kc == MOKYA_KEY_TAB) {
        if (merged_count_ > 0) {
            int next = (cand_page() + 1) * kCandPageSize;
            merged_sel_ = (next >= merged_count_) ? 0 : next;
            return true;
        }
        return false;
    }

    // UP / LEFT: previous candidate (wraps).
    if (kc == MOKYA_KEY_UP || kc == MOKYA_KEY_LEFT) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ - 1 + merged_count_) % merged_count_;
            return true;
        }
        return false;
    }

    // DOWN / RIGHT: next candidate (wraps).
    if (kc == MOKYA_KEY_DOWN || kc == MOKYA_KEY_RIGHT) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ + 1) % merged_count_;
            return true;
        }
        return false;
    }

    // Dictionary-input keys: append slot byte to key sequence and re-run search.
    int slot = keycode_to_input_slot(kc);
    if (slot >= 0) {
        if (key_seq_len_ < kMaxKeySeq) {
            char new_byte = (char)(slot + 0x21);
            key_seq_buf_[key_seq_len_++] = new_byte;
            key_seq_buf_[key_seq_len_]   = '\0';
            if (mode_ == InputMode::SmartEn) {
                const char* lt = key_to_direct_label(kc, 3);
                if (lt) append_to_display(lt);
            } else {
                const char* ph = key_to_phoneme(kc);
                if (ph) append_to_display(ph);
            }
            run_search();
        }
        return true;
    }

    return false;
}

} // namespace mie
