// SPDX-License-Identifier: MIT
// MokyaInput Engine — IME Logic public API (v2, listener-based)
//
// ImeLogic is a platform-agnostic input method service. The UI pushes key
// events and periodic ticks; ImeLogic drives internal state and fires
// events on an IImeListener that the UI implements. The UI owns the text
// buffer and cursor; MIE owns only pending composition and candidate
// state.
//
// Three input modes, cycled by MOKYA_KEY_MODE (SmartZh → SmartEn → Direct
// → SmartZh):
//   SmartZh — Bopomofo half-keyboard prefix prediction (ZH dictionary).
//   SmartEn — English T9 letter prediction on rows 1-3 keys plus
//             Direct-style multi-tap on the row 0 digit keys (digits are
//             not in the English dictionary).
//   Direct  — Full multi-tap cycling across all 20 input keys (letter
//             keys cycle a/s/A/S; digit keys cycle 1/2). Used for
//             out-of-dictionary content such as passwords or rare
//             characters.
//
// Time is injected by the caller; MIE has no intrinsic clock. Event
// producers must populate KeyEvent::now_ms at the source, and tick() must
// be driven from the same time base.
//
// Key routing contract:
//   MOKYA_KEY_BACK — never consumed by MIE; the router must filter BACK
//                    before dispatch. BACK is reserved for UI /
//                    application layer (return, exit service).
//   MOKYA_KEY_DEL  — MIE-exclusive. Priority: if pending composition is
//                    non-empty, delete the last-appended key; otherwise
//                    fire on_delete_before() for the UI to delete the
//                    character before the textarea cursor.
//   DPAD           — if candidates are showing, navigate the candidate
//                    list (Left/Right = select, Up/Down = page).
//                    Otherwise fire on_cursor_move(dir) so the UI can
//                    move the textarea cursor.
//   MOKYA_KEY_MODE — commit any pending state, then cycle input mode.

#pragma once

#include <stdint.h>
#include <mie/hal_port.h>
#include <mie/keycode.h>
#include <mie/trie_searcher.h>

namespace mie {

// ── Input modes ──────────────────────────────────────────────────────────────
enum class InputMode : uint8_t {
    SmartZh = 0,   // 注音
    SmartEn = 1,   // 智慧英數
    Direct  = 2,   // 直選英數
};

/// Direction for candidate navigation or textarea cursor movement.
enum class NavDir : uint8_t { Left, Right, Up, Down };

/// Style hint for rendering the pending composition string.
enum class PendingStyle : uint8_t {
    None,         ///< Pending buffer is empty (PendingView::str == "").
    PrefixBold,   ///< SmartZh / SmartEn letter: [matched prefix bold][rest normal].
    Inverted,     ///< Direct / SmartEn digit / SYM cycling: draw entire string inverted.
};

/// Pending composition snapshot. The UI renders this directly without
/// querying additional state. The str pointer is owned by ImeLogic and
/// remains valid until the next mutating call (process_key / tick /
/// abort) on the same thread.
struct PendingView {
    const char*  str;                    ///< UTF-8, null-terminated.
    int          byte_len;               ///< bytes in str (excludes the \0).
    int          matched_prefix_bytes;   ///< 0..byte_len; 0 when style == Inverted.
    PendingStyle style;
};

// ── Listener interface (UI implements) ───────────────────────────────────────
//
// Event flow:
//   UI → process_key() / tick() → ImeLogic updates state → listener fires.
//   After on_composition_changed(), UI re-queries pending_view() and the
//   candidate accessors.
//
// All callbacks are synchronous. The listener implementation MUST NOT
// call back into ImeLogic from within a callback; re-entrancy is
// unsupported. The single-consumer architectural rule (one FreeRTOS task
// owns the ImeLogic instance and drains the KeyEvent queue) guarantees
// this at the system level.
class IImeListener {
public:
    virtual ~IImeListener() = default;

