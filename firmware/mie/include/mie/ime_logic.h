// SPDX-License-Identifier: MIT
// MokyaInput Engine — IME Logic public API
//
// ImeLogic sits between HAL key events and TrieSearcher.
// Two input modes (cycled by MODE key at row 4, col 0):
//
//   Smart Mode  — half-keyboard unified Chinese (Bopomofo) + English prediction.
//                 Same key sequence is searched against both zh and en dictionaries.
//                 Candidates displayed in two groups: [中文] and [English].
//
//   Direct Mode — each input key cycles through all its labels
//                 (phonemes then letters).  OK confirms the pending character.
//                 Used for passwords / account names.
//
// Symbol keys (row 4, col 3 = ，SYM ; row 4, col 4 = 。.？):
//   In Smart Mode: cycle context-sensitive punctuation list (ZH or EN set),
//                  confirming on OK or on press of a different key.
//   In Direct Mode: cycle a combined symbol list.

#pragma once
#include <stdint.h>
#include <mie/hal_port.h>
#include <mie/trie_searcher.h>

namespace mie {

// ── Input modes ──────────────────────────────────────────────────────────────
enum class InputMode : uint8_t {
    Smart  = 0,  // Unified Chinese + English prediction (half-keyboard)
    Direct = 1,  // Direct character input — cycle key labels (password mode)
};

// ── ImeLogic ─────────────────────────────────────────────────────────────────
class ImeLogic {
public:
    // CommitCallback: called whenever text is confirmed.
    // utf8 — null-terminated UTF-8 string to append to the output.
    // ctx  — opaque pointer provided to set_commit_callback().
    typedef void (*CommitCallback)(const char* utf8, void* ctx);

    // zh_searcher: required Chinese dictionary.
    // en_searcher: optional English dictionary; nullptr disables English candidates.
    explicit ImeLogic(TrieSearcher& zh_searcher,
                      TrieSearcher* en_searcher = nullptr);

    // Register a commit callback.
    void set_commit_callback(CommitCallback cb, void* ctx);

    // Feed one key event.  Returns true if the display should be refreshed.
    bool process_key(const KeyEvent& ev);

    // ── Query ─────────────────────────────────────────────────────────────
    InputMode mode() const { return mode_; }

    // Display string (UTF-8, null-terminated):
    //   Smart Mode  — accumulated primary phonemes, e.g. "ㄆㄍㄔ"
    //   Direct Mode — current pending label, e.g. "ㄊ" or "W"
    //   Symbol pending — current symbol, e.g. "、"
    const char* input_str()   const { return input_buf_; }
    int         input_bytes() const { return input_len_; }

    // Chinese candidates (Smart Mode; always 0 in Direct Mode).
    int              zh_candidate_count() const { return zh_cand_count_; }
    const Candidate& zh_candidate(int i)  const { return zh_candidates_[i]; }

    // English candidates (Smart Mode; requires en_searcher != nullptr).
    int              en_candidate_count() const { return en_cand_count_; }
    const Candidate& en_candidate(int i)  const { return en_candidates_[i]; }

    // Merged interleaved ZH+EN candidates (ZH[0], EN[0], ZH[1], EN[1], ...).
    // Empty slots are skipped, so count <= zh_count + en_count.
    // merged_candidate_lang(i): 0 = ZH, 1 = EN.
    int              merged_candidate_count()  const { return merged_count_; }
    const Candidate& merged_candidate(int i)   const { return *merged_[i].cand; }
    int              merged_candidate_lang(int i) const { return merged_[i].lang; }

    // Clear all input state.
    void clear_input();

    // Current candidate group (0 = ZH, 1 = EN) and index within that group.
    // Updated by UP/DOWN/LEFT/RIGHT navigation in Smart Mode.
    int candidate_group() const { return cand_sel_.group; }
    int candidate_index() const { return cand_sel_.index; }

