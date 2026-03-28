// SPDX-License-Identifier: MIT
// MokyaInput Engine — ImeLogic implementation
//
// Five input modes (MODE key at row 4, col 0 cycles 中→EN→ABC→abc→ㄅ→中):
//
//   SmartZh Mode — key-index byte sequence searched against ZH dict only.
//                  input_buf_ shows primary Bopomofo phonemes for display.
//
//   SmartEn Mode — same key encoding, searched against EN dict only.
//                  input_buf_ shows primary letters (T9-style).
//
//   Direct Mode  — each input key (rows 0-3, col 0-4) cycles through all its labels
//                  (primary phoneme, secondary phoneme, primary letter, secondary letter).
//                  OK / SPACE confirms the pending character.
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

// Number of cycling slots for the current direct mode.
//   DirectBopomofo → phoneme slots (indices 0..1 in kDirectTable).
//   DirectUpper/DirectLower → letter/digit slots (indices 2..3).
int ImeLogic::direct_mode_slot_count(uint8_t row, uint8_t col) const {
    const DirectEntry* e = find_direct_entry(row, col);
    if (!e) return 0;
    if (mode_ == InputMode::DirectBopomofo) {
        int n = 0;
        while (n < 2 && e->labels[n]) ++n;
        return n;
    } else {
        int n = 0;
        while (n < 2 && e->labels[2 + n]) ++n;
        return n;
    }
}

// Lowercase-conversion buffer (single-threaded use only).
static char s_lower_buf[8];

static const char* to_lower_label(const char* lbl) {
    if (!lbl) return nullptr;
    int i = 0;
    while (lbl[i] && i < 7) {
        char c = lbl[i];
        s_lower_buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        ++i;
    }
    s_lower_buf[i] = '\0';
    return s_lower_buf;
}

