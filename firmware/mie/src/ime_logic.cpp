// ime_logic.cpp — ImeLogic constructor, clear_input, and process_key dispatcher.
// SPDX-License-Identifier: MIT
//
// Implementation is split across focused modules:
//   ime_keys.cpp    — static phoneme/symbol tables, key-to-label lookups
//   ime_search.cpp  — greedy prefix search, candidate merge, input buf rebuild
//   ime_display.cpp — display buffer helpers, compound input string
//   ime_commit.cpp  — do_commit, do_commit_partial, did_commit
//   ime_smart.cpp   — process_smart (SmartZh / SmartEn)
//   ime_direct.cpp  — process_direct, process_sym_key, commit_sym_pending
//   ime_internal.h  — inline helpers (utf8_char_count, tone helpers, is_direct_mode)

#include "ime_internal.h"

namespace mie {

ImeLogic::ImeLogic(TrieSearcher& zh_searcher, TrieSearcher* en_searcher)
    : zh_searcher_(zh_searcher)
    , en_searcher_(en_searcher)
    , mode_(InputMode::SmartZh)
    , context_lang_(ZH)
    , key_seq_len_(0)
    , input_len_(0)
    , zh_cand_count_(0)
    , en_cand_count_(0)
    , merged_count_(0)
    , merged_sel_(0)
    , matched_prefix_len_(0)
    , en_capitalize_next_(false)
    , commit_cb_(nullptr)
    , commit_ctx_(nullptr)
{
    key_seq_buf_[0] = '\0';
    input_buf_[0]   = '\0';
    direct_      = { 0xFF, 0xFF, 0 };
    sym_pending_ = { 0xFF, 0 };
}

void ImeLogic::set_commit_callback(CommitCallback cb, void* ctx) {
    commit_cb_  = cb;
    commit_ctx_ = ctx;
}

void ImeLogic::clear_input() {
    key_seq_len_        = 0;
    key_seq_buf_[0]     = '\0';
    input_len_          = 0;
    input_buf_[0]       = '\0';
    zh_cand_count_      = 0;
    en_cand_count_      = 0;
    merged_count_       = 0;
    merged_sel_         = 0;
    matched_prefix_len_ = 0;
    direct_             = { 0xFF, 0xFF, 0 };
    sym_pending_        = { 0xFF, 0 };
}

bool ImeLogic::process_key(const KeyEvent& ev) {
    if (!ev.pressed) return false;

    // MODE key (4,0): commit pending input, then cycle 中→EN→ABC→abc→ㄅ→中.
    if (ev.row == 4 && ev.col == 0) {
        if (mode_ == InputMode::SmartZh || mode_ == InputMode::SmartEn) {
            if (sym_pending_.key_col != 0xFF) {
                commit_sym_pending();
            } else if (key_seq_len_ > 0) {
                if (merged_count_ > 0) {
                    int sel = (merged_sel_ < merged_count_) ? merged_sel_ : 0;
                    do_commit(merged_[sel].cand->word, merged_[sel].lang);
                } else {
                    do_commit(input_buf_, 2);
                }
            }
        } else {
            if (sym_pending_.key_col != 0xFF) {
                commit_sym_pending();
            } else if (direct_.row != 0xFF) {
                const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
                direct_ = { 0xFF, 0xFF, 0 };
                input_len_ = 0; input_buf_[0] = '\0';
                if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
            }
        }
        switch (mode_) {
            case InputMode::SmartZh:        mode_ = InputMode::SmartEn;        break;
            case InputMode::SmartEn:        mode_ = InputMode::DirectUpper;    break;
            case InputMode::DirectUpper:    mode_ = InputMode::DirectLower;    break;
            case InputMode::DirectLower:    mode_ = InputMode::DirectBopomofo; break;
            default:                        mode_ = InputMode::SmartZh;        break;
        }
        clear_input();
        return true;
    }

    // Symbol keys (row 4, col 3 or 4).
    if (ev.row == 4 && (ev.col == 3 || ev.col == 4))
        return process_sym_key(ev.col);

    // Any non-symbol, non-MODE key cancels a pending symbol cycle.
    if (sym_pending_.key_col != 0xFF) {
        commit_sym_pending();
        // Fall through: the new key is also processed.
    }

    switch (mode_) {
        case InputMode::SmartZh:        return process_smart(ev);
        case InputMode::SmartEn:        return process_smart(ev);
        case InputMode::DirectUpper:    return process_direct(ev);
        case InputMode::DirectLower:    return process_direct(ev);
        case InputMode::DirectBopomofo: return process_direct(ev);
        default:                        return false;
    }
}

} // namespace mie