    // Short mode label for UI display: "[智慧]" or "[直接]".
    const char* mode_indicator() const;

private:
    // ── Mode handlers ─────────────────────────────────────────────────────
    bool process_smart(const KeyEvent& ev);
    bool process_direct(const KeyEvent& ev);

    // ── Symbol key handler (row 4 col 3/4, used in both modes) ───────────
    // Returns true if the event was consumed.
    bool process_sym_key(uint8_t col);

    // Commit current pending symbol (if any).
    void commit_sym_pending();

    // ── Search & commit ───────────────────────────────────────────────────
    // Search both dictionaries using key_seq_buf_; update candidate arrays.
    void run_search();
    // Rebuild merged_[] from zh_candidates_ + en_candidates_ (interleaved).
    void build_merged();

    // Commit utf8: invoke callback, update context_lang_, clear input.
    // lang_hint: 0=ZH, 1=EN, 2=neutral (Direct/symbol, no lang update).
    void do_commit(const char* utf8, int lang_hint = 2);

    // ── Display buffer helpers ─────────────────────────────────────────────
    void append_to_display(const char* utf8);
    void backspace_display();           // remove last UTF-8 codepoint
    void set_display(const char* utf8); // replace entire display string

    // ── Static key tables ─────────────────────────────────────────────────
    // Primary Bopomofo phoneme for (row 0-3, col 0-4). nullptr = not a phoneme key.
    static const char* key_to_phoneme(uint8_t row, uint8_t col);

    // idx-th cycle label for Direct Mode (phoneme[0], phoneme[1], letter[0], letter[1]).
    // nullptr when idx >= label count.
    static const char* key_to_direct_label(uint8_t row, uint8_t col, int idx);
    static int         direct_label_count(uint8_t row, uint8_t col);

    // idx-th symbol for sym key at col (3 or 4), given current context_lang_.
    const char* sym_label(uint8_t col, int idx) const;
    int         sym_label_count(uint8_t col)    const;

    // ── Members ───────────────────────────────────────────────────────────
    TrieSearcher&  zh_searcher_;
    TrieSearcher*  en_searcher_;

    InputMode mode_;

    // Context language: updated on each commit; controls punctuation set.
    enum Lang : uint8_t { ZH = 0, EN = 1 } context_lang_;

    // Smart Mode: key-index byte sequence for dictionary search.
    static constexpr int kMaxKeySeq = 64;
    char key_seq_buf_[kMaxKeySeq + 1];
    int  key_seq_len_;

    // Display buffer.
    static constexpr int kMaxDisplayBytes = 256;
    char input_buf_[kMaxDisplayBytes + 1];
    int  input_len_;

    // Candidate arrays (grouped).
    static constexpr int kMaxCandidates = 5;
    Candidate zh_candidates_[kMaxCandidates];
    int       zh_cand_count_;
    Candidate en_candidates_[kMaxCandidates];
    int       en_cand_count_;

    // Merged interleaved view (rebuilt after every run_search()).
    struct MergedCandidate {
        const Candidate* cand;
        int lang;  // 0 = ZH, 1 = EN
    };
    static constexpr int kMaxMerged = kMaxCandidates * 2;
    MergedCandidate merged_[kMaxMerged];
    int             merged_count_;

    // Direct Mode cycle state.
    struct DirectState {
        uint8_t row;        // 0xFF = idle
        uint8_t col;
        int     label_idx;
    } direct_;

    // Symbol key pending state (row 4, col 3 or 4).
    struct SymPendingState {
        uint8_t key_col;    // 3 or 4; 0xFF = idle
        int     sym_idx;
    } sym_pending_;

    // Candidate navigation state (Smart Mode).
    // group: 0 = ZH, 1 = EN.  index: highlighted candidate within the group.
    // Reset to {0, 0} on every run_search(), do_commit(), and clear_input().
    struct CandidateSelection {
        int group;
        int index;
    } cand_sel_;

    // Commit callback.
    CommitCallback commit_cb_;
    void*          commit_ctx_;
};

} // namespace mie