    /// Insert UTF-8 text at the current textarea cursor; UI advances its
    /// cursor. Fired by OK / SPACE commits, multi-tap timeout, long-press
    /// symbol picker, and partial-commit on candidate selection.
    virtual void on_commit(const char* utf8) = 0;

    /// DPAD pressed while no candidates are showing — UI moves textarea cursor.
    virtual void on_cursor_move(NavDir dir) {}

    /// DEL pressed while pending composition is empty — UI deletes the
    /// character before the cursor.
    virtual void on_delete_before() {}

    /// Pending composition or candidate list changed — UI repaints both panels.
    virtual void on_composition_changed() {}
};

// ── ImeLogic ─────────────────────────────────────────────────────────────────
class ImeLogic {
public:
    // Timing constants.
    static constexpr uint32_t kMultiTapTimeoutMs = 800;   ///< Direct / digit / SYM2 auto-commit.
    static constexpr uint32_t kLongPressMs       = 500;   ///< SYM1 long-press threshold.

    // Candidate display.
    static constexpr int      kPageSize          = 5;
    static constexpr int      kMaxCandidates     = 50;

    // Internal buffer limits.
    static constexpr int      kMaxKeySeq         = 64;
    static constexpr int      kMaxDisplayBytes   = 256;

    /// @param zh_searcher  Required Chinese dictionary (used in SmartZh).
    /// @param en_searcher  Optional English dictionary (SmartEn letter
    ///                     mode). nullptr disables English prediction but
    ///                     leaves SmartEn digit multi-tap working.
    explicit ImeLogic(TrieSearcher& zh_searcher, TrieSearcher* en_searcher = nullptr);

    /// Attach the event listener. Single slot; nullptr detaches.
    void set_listener(IImeListener* listener);

    /// Feed one key event (down or up).
    /// ev.now_ms must be non-decreasing across calls on the same instance.
    /// MOKYA_KEY_BACK is silently ignored (router should pre-filter).
    /// @return true if the event mutated MIE state (listener may have fired).
    bool process_key(const KeyEvent& ev);

    /// Advance internal timers. Call periodically (~20 ms recommended)
    /// from the UI task. Drives multi-tap auto-commit and SYM1 long-press
    /// firing. now_ms must share the time base used by KeyEvent::now_ms.
    /// @return true if state changed during this tick.
    bool tick(uint32_t now_ms);

    /// Abort any pending composition or cycling state without committing.
    /// Call when the textarea cursor moves externally (touch, focus
    /// change) or when the IME session is being torn down. Fires
    /// on_composition_changed() when state was non-empty.
    void abort();

    // ── Query ────────────────────────────────────────────────────────────
    InputMode   mode()           const { return mode_; }
    const char* mode_indicator() const;    ///< "中" / "EN" / "ABC"

    bool        has_pending()    const { return display_len_ > 0; }
    bool        has_candidates() const { return cand_count_ > 0; }

    /// Single-snapshot pending composition with style hint.
    PendingView pending_view()   const;

    // ── Candidates ───────────────────────────────────────────────────────
    int              candidate_count() const { return cand_count_; }
    const Candidate& candidate(int i)  const { return candidates_[i]; }
    int              selected()        const { return selected_; }

    // Pagination (page size is kPageSize).
    int page()            const { return cand_count_ ? selected_ / kPageSize : 0; }
    int page_count()      const { return (cand_count_ + kPageSize - 1) / kPageSize; }
    int page_cand_count() const;
    const Candidate& page_cand(int i) const;
    int page_sel()        const { return cand_count_ ? selected_ % kPageSize : 0; }

private:
    // ── Mode handlers (ime_smart.cpp / ime_direct.cpp) ───────────────────
    bool handle_smart(const KeyEvent& ev);
    bool handle_direct(const KeyEvent& ev);
    bool handle_sym1(bool pressed, uint32_t now_ms);
    bool handle_sym2(uint32_t now_ms);
    bool handle_dpad(mokya_keycode_t kc);
    bool handle_del();
    void cycle_mode();

