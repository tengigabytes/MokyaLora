// ime_direct.cpp — Direct Mode, symbol key handler.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <cstring>

namespace mie {

// ── Symbol key handler (row 4, col 3 or 4) ───────────────────────────────

void ImeLogic::commit_sym_pending() {
    if (sym_pending_.key_col == 0xFF) return;
    const char* s = sym_label(sym_pending_.key_col, sym_pending_.sym_idx);
    sym_pending_ = { 0xFF, 0 };
    input_len_ = 0; input_buf_[0] = '\0';
    if (s) { did_commit(s); if (commit_cb_) commit_cb_(s, commit_ctx_); }
}

bool ImeLogic::process_sym_key(uint8_t col) {
    // In Smart modes: if there's pending key input, commit the first merged
    // candidate (partial commit; keeps remaining keys after matched prefix).
    if ((mode_ == InputMode::SmartZh || mode_ == InputMode::SmartEn) && key_seq_len_ > 0) {
        if (merged_count_ > 0)
            do_commit_partial(merged_[0].cand->word, merged_[0].lang, matched_prefix_len_);
        else
            do_commit_partial(input_buf_, 2, key_seq_len_);
    }

    if (sym_pending_.key_col == col) {
        int count = sym_label_count(col);
        if (count > 0) {
            sym_pending_.sym_idx = (sym_pending_.sym_idx + 1) % count;
            const char* s = sym_label(col, sym_pending_.sym_idx);
            set_display(s ? s : "");
        }
    } else {
        commit_sym_pending();
        sym_pending_ = { col, 0 };
        const char* s = sym_label(col, 0);
        set_display(s ? s : "");
    }
    return true;
}

// ── Direct Mode ───────────────────────────────────────────────────────────

bool ImeLogic::process_direct(const KeyEvent& ev) {
    // BACK (2,5) / DEL (3,5): cancel current pending character.
    if ((ev.row == 2 && ev.col == 5) || (ev.row == 3 && ev.col == 5)) {
        direct_ = { 0xFF, 0xFF, 0 };
        input_len_ = 0; input_buf_[0] = '\0';
        zh_cand_count_ = 0; en_cand_count_ = 0; merged_count_ = 0; merged_sel_ = 0;
        return true;
    }

    // SPACE (4,2) / OK (5,4): confirm pending character.
    if ((ev.row == 4 && ev.col == 2) || (ev.row == 5 && ev.col == 4)) {
        if (direct_.row != 0xFF) {
            const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
            direct_ = { 0xFF, 0xFF, 0 };
            input_len_ = 0; input_buf_[0] = '\0';
            zh_cand_count_ = 0; en_cand_count_ = 0; merged_count_ = 0; merged_sel_ = 0;
            if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
        }
        return true;
    }

    // Input keys (rows 0-3, col 0-4): mode-restricted slot cycling.
    if (ev.row <= 3 && ev.col <= 4) {
        int count = direct_mode_slot_count(ev.row, ev.col);
        if (count == 0) return false;

        if (direct_.row == ev.row && direct_.col == ev.col) {
            direct_.label_idx = (direct_.label_idx + 1) % count;
        } else {
            if (direct_.row != 0xFF) {
                const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
                if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
            }
            direct_ = { ev.row, ev.col, 0 };
        }
        const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
        set_display(lbl ? lbl : "");

        // DirectBopomofo: show single-character ZH candidates for current phoneme.
        if (mode_ == InputMode::DirectBopomofo) {
            zh_cand_count_ = 0; en_cand_count_ = 0;
            merged_count_  = 0; merged_sel_    = 0;
            if (zh_searcher_.is_loaded()) {
                char kb[2] = { (char)(ev.row * 5 + ev.col + 0x21), '\0' };
                Candidate tmp[kMaxCandidates];
                int n = zh_searcher_.search(kb, tmp, kMaxCandidates);
                for (int i = 0; i < n && zh_cand_count_ < kMaxCandidates; ++i)
                    if (utf8_char_count(tmp[i].word) == 1)
                        zh_candidates_[zh_cand_count_++] = tmp[i];
            }
            build_merged();
        }

        return true;
    }

    return false;
}

} // namespace mie
