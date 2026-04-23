// SPDX-License-Identifier: MIT
// ime_logic.cpp — Constructor, top-level dispatcher, tick, abort, mode cycle,
//                 DPAD handler, DEL handler, page accessors.
//
// Mode-specific key processing lives in ime_smart.cpp / ime_direct.cpp.
// Search, display, commit, and key-table helpers live in their own files.

#include "ime_internal.h"
#include <cstring>

namespace mie {

// ── SYM1 long-press symbol picker (Phase 1.4 Task B) ────────────────────
// 4-column × 4-row grid of common Traditional-Chinese punctuation that the
// user can't otherwise type from the half-keyboard. Layout (row-major):
//
//   「  」  『  』
//   （  ）  【  】
//   ，  。  、  ；
//   ：  ？  ！  …
//
// Selection wraps modulo grid size on Left/Right; Up/Down step ±cols and
// also wrap mod size. The grid lives in .rodata — no runtime allocation.
static constexpr int kSymPickerCols  = 4;
static constexpr int kSymPickerRows  = 4;
static constexpr int kSymPickerCells = kSymPickerCols * kSymPickerRows;

static const char* const kSymPickerCells_[kSymPickerCells] = {
    "\xe3\x80\x8c", "\xe3\x80\x8d", "\xe3\x80\x8e", "\xe3\x80\x8f",  // 「」『』
    "\xef\xbc\x88", "\xef\xbc\x89", "\xe3\x80\x90", "\xe3\x80\x91",  // （）【】
    "\xef\xbc\x8c", "\xe3\x80\x82", "\xe3\x80\x81", "\xef\xbc\x9b",  // ，。、；
    "\xef\xbc\x9a", "\xef\xbc\x9f", "\xef\xbc\x81", "\xe2\x80\xa6",  // ：？！…
};

int  ImeLogic::picker_cell_count() const { return kSymPickerCells; }
int  ImeLogic::picker_cols()        const { return kSymPickerCols;  }
const char* ImeLogic::picker_cell(int idx) const {
    if (idx < 0 || idx >= kSymPickerCells) return "";
    return kSymPickerCells_[idx];
}

// ── Construction / configuration ─────────────────────────────────────────────

ImeLogic::ImeLogic(TrieSearcher& zh_searcher, TrieSearcher* en_searcher)
    : zh_searcher_(zh_searcher)
    , en_searcher_(en_searcher)
{
    // All POD members initialized in-class.
}

void ImeLogic::set_selected(int idx) {
    if (cand_count_ == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= cand_count_) idx = cand_count_ - 1;
    if (selected_ == idx) return;
    selected_ = idx;
    notify_changed();
}

void ImeLogic::set_listener(IImeListener* listener) {
    listener_ = listener;
}

void ImeLogic::attach_composition_searcher(CompositionSearcher* cs) {
    composition_searcher_ = cs;
}

// ── Top-level dispatcher ─────────────────────────────────────────────────────

bool ImeLogic::process_key(const KeyEvent& ev) {
    // BACK is reserved for UI; silently ignore if the router missed it.
    if (ev.keycode == MOKYA_KEY_BACK)               return false;
    if (ev.keycode == MOKYA_KEY_NONE)               return false;
    if (ev.keycode >= MOKYA_KEY_LIMIT)              return false;

    // SYM1 uses both edges (short-press vs long-press).
    if (ev.keycode == MOKYA_KEY_SYM1)
        return handle_sym1(ev.pressed, ev.now_ms);

    // All other keys trigger on press only.
    if (!ev.pressed) return false;

    const mokya_keycode_t kc = ev.keycode;

    // ── Picker-active routing ───────────────────────────────────────
    // While the SYM1 long-press picker is open, DPAD navigates the
    // grid and OK commits the selected symbol. Any other key closes
    // the picker without commit (the user changed their mind).
    if (sym_picker_open_) {
        if (kc == MOKYA_KEY_LEFT || kc == MOKYA_KEY_RIGHT ||
            kc == MOKYA_KEY_UP   || kc == MOKYA_KEY_DOWN) {
            int sel = sym_picker_sel_;
            if      (kc == MOKYA_KEY_LEFT)  sel = (sel - 1 + kSymPickerCells) % kSymPickerCells;
            else if (kc == MOKYA_KEY_RIGHT) sel = (sel + 1) % kSymPickerCells;
            else if (kc == MOKYA_KEY_UP)    sel = (sel - kSymPickerCols + kSymPickerCells) % kSymPickerCells;
            else                            sel = (sel + kSymPickerCols) % kSymPickerCells;
            sym_picker_sel_ = sel;
            notify_changed();
            return true;
        }
        if (kc == MOKYA_KEY_OK) {
            const char* s = kSymPickerCells_[sym_picker_sel_];
            sym_picker_open_ = false;
            sym_picker_sel_  = 0;
            emit_commit(s);
            notify_changed();
            return true;
        }
        // Any other key (DEL, MODE, slot keys, SYM2, etc): close
        // without commit and fall through so the key is ignored this
        // press. Next press hits normal routing.
        sym_picker_open_ = false;
        sym_picker_sel_  = 0;
        notify_changed();
        return true;
    }

    // MODE — commit any pending, then cycle mode.
    if (kc == MOKYA_KEY_MODE) { cycle_mode(); return true; }

    // DEL — MIE-exclusive. Priority: pending → listener on_delete_before.
    if (kc == MOKYA_KEY_DEL)  return handle_del();

    // DPAD — candidates navigate list; otherwise fire on_cursor_move.
    if (kc == MOKYA_KEY_UP   || kc == MOKYA_KEY_DOWN ||
        kc == MOKYA_KEY_LEFT || kc == MOKYA_KEY_RIGHT)
        return handle_dpad(kc);

    // OK — commit selected candidate, or commit multi-tap pending, or let
    // the UI handle it as Enter/confirm. OK does NOT auto-append a
    // trailing space in SmartEn: the English-sentence convention is
    // that the next word carries its own leading space (handled in
    // commit_selected_candidate), so punctuation can follow a word
    // immediately without creating "Apple ," artifacts.
    if (kc == MOKYA_KEY_OK) {
        if (has_candidates()) { commit_selected_candidate(); notify_changed(); return true; }
        if (multitap_.keycode != MOKYA_KEY_NONE) { multitap_commit(); notify_changed(); return true; }
        return false;
    }

    // SYM2 — multi-tap punctuation cycling.
    if (kc == MOKYA_KEY_SYM2) return handle_sym2(ev.now_ms);

    // A non-multitap key arriving while multi-tap is pending: commit the
    // multi-tap first, then fall through to mode-specific handling.
    // Exception: the incoming key is the same multitap target (handled
    // internally by multitap_press).
    if (multitap_.keycode != MOKYA_KEY_NONE && multitap_.keycode != kc) {
        // Preserve same-key cycling of SmartEn digits: only flush when
        // switching to a different key.
        multitap_commit();
    }

    // Mode-specific.
    switch (mode_) {
        case InputMode::SmartZh:
        case InputMode::SmartEn: return handle_smart(ev);
        case InputMode::Direct:  return handle_direct(ev);
    }
    return false;
}

// ── Periodic tick (multi-tap timeout + SYM1 long-press detection) ───────────

bool ImeLogic::tick(uint32_t now_ms) {
    bool changed = false;

    if (multitap_.keycode != MOKYA_KEY_NONE &&
        (uint32_t)(now_ms - multitap_.last_ms) >= kMultiTapTimeoutMs) {
        multitap_commit();
        changed = true;
    }

    if (sym1_.down && !sym1_.long_fired &&
        (uint32_t)(now_ms - sym1_.pressed_at_ms) >= kLongPressMs) {
        sym1_.long_fired = true;
        sym_picker_open_ = true;
        sym_picker_sel_  = 0;
        // Picker display is driven in handle_sym1 / dpad / OK paths;
        // Phase A ships with an open flag only. A follow-up milestone
        // wires the picker list into pending_view().
        changed = true;
    }

    // Lock the long-press cycle once the multitap timeout passes — no
    // visible change (the cycled phoneme stays in phoneme_hint_), just
    // means the next long-press of the same slot will start a new byte
    // instead of cycling the previous one.
    if (lp_cycle_.byte_index >= 0 &&
        (uint32_t)(now_ms - lp_cycle_.last_ms) >= kMultiTapTimeoutMs) {
        lp_cycle_.byte_index = -1;
    }

    if (changed) notify_changed();
    return changed;
}

// ── Abort pending composition (external cursor move, session teardown) ──────

void ImeLogic::set_text_context(const char* prev_utf8) {
    if (!prev_utf8 || !prev_utf8[0]) {
        en_capitalize_next_       = true;
        en_last_ended_with_space_ = true;
        return;
    }
    const size_t len = std::strlen(prev_utf8);

    // Leading-space suppressor: set if the caller's last char is a space.
    en_last_ended_with_space_ =
        (prev_utf8[len - 1] == ' ') ||
        (len >= 3 && std::memcmp(prev_utf8 + len - 3, "\xe3\x80\x80", 3) == 0);

    // Capitalise next: scan past trailing whitespace and check for
    // sentence-ending punctuation (ASCII . ? ! or full-width 。？！).
    int end_pos = (int)len - 1;
    while (end_pos > 0 && prev_utf8[end_pos] == ' ') --end_pos;
    bool ends_sentence = false;
    if (end_pos >= 0) {
        const char c = prev_utf8[end_pos];
        if (c == '.' || c == '?' || c == '!') ends_sentence = true;
        if (!ends_sentence && end_pos >= 2 &&
            (std::memcmp(prev_utf8 + end_pos - 2, "\xe3\x80\x82", 3) == 0 ||
             std::memcmp(prev_utf8 + end_pos - 2, "\xef\xbc\x9f", 3) == 0 ||
             std::memcmp(prev_utf8 + end_pos - 2, "\xef\xbc\x81", 3) == 0)) {
            ends_sentence = true;
        }
    }
    en_capitalize_next_ = ends_sentence;
}

void ImeLogic::abort() {
    bool had_state = has_pending() || has_candidates() ||
                     multitap_.keycode != MOKYA_KEY_NONE || sym_picker_open_;

    key_seq_len_          = 0;
    key_seq_[0]           = '\0';
    std::memset(phoneme_hint_, 0, sizeof(phoneme_hint_));
    display_clear();
    pending_style_        = PendingStyle::None;
    matched_prefix_bytes_ = 0;
    matched_prefix_keys_  = 0;
    cand_count_           = 0;
    selected_             = 0;
    multitap_             = {};
    sym1_                 = {};
    sym_picker_open_      = false;
    sym_picker_sel_       = 0;
    lp_cycle_             = {};
    en_capitalize_next_        = true;   // treat abort as new sentence start
    en_last_ended_with_space_  = true;

    if (had_state) notify_changed();
}

// ── MODE key: commit pending then cycle SmartZh → SmartEn → Direct → ... ───

void ImeLogic::cycle_mode() {
    // Commit any pending state first.
    if (multitap_.keycode != MOKYA_KEY_NONE) {
        multitap_commit();
    } else if (has_candidates()) {
        commit_selected_candidate();
    }
    // Clear remaining composition (no candidates → just discard).
    key_seq_len_          = 0;
    key_seq_[0]           = '\0';
    std::memset(phoneme_hint_, 0, sizeof(phoneme_hint_));
    display_clear();
    pending_style_        = PendingStyle::None;
    matched_prefix_bytes_ = 0;
    matched_prefix_keys_  = 0;
    cand_count_           = 0;
    selected_             = 0;
    lp_cycle_             = {};

    switch (mode_) {
        case InputMode::SmartZh: mode_ = InputMode::SmartEn; break;
        case InputMode::SmartEn: mode_ = InputMode::Direct;  break;
        case InputMode::Direct:  mode_ = InputMode::SmartZh; break;
    }
    notify_changed();
}

// ── DPAD ────────────────────────────────────────────────────────────────────

bool ImeLogic::handle_dpad(mokya_keycode_t kc) {
    if (has_candidates()) {
        switch (kc) {
            case MOKYA_KEY_LEFT:
                selected_ = (selected_ - 1 + cand_count_) % cand_count_;
                break;
            case MOKYA_KEY_RIGHT:
                selected_ = (selected_ + 1) % cand_count_;
                break;
            case MOKYA_KEY_UP:
            case MOKYA_KEY_DOWN:
                // Up/Down navigation is owned by the view layer, which knows
                // the visual flex-wrap row layout (see ime_view_apply +
                // find_row_neighbour). The engine's kPageSize page-jump
                // raced the view override and caused the highlight to flash
                // to the wrong cell for one frame before snapping back.
                // Consume the event without changing selected_; the view
                // override runs in the LVGL task and sets selected_ itself.
                return true;
            default: return false;
        }
        notify_changed();
        return true;
    }

    // No candidates — tell UI to move its textarea cursor.
    if (listener_) {
        NavDir dir;
        switch (kc) {
            case MOKYA_KEY_LEFT:  dir = NavDir::Left;  break;
            case MOKYA_KEY_RIGHT: dir = NavDir::Right; break;
            case MOKYA_KEY_UP:    dir = NavDir::Up;    break;
            case MOKYA_KEY_DOWN:  dir = NavDir::Down;  break;
            default: return false;
        }
        listener_->on_cursor_move(dir);
        return true;
    }
    return false;
}

// ── DEL ─────────────────────────────────────────────────────────────────────

bool ImeLogic::handle_del() {
    // Direct / SYM multi-tap pending? Cancel it.
    if (multitap_.keycode != MOKYA_KEY_NONE) {
        multitap_             = {};
        display_clear();
        pending_style_        = PendingStyle::None;
        matched_prefix_bytes_ = 0;
        notify_changed();
        return true;
    }
    // Smart mode: remove last byte from key_seq and re-run search.
    if (key_seq_len_ > 0) {
        --key_seq_len_;
        key_seq_[key_seq_len_] = '\0';
        // If the cycled byte was just deleted, clear the cycle so the
        // next long-press starts fresh.
        if (lp_cycle_.byte_index >= key_seq_len_) {
            lp_cycle_.byte_index = -1;
        }
        run_search();
        notify_changed();
        return true;
    }
    // Pending empty — forward to UI as delete-before-cursor.
    if (listener_) listener_->on_delete_before();
    return true;
}

// ── Notification helper ─────────────────────────────────────────────────────

void ImeLogic::notify_changed() {
    if (listener_) listener_->on_composition_changed();
}

// ── Page accessors ──────────────────────────────────────────────────────────

int ImeLogic::page_cand_count() const {
    if (cand_count_ == 0) return 0;
    int start = page() * kPageSize;
    int rem   = cand_count_ - start;
    return (rem < kPageSize) ? rem : kPageSize;
}

const Candidate& ImeLogic::page_cand(int i) const {
    return candidates_[page() * kPageSize + i];
}

} // namespace mie