    // ── Multi-tap state machine (Direct / digit / SYM2) ──────────────────
    void multitap_press(mokya_keycode_t kc, int slot_count, bool inverted, uint32_t now_ms);
    void multitap_commit();

    // ── Commit helpers (ime_commit.cpp) ──────────────────────────────────
    void emit_commit(const char* utf8);
    void commit_selected_candidate();
    void commit_partial(const char* utf8, int prefix_keys);
    void did_commit(const char* utf8);

    // ── Search helpers (ime_search.cpp) ──────────────────────────────────
    void run_search();
    void rebuild_display_smart();

    // ── Display helpers (ime_display.cpp) ────────────────────────────────
    void display_clear();
    void display_set(const char* utf8);

    // ── Notification ─────────────────────────────────────────────────────
    void notify_changed();

    // ── State data ───────────────────────────────────────────────────────
    TrieSearcher&  zh_searcher_;
    TrieSearcher*  en_searcher_;
    IImeListener*  listener_ = nullptr;

    InputMode      mode_ = InputMode::SmartZh;

    // Smart mode key sequence — byte-encoded (slot + 0x21, 0x20
    // first-tone marker, 0x22 for the ˇ/ˋ tone key).
    char           key_seq_[kMaxKeySeq + 1] = {0};
    int            key_seq_len_ = 0;

    // Display buffer backing pending_view().str. Contents depend on mode:
    //   SmartZh — compound "ㄅㄉ, ㄆㄊ" (grouped phonemes per key).
    //   SmartEn — accumulated letters "we".
    //   Direct / SmartEn digit — current cycling character "a" / "1".
    //   SYM2 cycling — current symbol "。" / ".".
    char           display_[kMaxDisplayBytes + 1] = {0};
    int            display_len_ = 0;

    PendingStyle   pending_style_        = PendingStyle::None;
    int            matched_prefix_bytes_ = 0;   // in display_ bytes
    int            matched_prefix_keys_  = 0;   // in key_seq_ bytes

    // Candidate pool (single-language per run; lang tag per Candidate).
    // candidates_prefix_keys_[i] = number of key_seq_ bytes matched by
    // candidate i. Longer-match candidates appear first; each candidate
    // remembers its own prefix length so commit_selected_candidate knows
    // how many bytes to strip (different candidates can match different
    // lengths of the same key sequence).
    Candidate      candidates_[kMaxCandidates];
    uint8_t        candidates_prefix_keys_[kMaxCandidates] = {0};
    int            cand_count_ = 0;
    int            selected_   = 0;

    // Multi-tap state (Direct / SmartEn digit / SYM2).
    struct MultiTapState {
        mokya_keycode_t keycode    = MOKYA_KEY_NONE;
        uint8_t         slot_idx   = 0;
        uint8_t         slot_count = 0;
        bool            inverted   = false;
        uint32_t        last_ms    = 0;
    } multitap_;

    // SYM1 press tracking (short-press vs long-press differentiation).
    struct Sym1State {
        bool     down          = false;
        bool     long_fired    = false;
        uint32_t pressed_at_ms = 0;
    } sym1_;

    // SYM picker (opened by SYM1 long-press; DPAD navigates, OK commits).
    bool sym_picker_open_ = false;
    int  sym_picker_sel_  = 0;

    // SmartEn auto-capitalize after sentence-ending punctuation.
    // Default-true so the very first SmartEn word after construction
    // (or abort) is capitalised — treats "fresh ImeLogic" as a sentence
    // start; did_commit() clears the flag once a letter word lands.
    bool en_capitalize_next_ = true;

    // SmartEn space-aware spacing: true when the last commit output
    // ended with a space-like character (ASCII space, U+3000). Used by
    // commit_selected_candidate to auto-prepend a leading space before
    // a word commit unless the text already ends with one (English
    // sentence convention: "Hello World, Apple." — space before each
    // new word, no space before punctuation, trailing space after
    // , . ? !). Default-true so the very first word after construction
    // does NOT prepend (sentence-start).
    bool en_last_ended_with_space_ = true;
};

} // namespace mie
