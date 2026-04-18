// SPDX-License-Identifier: MIT
// ime_logic.cpp — Constructor, top-level dispatcher, tick, abort, mode cycle,
//                 DPAD handler, DEL handler, page accessors.
//
// Mode-specific key processing lives in ime_smart.cpp / ime_direct.cpp.
// Search, display, commit, and key-table helpers live in their own files.

#include "ime_internal.h"
#include <cstring>

namespace mie {

// ── Construction / configuration ─────────────────────────────────────────────

ImeLogic::ImeLogic(TrieSearcher& zh_searcher, TrieSearcher* en_searcher)
    : zh_searcher_(zh_searcher)
    , en_searcher_(en_searcher)
{
    // All POD members initialized in-class.
}

void ImeLogic::set_listener(IImeListener* listener) {
    listener_ = listener;
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

    // MODE — commit any pending, then cycle mode.
    if (kc == MOKYA_KEY_MODE) { cycle_mode(); return true; }

    // DEL — MIE-exclusive. Priority: pending → listener on_delete_before.
    if (kc == MOKYA_KEY_DEL)  return handle_del();

    // DPAD — candidates navigate list; otherwise fire on_cursor_move.
    if (kc == MOKYA_KEY_UP   || kc == MOKYA_KEY_DOWN ||
        kc == MOKYA_KEY_LEFT || kc == MOKYA_KEY_RIGHT)
        return handle_dpad(kc);

    // OK — commit selected candidate, or commit multi-tap pending, or let
    // the UI handle it as Enter/confirm.
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

    if (changed) notify_changed();
    return changed;
}

// ── Abort pending composition (external cursor move, session teardown) ──────

void ImeLogic::abort() {
    bool had_state = has_pending() || has_candidates() ||
                     multitap_.keycode != MOKYA_KEY_NONE || sym_picker_open_;

    key_seq_len_          = 0;
    key_seq_[0]           = '\0';
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
    en_capitalize_next_   = false;

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
    display_clear();
    pending_style_        = PendingStyle::None;
    matched_prefix_bytes_ = 0;
    matched_prefix_keys_  = 0;
    cand_count_           = 0;
    selected_             = 0;

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
            case MOKYA_KEY_UP: {
                // Previous page, keeping slot within page.
                int slot = selected_ % kPageSize;
                int p    = page();
                int pc   = page_count();
                int np   = (p - 1 + pc) % pc;
                int pos  = np * kPageSize + slot;
                if (pos >= cand_count_) pos = cand_count_ - 1;
                selected_ = pos;
                break;
            }
            case MOKYA_KEY_DOWN: {
                // Next page, keeping slot within page.
                int slot = selected_ % kPageSize;
                int p    = page();
                int pc   = page_count();
                int np   = (p + 1) % pc;
                int pos  = np * kPageSize + slot;
                if (pos >= cand_count_) pos = cand_count_ - 1;
                selected_ = pos;
                break;
            }
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
