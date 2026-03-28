// SPDX-License-Identifier: MIT
// MokyaInput Engine — ImeLogic implementation
//
// Two input modes (MODE key at row 4, col 0):
//
//   Smart Mode  — same key-index byte sequence searched against zh + en dictionaries.
//                 input_buf_ shows primary phonemes for display only.
//                 Symbol keys (4,3) and (4,4) cycle context-sensitive punctuation.
//
//   Direct Mode — each input key (rows 0-3, col 0-4) cycles through all its labels
//                 (primary phoneme, secondary phoneme, primary letter, secondary letter).
//                 OK / SPACE confirms the pending character.
//                 Symbol keys (4,3) and (4,4) cycle a combined symbol list.
//
// Key encoding (half-keyboard):
//   key_index   = row × 5 + col     (rows 0-3, col 0-4 → 0-19)
//   key_seq_byte = key_index + 0x21  (→ ASCII '!' to '4')
//
// Source file must be compiled as UTF-8 (MSVC: /utf-8).

#include <mie/ime_logic.h>
#include <cstring>
#include <cstdlib>

namespace mie {

// ═══════════════════════════════════════════════════════════════════════════
// Static key tables
// ═══════════════════════════════════════════════════════════════════════════

// ── Primary phoneme (Smart Mode display) ─────────────────────────────────
//    Same table as before; only the first (primary) phoneme per key.

struct PhonemeEntry { uint8_t row; uint8_t col; const char* phoneme; };

// clang-format off
static const PhonemeEntry kPhonemeTable[] = {
    { 0, 0, "ㄅ" }, { 0, 1, "ˇ"  }, { 0, 2, "ㄓ" }, { 0, 3, "˙"  }, { 0, 4, "ㄞ" },
    { 1, 0, "ㄆ" }, { 1, 1, "ㄍ" }, { 1, 2, "ㄔ" }, { 1, 3, "ㄧ" }, { 1, 4, "ㄟ" },
    { 2, 0, "ㄇ" }, { 2, 1, "ㄎ" }, { 2, 2, "ㄕ" }, { 2, 3, "ㄨ" }, { 2, 4, "ㄠ" },
    { 3, 0, "ㄈ" }, { 3, 1, "ㄏ" }, { 3, 2, "ㄖ" }, { 3, 3, "ㄩ" }, { 3, 4, "ㄡ" },
    { 0xFF, 0xFF, nullptr },
};
// clang-format on

const char* ImeLogic::key_to_phoneme(uint8_t row, uint8_t col) {
    for (const PhonemeEntry* e = kPhonemeTable; e->phoneme; ++e)
        if (e->row == row && e->col == col) return e->phoneme;
    return nullptr;
}

// ── Direct Mode cycle labels ──────────────────────────────────────────────
//    Order per key: [0] primary phoneme, [1] secondary phoneme,
//                  [2] primary letter,   [3] secondary letter (nullptr if none).

struct DirectEntry {
    uint8_t     row, col;
    const char* labels[4];  // nullptr-terminated
};

// clang-format off
static const DirectEntry kDirectTable[] = {
    { 0, 0, { "ㄅ","ㄉ","1","2"     } },
    { 0, 1, { "ˇ", "ˋ","3","4"     } },
    { 0, 2, { "ㄓ","ˊ","5","6"     } },
    { 0, 3, { "˙", "ㄚ","7","8"    } },
    { 0, 4, { "ㄞ","ㄢ","9","0"    } },

    { 1, 0, { "ㄆ","ㄊ","Q","W"    } },
    { 1, 1, { "ㄍ","ㄐ","E","R"    } },
    { 1, 2, { "ㄔ","ㄗ","T","Y"    } },
    { 1, 3, { "ㄧ","ㄛ","U","I"    } },
    { 1, 4, { "ㄟ","ㄣ","O","P"    } },

    { 2, 0, { "ㄇ","ㄋ","A","S"    } },
    { 2, 1, { "ㄎ","ㄑ","D","F"    } },
    { 2, 2, { "ㄕ","ㄘ","G","H"    } },
    { 2, 3, { "ㄨ","ㄜ","J","K"    } },
    { 2, 4, { "ㄠ","ㄤ","L",nullptr} },

    { 3, 0, { "ㄈ","ㄌ","Z","X"    } },
    { 3, 1, { "ㄏ","ㄒ","C","V"    } },
    { 3, 2, { "ㄖ","ㄙ","B","N"    } },
    { 3, 3, { "ㄩ","ㄝ","M",nullptr} },
    { 3, 4, { "ㄡ","ㄥ",nullptr,nullptr} },
};
// clang-format on

static const DirectEntry* find_direct_entry(uint8_t row, uint8_t col) {
    static const int kCount = (int)(sizeof(kDirectTable) / sizeof(kDirectTable[0]));
    for (int i = 0; i < kCount; ++i)
        if (kDirectTable[i].row == row && kDirectTable[i].col == col)
            return &kDirectTable[i];
    return nullptr;
}

const char* ImeLogic::key_to_direct_label(uint8_t row, uint8_t col, int idx) {
    if (idx < 0 || idx >= 4) return nullptr;
    const DirectEntry* e = find_direct_entry(row, col);
    if (!e) return nullptr;
    return e->labels[idx];
}

int ImeLogic::direct_label_count(uint8_t row, uint8_t col) {
    const DirectEntry* e = find_direct_entry(row, col);
    if (!e) return 0;
    int n = 0;
    while (n < 4 && e->labels[n]) ++n;
    return n;
}

// ── Symbol tables ─────────────────────────────────────────────────────────
//    col 3 = ，SYM key, col 4 = 。.？ key.
//    Smart Mode: context-sensitive (ZH / EN).
//    Direct Mode: combined list.

// clang-format off
static const char* const kSymZH3[] = { "，","、","；","：","「","」","（","）","【","】", nullptr };
static const char* const kSymEN3[] = { ",", ";", ":", "(", ")", "[", "]", "\"","'", nullptr };
static const char* const kSymZH4[] = { "。","？","！","…","—","～", nullptr };
static const char* const kSymEN4[] = { ".", "?", "!",  "-","_", "~", nullptr };
// Direct Mode: zh followed by en (de-duplicated in practice; they're distinct glyphs)
static const char* const kSymDir3[] = { "，","、","；","：","「","」","（","）","【","】",
                                        ",", ";", ":", "\"","'", "(",")", "[","]", nullptr };
static const char* const kSymDir4[] = { "。","？","！","…","—","～",
                                        ".", "?", "!",  "-","_","~", nullptr };
// clang-format on

static int str_arr_len(const char* const* arr) {
    int n = 0; while (arr[n]) ++n; return n;
}

const char* ImeLogic::sym_label(uint8_t col, int idx) const {
    const char* const* arr = nullptr;
    if (mode_ == InputMode::Direct) {
        arr = (col == 3) ? kSymDir3 : kSymDir4;
    } else {
        if (col == 3) arr = (context_lang_ == ZH) ? kSymZH3 : kSymEN3;
        else          arr = (context_lang_ == ZH) ? kSymZH4 : kSymEN4;
    }
    if (!arr || idx < 0) return nullptr;
    int n = 0;
    while (arr[n] && n <= idx) {
        if (n == idx) return arr[idx];
        ++n;
    }
    return nullptr;
}

int ImeLogic::sym_label_count(uint8_t col) const {
    const char* const* arr = nullptr;
    if (mode_ == InputMode::Direct) {
        arr = (col == 3) ? kSymDir3 : kSymDir4;
    } else {
        if (col == 3) arr = (context_lang_ == ZH) ? kSymZH3 : kSymEN3;
        else          arr = (context_lang_ == ZH) ? kSymZH4 : kSymEN4;
    }
    return arr ? str_arr_len(arr) : 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Constructor & callback
// ═══════════════════════════════════════════════════════════════════════════

ImeLogic::ImeLogic(TrieSearcher& zh_searcher, TrieSearcher* en_searcher)
    : zh_searcher_(zh_searcher)
    , en_searcher_(en_searcher)
    , mode_(InputMode::Smart)
    , context_lang_(ZH)
    , key_seq_len_(0)
    , input_len_(0)
    , zh_cand_count_(0)
    , en_cand_count_(0)
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

// ═══════════════════════════════════════════════════════════════════════════
// Display buffer helpers
// ═══════════════════════════════════════════════════════════════════════════

void ImeLogic::append_to_display(const char* utf8) {
    if (!utf8 || !*utf8) return;
    int len = (int)strlen(utf8);
    if (input_len_ + len >= kMaxDisplayBytes) return;
    memcpy(input_buf_ + input_len_, utf8, (size_t)len);
    input_len_ += len;
    input_buf_[input_len_] = '\0';
}

void ImeLogic::backspace_display() {
    if (input_len_ == 0) return;
    int pos = input_len_ - 1;
    while (pos > 0 && ((uint8_t)input_buf_[pos] & 0xC0) == 0x80) --pos;
    input_len_         = pos;
    input_buf_[pos]    = '\0';
}

void ImeLogic::set_display(const char* utf8) {
    if (!utf8) {
        input_len_ = 0; input_buf_[0] = '\0'; return;
    }
    int len = (int)strlen(utf8);
    if (len > kMaxDisplayBytes) len = kMaxDisplayBytes;
    memcpy(input_buf_, utf8, (size_t)len);
    input_len_ = len;
    input_buf_[input_len_] = '\0';
}

// ═══════════════════════════════════════════════════════════════════════════
// clear_input
// ═══════════════════════════════════════════════════════════════════════════

void ImeLogic::clear_input() {
    key_seq_len_    = 0;
    key_seq_buf_[0] = '\0';
    input_len_      = 0;
    input_buf_[0]   = '\0';
    zh_cand_count_  = 0;
    en_cand_count_  = 0;
    direct_         = { 0xFF, 0xFF, 0 };
    sym_pending_    = { 0xFF, 0 };
}

// ═══════════════════════════════════════════════════════════════════════════
// run_search  (uses key_seq_buf_, NOT input_buf_)
// ═══════════════════════════════════════════════════════════════════════════

void ImeLogic::run_search() {
    zh_cand_count_ = 0;
    en_cand_count_ = 0;
    if (key_seq_len_ == 0) return;

    if (zh_searcher_.is_loaded())
        zh_cand_count_ = zh_searcher_.search(key_seq_buf_, zh_candidates_, kMaxCandidates);

    if (en_searcher_ && en_searcher_->is_loaded())
        en_cand_count_ = en_searcher_->search(key_seq_buf_, en_candidates_, kMaxCandidates);
}

// ═══════════════════════════════════════════════════════════════════════════
// do_commit
// ═══════════════════════════════════════════════════════════════════════════

void ImeLogic::do_commit(const char* utf8, int lang_hint) {
    if (utf8 && *utf8) {
        if (lang_hint == 0) context_lang_ = ZH;
        else if (lang_hint == 1) context_lang_ = EN;
        if (commit_cb_) commit_cb_(utf8, commit_ctx_);
    }
    // Reset input state (but not sym_pending_ — that's handled by caller)
    key_seq_len_    = 0;
    key_seq_buf_[0] = '\0';
    input_len_      = 0;
    input_buf_[0]   = '\0';
    zh_cand_count_  = 0;
    en_cand_count_  = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Symbol key handler (row 4, col 3 or 4)
// ═══════════════════════════════════════════════════════════════════════════

void ImeLogic::commit_sym_pending() {
    if (sym_pending_.key_col == 0xFF) return;
    const char* s = sym_label(sym_pending_.key_col, sym_pending_.sym_idx);
    sym_pending_ = { 0xFF, 0 };
    // Clear the display buffer that was showing the symbol.
    input_len_ = 0; input_buf_[0] = '\0';
    if (s && commit_cb_) commit_cb_(s, commit_ctx_);
    // symbol commit does not update context_lang_
}

bool ImeLogic::process_sym_key(uint8_t col) {
    // In Smart Mode: if there's pending phoneme input, commit best candidate first.
    if (mode_ == InputMode::Smart && key_seq_len_ > 0) {
        if (zh_cand_count_ > 0)
            do_commit(zh_candidates_[0].word, 0);
        else if (en_cand_count_ > 0)
            do_commit(en_candidates_[0].word, 1);
        else
            do_commit(input_buf_, 2);
    }

    if (sym_pending_.key_col == col) {
        // Same sym key: advance cycle
        int count = sym_label_count(col);
        if (count > 0) {
            sym_pending_.sym_idx = (sym_pending_.sym_idx + 1) % count;
            const char* s = sym_label(col, sym_pending_.sym_idx);
            set_display(s ? s : "");
        }
    } else {
        // Different key or first press: commit previous sym (if any), start new
        commit_sym_pending();
        sym_pending_ = { col, 0 };
        const char* s = sym_label(col, 0);
        set_display(s ? s : "");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Top-level process_key
// ═══════════════════════════════════════════════════════════════════════════

bool ImeLogic::process_key(const KeyEvent& ev) {
    if (!ev.pressed) return false;

    // MODE key (4,0): toggle Smart ↔ Direct, clear all state.
    if (ev.row == 4 && ev.col == 0) {
        mode_ = (mode_ == InputMode::Smart) ? InputMode::Direct : InputMode::Smart;
        clear_input();
        return true;
    }

    // Symbol keys (row 4, col 3 or 4): handled uniformly in both modes.
    if (ev.row == 4 && (ev.col == 3 || ev.col == 4))
        return process_sym_key(ev.col);

    // Dispatch to mode handler.
    // Any non-symbol, non-MODE key cancels a pending symbol cycle.
    if (sym_pending_.key_col != 0xFF) {
        commit_sym_pending();
        // Fall through: the new key is also processed.
    }

    switch (mode_) {
        case InputMode::Smart:  return process_smart(ev);
        case InputMode::Direct: return process_direct(ev);
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Smart Mode
// ═══════════════════════════════════════════════════════════════════════════

bool ImeLogic::process_smart(const KeyEvent& ev) {
    // BACK (2,5) / DEL (3,5): remove last key from both buffers.
    if ((ev.row == 2 && ev.col == 5) || (ev.row == 3 && ev.col == 5)) {
        if (key_seq_len_ > 0) {
            --key_seq_len_;
            key_seq_buf_[key_seq_len_] = '\0';
            backspace_display();
            run_search();
        }
        return true;
    }

    // SPACE (4,2): commit first Chinese candidate, or first English, or raw input.
    if (ev.row == 4 && ev.col == 2) {
        if (key_seq_len_ == 0) {
            // No pending input: output a space in English context, 　 in Chinese
            const char* sp = (context_lang_ == EN) ? " " : "\xe3\x80\x80"; // U+3000
            if (commit_cb_) commit_cb_(sp, commit_ctx_);
            return true;
        }
        if (zh_cand_count_ > 0)      do_commit(zh_candidates_[0].word, 0);
        else if (en_cand_count_ > 0) do_commit(en_candidates_[0].word, 1);
        else                         do_commit(input_buf_, 2);
        return true;
    }

    // OK (5,4): same as SPACE.
    if (ev.row == 5 && ev.col == 4) {
        if (key_seq_len_ == 0) return false;
        if (zh_cand_count_ > 0)      do_commit(zh_candidates_[0].word, 0);
        else if (en_cand_count_ > 0) do_commit(en_candidates_[0].word, 1);
        else                         do_commit(input_buf_, 2);
        return true;
    }

    // Number keys (row 0, col 0-4): select nth Chinese candidate (1-5).
    // Pressed when there are active candidates.
    if (ev.row == 0 && ev.col <= 4 && zh_cand_count_ > 0) {
        int idx = (int)ev.col;  // col 0 = candidate 1, col 1 = candidate 2, ...
        if (idx < zh_cand_count_) {
            do_commit(zh_candidates_[idx].word, 0);
            return true;
        }
    }

    // LEFT/RIGHT (5,2)/(5,3): if English candidates exist, select en candidate.
    // LEFT selects en_candidate[0], RIGHT selects en_candidate[1] etc.
    // (Simplified: LEFT = first en candidate, RIGHT = second en candidate)
    if (ev.row == 5 && ev.col == 2 && en_cand_count_ > 0) {
        do_commit(en_candidates_[0].word, 1);
        return true;
    }
    if (ev.row == 5 && ev.col == 3 && en_cand_count_ > 1) {
        do_commit(en_candidates_[1].word, 1);
        return true;
    }

    // Input keys (rows 0-3, col 0-4): append to key sequence.
    if (ev.row <= 3 && ev.col <= 4) {
        if (key_seq_len_ < kMaxKeySeq) {
            uint8_t key_index = ev.row * 5 + ev.col;
            key_seq_buf_[key_seq_len_++] = (char)(key_index + 0x21);
            key_seq_buf_[key_seq_len_]   = '\0';
        }
        // Append primary phoneme to display buffer.
        const char* ph = key_to_phoneme(ev.row, ev.col);
        if (ph) append_to_display(ph);
        run_search();
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Direct Mode
// ═══════════════════════════════════════════════════════════════════════════

bool ImeLogic::process_direct(const KeyEvent& ev) {
    // BACK (2,5) / DEL (3,5): cancel current pending character.
    if ((ev.row == 2 && ev.col == 5) || (ev.row == 3 && ev.col == 5)) {
        direct_ = { 0xFF, 0xFF, 0 };
        input_len_ = 0; input_buf_[0] = '\0';
        return true;
    }

    // SPACE (4,2) / OK (5,4): confirm pending character.
    if ((ev.row == 4 && ev.col == 2) || (ev.row == 5 && ev.col == 4)) {
        if (direct_.row != 0xFF) {
            const char* lbl = key_to_direct_label(direct_.row, direct_.col, direct_.label_idx);
            direct_ = { 0xFF, 0xFF, 0 };
            input_len_ = 0; input_buf_[0] = '\0';
            if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
        }
        return true;
    }

    // Input keys (rows 0-3, col 0-4).
    if (ev.row <= 3 && ev.col <= 4) {
        int count = direct_label_count(ev.row, ev.col);
        if (count == 0) return false;

        if (direct_.row == ev.row && direct_.col == ev.col) {
            // Same key: advance cycle.
            direct_.label_idx = (direct_.label_idx + 1) % count;
        } else {
            // Different key: auto-commit previous pending, start new.
            if (direct_.row != 0xFF) {
                const char* lbl = key_to_direct_label(direct_.row, direct_.col, direct_.label_idx);
                if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
            }
            direct_ = { ev.row, ev.col, 0 };
        }
        const char* lbl = key_to_direct_label(direct_.row, direct_.col, direct_.label_idx);
        set_display(lbl ? lbl : "");
        return true;
    }

    return false;
}

} // namespace mie
