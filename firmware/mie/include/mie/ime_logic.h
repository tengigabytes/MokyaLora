// ime_logic.h — MokyaInput Engine IME-Logic public API
// SPDX-License-Identifier: MIT
//
// IME-Logic sits between the HAL key events and the TrieSearcher.
// It maintains:
//   - Current input mode (Bopomofo / English / Alphanumeric / Calculator)
//   - Accumulated phoneme sequence (UTF-8, Bopomofo mode)
//   - Candidate list produced by the last TrieSearcher query
//
// Phase 1: Bopomofo primary-phoneme mapping.
//   Each ambiguous physical key is mapped to its first (primary) Bopomofo
//   phoneme.  Full disambiguation and smart correction are Phase 3 work.
//
// Key event handling in Bopomofo mode:
//   Phoneme key  → append primary phoneme to input buffer, run search
//   BACK (2,5)   → remove last UTF-8 character from input buffer, run search
//   DEL  (3,5)   → same as BACK
//   MODE (4,0)   → cycle to next input mode, clear input
//   SPACE(4,2)   → commit first candidate (clears input buffer)
//   OK   (5,4)   → commit first candidate (clears input buffer)
//   Other keys   → ignored in this phase

#pragma once
#include <mie/hal_port.h>
#include <mie/trie_searcher.h>

namespace mie {

/// Input modes, cycled by the MODE key (three total).
enum class InputMode : uint8_t {
    Bopomofo     = 0,  ///< Bopomofo syllable prediction → Traditional Chinese (Phase 1 active)
    English      = 1,  ///< English word prediction via half-keyboard pair expansion (Phase 1 ext.)
    Alphanumeric = 2,  ///< Multi-tap single character — English letters and digits (Phase 1 ext.)
};

/// Maximum number of candidates returned per query.
static constexpr int kMaxCandidates = 10;

/// IME-Logic: stateful input method engine.
///
/// Typical usage:
///   TrieSearcher searcher;
///   searcher.load_from_file("dict_dat.bin", "dict_values.bin");
///   ImeLogic ime(searcher);
///   while (...) {
///       KeyEvent ev;
///       if (hal.poll(ev)) {
///           bool refresh = ime.process_key(ev);
///           if (refresh) redraw(ime.input_str(), ime.candidate_count(), ...);
///       }
///   }
class ImeLogic {
public:
    explicit ImeLogic(TrieSearcher& searcher);

    /// Feed one key event from the HAL.
    /// @return true if the display should be refreshed.
    bool process_key(const KeyEvent& ev);

    /// Current input mode.
    InputMode mode() const { return mode_; }

    /// Accumulated phoneme input (null-terminated UTF-8, may be empty).
    const char* input_str()      const { return input_buf_; }
    int         input_bytes()    const { return input_bytes_; }

    /// Number of valid candidates after the last search (0 if input is empty
    /// or no dictionary is loaded).
    int candidate_count() const { return cand_count_; }

    /// Access candidate i (0-based).  Caller must ensure i < candidate_count().
    const Candidate& candidate(int i) const { return candidates_[i]; }

    /// Clear the input buffer and candidate list.
    void clear_input();

private:
    TrieSearcher& searcher_;

    InputMode mode_           = InputMode::Bopomofo;
    char      input_buf_[256] = {};
    int       input_bytes_    = 0;

    Candidate candidates_[kMaxCandidates] = {};
    int       cand_count_ = 0;

    /// State for Alphanumeric multi-tap mode.
    struct MultiTapState {
        uint8_t last_row  = 0xFF;  ///< Row of last key (0xFF = idle)
        uint8_t last_col  = 0xFF;  ///< Col of last key (0xFF = idle)
        int     tap_count = 0;     ///< Consecutive taps on last key
        char    pending   = '\0';  ///< Character pending confirmation
    } multi_tap_;

    // ── Mode dispatch ─────────────────────────────────────────────────────

    /// Bopomofo mode: append primary phoneme, run Trie search, handle commit.
    bool process_bopomofo(const KeyEvent& ev);

    /// English mode: accumulate half-keyboard key pairs, run prefix search.
    /// Phase 1 extension — stub, returns false.
    bool process_english(const KeyEvent& ev);

    /// Alphanumeric mode: multi-tap single character cycling.
    /// Phase 1 extension — stub, returns false.
    bool process_alpha(const KeyEvent& ev);

    // ── Bopomofo helpers ─────────────────────────────────────────────────

    /// Append a null-terminated UTF-8 phoneme string to input_buf_.
    void append_phoneme(const char* utf8);

    /// Remove the last complete UTF-8 code point from input_buf_.
    void backspace_phoneme();

    /// Query the TrieSearcher with the current input_buf_ and update candidates_.
    void run_search();

    /// Map a physical key (row, col) to its primary Bopomofo phoneme
    /// (null-terminated UTF-8 literal, static storage).
    /// Returns nullptr for non-phoneme keys (control, navigation, etc.).
    static const char* key_to_phoneme(uint8_t row, uint8_t col);
};

} // namespace mie