// idx-th label within the current direct mode's slot range.
// For DirectLower: converts uppercase ASCII letters to lowercase.
const char* ImeLogic::direct_mode_slot_label(uint8_t row, uint8_t col, int idx) const {
    const DirectEntry* e = find_direct_entry(row, col);
    if (!e || idx < 0) return nullptr;
    int actual = (mode_ == InputMode::DirectBopomofo) ? idx : (2 + idx);
    if (actual < 0 || actual >= 4 || !e->labels[actual]) return nullptr;
    if (mode_ == InputMode::DirectLower)
        return to_lower_label(e->labels[actual]);
    return e->labels[actual];
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

static bool is_direct_mode(InputMode m) {
    return m == InputMode::DirectUpper ||
           m == InputMode::DirectLower ||
           m == InputMode::DirectBopomofo;
}

const char* ImeLogic::sym_label(uint8_t col, int idx) const {
    const char* const* arr = nullptr;
    if (is_direct_mode(mode_)) {
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
    if (is_direct_mode(mode_)) {
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
    , mode_(InputMode::SmartZh)
    , context_lang_(ZH)
    , key_seq_len_(0)
    , input_len_(0)
    , zh_cand_count_(0)
    , en_cand_count_(0)
    , merged_count_(0)
    , merged_sel_(0)
    , matched_prefix_len_(0)
    , commit_cb_(nullptr)
    , commit_ctx_(nullptr)
{
    key_seq_buf_[0] = '\0';
    input_buf_[0]   = '\0';
    direct_      = { 0xFF, 0xFF, 0 };
    sym_pending_ = { 0xFF, 0 };
}

const char* ImeLogic::mode_indicator() const {
    switch (mode_) {
        case InputMode::SmartZh:        return "\xe4\xb8\xad";  // 中
        case InputMode::SmartEn:        return "EN";
        case InputMode::DirectUpper:    return "ABC";
        case InputMode::DirectLower:    return "abc";
        case InputMode::DirectBopomofo: return "\xe3\x84\x85";  // ㄅ
        default:                        return "?";
    }
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

// ═══════════════════════════════════════════════════════════════════════════
// run_search  (uses key_seq_buf_, NOT input_buf_)
// ═══════════════════════════════════════════════════════════════════════════

void ImeLogic::run_search() {
    zh_cand_count_      = 0;
    en_cand_count_      = 0;
    merged_count_       = 0;
    merged_sel_         = 0;
    matched_prefix_len_ = 0;
    if (key_seq_len_ == 0) return;

    // Greedy prefix: try decreasing prefix lengths until candidates are found.
    // This lets the user type a long sequence (multi-word) without losing the
    // ability to see candidates for the leading word.
    for (int len = key_seq_len_; len >= 1; --len) {
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

// ═══════════════════════════════════════════════════════════════════════════
// do_commit
// ═══════════════════════════════════════════════════════════════════════════

void ImeLogic::do_commit(const char* utf8, int lang_hint) {
    if (utf8 && *utf8) {
        if (lang_hint == 0) context_lang_ = ZH;
        else if (lang_hint == 1) context_lang_ = EN;
        if (commit_cb_) commit_cb_(utf8, commit_ctx_);
    }
    // Full clear — reset all input state (but not sym_pending_; caller handles that).
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

// Partial commit: fire the callback for utf8, then remove the first prefix_len
// bytes from key_seq_buf_ and re-run greedy search on the remainder.
// Used by OK and SPACE in Smart Mode for continuous multi-word input.
void ImeLogic::do_commit_partial(const char* utf8, int lang_hint, int prefix_len) {
    if (utf8 && *utf8) {
        if (lang_hint == 0) context_lang_ = ZH;
        else if (lang_hint == 1) context_lang_ = EN;
        if (commit_cb_) commit_cb_(utf8, commit_ctx_);
    }
    int remove = (prefix_len > 0 && prefix_len <= key_seq_len_) ? prefix_len : key_seq_len_;
    memmove(key_seq_buf_, key_seq_buf_ + remove, (size_t)(key_seq_len_ - remove + 1));
    key_seq_len_ -= remove;
    rebuild_input_buf();
    zh_cand_count_      = 0;
    en_cand_count_      = 0;
    merged_count_       = 0;
    merged_sel_         = 0;
    matched_prefix_len_ = 0;
    run_search();
}

// Rebuild input_buf_ from key_seq_buf_: phonemes for SmartZh, letters for SmartEn.
void ImeLogic::rebuild_input_buf() {
    input_len_      = 0;
    input_buf_[0]   = '\0';
    for (int i = 0; i < key_seq_len_; ++i) {
        uint8_t idx = (uint8_t)key_seq_buf_[i] - 0x21;
        uint8_t row = idx / 5, col = idx % 5;
        if (mode_ == InputMode::SmartEn) {
            const char* lt = key_to_direct_label(row, col, 2); // primary letter
            if (lt) append_to_display(lt);
        } else {
            const char* ph = key_to_phoneme(row, col);
            if (ph) append_to_display(ph);
        }
    }
}

// Number of bytes in input_buf_ that correspond to the matched prefix keys.
int ImeLogic::matched_prefix_display_bytes() const {
    int bytes = 0;
    for (int i = 0; i < matched_prefix_len_ && i < key_seq_len_; ++i) {
        uint8_t idx = (uint8_t)key_seq_buf_[i] - 0x21;
        uint8_t row = idx / 5, col = idx % 5;
        if (mode_ == InputMode::SmartEn) {
            const char* lt = key_to_direct_label(row, col, 2);
            if (lt) bytes += (int)strlen(lt);
        } else {
            const char* ph = key_to_phoneme(row, col);
            if (ph) bytes += (int)strlen(ph);
        }
    }
    return bytes;
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
    // In SmartZh/SmartEn: if there's pending key input, commit the first merged
    // candidate (partial commit: keeps any remaining keys after the matched prefix).
    if ((mode_ == InputMode::SmartZh || mode_ == InputMode::SmartEn) && key_seq_len_ > 0) {
        if (merged_count_ > 0)
            do_commit_partial(merged_[0].cand->word, merged_[0].lang, matched_prefix_len_);
        else
            do_commit_partial(input_buf_, 2, key_seq_len_);
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

    // MODE key (4,0): cycle 中→EN→ABC→abc→ㄅ→中.
    // Commit any pending input first so no text is lost on switch.
    if (ev.row == 4 && ev.col == 0) {
        if (mode_ == InputMode::SmartZh || mode_ == InputMode::SmartEn) {
            if (sym_pending_.key_col != 0xFF) {
                commit_sym_pending();
            } else if (key_seq_len_ > 0) {
                // Commit the currently selected merged candidate (or raw input).
                if (merged_count_ > 0) {
                    int sel = (merged_sel_ < merged_count_) ? merged_sel_ : 0;
                    do_commit(merged_[sel].cand->word, merged_[sel].lang);
                } else {
                    do_commit(input_buf_, 2);
                }
            }
        } else {
            // Direct mode → commit any pending Direct character first.
            if (sym_pending_.key_col != 0xFF) {
                commit_sym_pending();
            } else if (direct_.row != 0xFF) {
                const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
                direct_ = { 0xFF, 0xFF, 0 };
                input_len_ = 0; input_buf_[0] = '\0';
                if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
            }
        }
        // Cycle: 中 → EN → ABC → abc → ㄅ → 中
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
        case InputMode::SmartZh:        return process_smart(ev);
        case InputMode::SmartEn:        return process_smart(ev);
        case InputMode::DirectUpper:    return process_direct(ev);
        case InputMode::DirectLower:    return process_direct(ev);
        case InputMode::DirectBopomofo: return process_direct(ev);
        default:                        return false;
    }
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

    // SPACE (4,2): quick-commit the first merged candidate (partial commit),
    // or output a space when no input is pending.
    if (ev.row == 4 && ev.col == 2) {
        if (key_seq_len_ == 0) {
            // No pending input: SmartEn always outputs ASCII space;
            // SmartZh outputs full-width space in ZH context.
            const char* sp = (mode_ == InputMode::SmartEn || context_lang_ == EN)
                             ? " " : "\xe3\x80\x80"; // U+3000 ideographic space
            if (commit_cb_) commit_cb_(sp, commit_ctx_);
            return true;
        }
        // Commit the first merged candidate and leave any remaining keys in buffer.
        if (merged_count_ > 0)
            do_commit_partial(merged_[0].cand->word, merged_[0].lang, matched_prefix_len_);
        else
            do_commit_partial(input_buf_, 2, key_seq_len_);
        return true;
    }

    // OK (5,4): commit the currently navigated candidate (partial commit).
    if (ev.row == 5 && ev.col == 4) {
        if (key_seq_len_ == 0) return false;
        if (merged_count_ > 0) {
            int sel = (merged_sel_ < merged_count_) ? merged_sel_ : 0;
            do_commit_partial(merged_[sel].cand->word, merged_[sel].lang, matched_prefix_len_);
        } else {
            do_commit_partial(input_buf_, 2, key_seq_len_);
        }
        return true;
    }

    // UP (5,0): previous candidate in merged list (wraps).
    if (ev.row == 5 && ev.col == 0) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ - 1 + merged_count_) % merged_count_;
            return true;
        }
        return false;
    }

    // DOWN (5,1): next candidate in merged list (wraps).
    if (ev.row == 5 && ev.col == 1) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ + 1) % merged_count_;
            return true;
        }
        return false;
    }

    // LEFT (5,2): previous candidate in merged list (wraps).
    if (ev.row == 5 && ev.col == 2) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ - 1 + merged_count_) % merged_count_;
            return true;
        }
        return false;
    }

    // RIGHT (5,3): next candidate in merged list (wraps).
    if (ev.row == 5 && ev.col == 3) {
        if (merged_count_ > 0) {
            merged_sel_ = (merged_sel_ + 1) % merged_count_;
            return true;
        }
        return false;
    }

    // Input keys (rows 0-3, col 0-4): append to key sequence and re-run greedy search.
    // The greedy prefix search handles multi-word input naturally: if the new key
    // extends beyond any matching prefix, run_search() still finds the longest
    // matching prefix among the leading bytes.
    if (ev.row <= 3 && ev.col <= 4) {
        if (key_seq_len_ < kMaxKeySeq) {
            uint8_t key_index = ev.row * 5 + ev.col;
            char    new_byte  = (char)(key_index + 0x21);
            key_seq_buf_[key_seq_len_++] = new_byte;
            key_seq_buf_[key_seq_len_]   = '\0';
            if (mode_ == InputMode::SmartEn) {
                const char* lt = key_to_direct_label(ev.row, ev.col, 2); // primary letter
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
            const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
            direct_ = { 0xFF, 0xFF, 0 };
            input_len_ = 0; input_buf_[0] = '\0';
            if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
        }
        return true;
    }

    // Input keys (rows 0-3, col 0-4): mode-restricted slot cycling.
    if (ev.row <= 3 && ev.col <= 4) {
        int count = direct_mode_slot_count(ev.row, ev.col);
        if (count == 0) return false;

        if (direct_.row == ev.row && direct_.col == ev.col) {
            // Same key: advance within the mode's slot range.
            direct_.label_idx = (direct_.label_idx + 1) % count;
        } else {
            // Different key: auto-commit previous pending, start new.
            if (direct_.row != 0xFF) {
                const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
                if (lbl && commit_cb_) commit_cb_(lbl, commit_ctx_);
            }
            direct_ = { ev.row, ev.col, 0 };
        }
        const char* lbl = direct_mode_slot_label(direct_.row, direct_.col, direct_.label_idx);
        set_display(lbl ? lbl : "");
        return true;
    }

    return false;
}

} // namespace mie
