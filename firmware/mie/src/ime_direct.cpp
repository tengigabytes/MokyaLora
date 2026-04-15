// ime_direct.cpp — Direct Mode, symbol key handler.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <cstring>

namespace mie {

// ── Symbol key handler (MOKYA_KEY_SYM1 / MOKYA_KEY_SYM2) ──────────────────

void ImeLogic::commit_sym_pending() {
    if (sym_pending_.keycode == MOKYA_KEY_NONE) return;
    const char* s = sym_label(sym_pending_.keycode, sym_pending_.sym_idx);
    sym_pending_ = { MOKYA_KEY_NONE, 0 };
    input_len_ = 0; input_buf_[0] = '\0';
    if (s) { did_commit(s); if (commit_cb_) commit_cb_(s, commit_ctx_); }
}

bool ImeLogic::process_sym_key(mokya_keycode_t kc) {
    // In Smart modes: if there's pending key input, commit the first merged
    // candidate (partial commit; keeps remaining keys after matched prefix).
    if ((mode_ == InputMode::SmartZh || mode_ == InputMode::SmartEn) && key_seq_len_ > 0) {
        if (merged_count_ > 0)
            do_commit_partial(merged_[0].cand->word, merged_[0].lang, matched_prefix_len_);
        else
            do_commit_partial(input_buf_, 2, key_seq_len_);
    }

    if (sym_pending_.keycode == kc) {
        int count = sym_label_count(kc);
        if (count > 0) {
            sym_pending_.sym_idx = (sym_pending_.sym_idx + 1) % count;
            const char* s = sym_label(kc, sym_pending_.sym_idx);
            set_display(s ? s : "");
        }
    } else {
        commit_sym_pending();
        sym_pending_ = { kc, 0 };
        const char* s = sym_label(kc, 0);
        set_display(s ? s : "");
    }
    return true;
}

// ── Direct Mode ───────────────────────────────────────────────────────────

bool ImeLogic::process_direct(const KeyEvent& ev) {
    const mokya_keycode_t kc = ev.keycode;

    // BACK / DEL: cancel current pending character.
    if (kc == MOKYA_KEY_BACK || kc == MOKYA_KEY_DEL) {
        direct_ = { MOKYA_KEY_NONE, 0 };
        input_len_ = 0; input_buf_[0] = '\0';
        zh_cand_count_ = 0; en_cand_count_ = 0; merged_count_ = 0; merged_sel_ = 0;
        return true;
    }

    // SPACE / OK: confirm pending character.
    if (kc == MOKYA_KEY_SPACE || kc == MOKYA_KEY_OK) {
        if (direct_.keycode != MOKYA_KEY_NONE) {
            const char* lbl = direct_mode_slot_label(direct_.keycode, direct_.label_idx);
            direct_ = { MOKYA_KEY_NONE, 0 };
            input_len_ = 0; input_buf_[0] = '\0';
            zh_cand_count_ = 0; en_cand_count_ = 0; merged_count_ = 0; merged_sel_ = 0;
            if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
        }
        return true;
    }

    // Dictionary-input keys: mode-restricted slot cycling.
    int slot = keycode_to_input_slot(kc);
    if (slot >= 0) {
        int count = direct_mode_slot_count(kc);
        if (count == 0) return false;

        if (direct_.keycode == kc) {
            direct_.label_idx = (direct_.label_idx + 1) % count;
        } else {
            if (direct_.keycode != MOKYA_KEY_NONE) {
                const char* lbl = direct_mode_slot_label(direct_.keycode, direct_.label_idx);
                if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
            }
            direct_ = { kc, 0 };
        }
        const char* lbl = direct_mode_slot_label(direct_.keycode, direct_.label_idx);
        set_display(lbl ? lbl : "");

        // DirectBopomofo: show single-character ZH candidates for current phoneme.
        if (mode_ == InputMode::DirectBopomofo) {
            zh_cand_count_ = 0; en_cand_count_ = 0;
            merged_count_  = 0; merged_sel_    = 0;
            if (zh_searcher_.is_loaded()) {
                char kb[2] = { (char)(slot + 0x21), '\0' };
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
