// SPDX-License-Identifier: MIT
// ime_direct.cpp — Direct mode handler, multi-tap state machine,
//                  SYM1 (short/long-press) and SYM2 (multi-tap) handlers.

#include "ime_internal.h"
#include <cstring>

namespace mie {

// ── Punctuation cycle tables ────────────────────────────────────────────────

// SYM1 short-press single character.
static const char kSym1ZhShort[] = "\xef\xbc\x8c";  // ，
static const char kSym1EnShort[] = ",";

// SYM2 multi-tap cycle: 。/？/！ in ZH, ./?/! in EN.
static const char* const kSym2ZhCycle[] = {
    "\xe3\x80\x82",  // 。
    "\xef\xbc\x9f",  // ？
    "\xef\xbc\x81",  // ！
    nullptr,
};
static const char* const kSym2EnCycle[] = { ".", "?", "!", nullptr };

// ── Direct mode handler ─────────────────────────────────────────────────────

bool ImeLogic::handle_direct(const KeyEvent& ev) {
    const mokya_keycode_t kc = ev.keycode;

    // SPACE — commit multi-tap pending (if any), then insert half-width space.
    if (kc == MOKYA_KEY_SPACE) {
        if (multitap_.keycode != MOKYA_KEY_NONE) multitap_commit();
        emit_commit(" ");
        notify_changed();
        return true;
    }

    // TAB — commit pending, then insert a tab.
    if (kc == MOKYA_KEY_TAB) {
        if (multitap_.keycode != MOKYA_KEY_NONE) multitap_commit();
        emit_commit("\t");
        notify_changed();
        return true;
    }

    // Dictionary-input key → multi-tap cycling.
    int slot = keycode_to_input_slot(kc);
    if (slot >= 0) {
        const KeyEntry& e = kKeyTable[slot];
        int slots = e.digit_slots[0] ? count_slots(e.digit_slots)
                                     : count_slots(e.letter_slots);
        if (slots == 0) {
            // BACKSLASH has no letter/digit slots in Direct — NOP.
            return false;
        }
        multitap_press(kc, slots, /*inverted=*/true, ev.now_ms);
        return true;
    }

    return false;
}

// ── Multi-tap state machine ─────────────────────────────────────────────────

namespace {

// Retrieve the idx-th cycling label for the multi-tap target keycode.
// Handles input keys (kKeyTable entries) and SYM2 (kSym2*Cycle). Returns
// nullptr on out-of-range.
static const char* multitap_label(const ImeLogic* self,
                                  mokya_keycode_t kc, int idx, InputMode mode) {
    (void)self;
    if (kc == MOKYA_KEY_SYM2) {
        const char* const* arr = (mode == InputMode::SmartZh) ? kSym2ZhCycle
                                                              : kSym2EnCycle;
        int n = 0; while (arr[n]) ++n;
        return (idx >= 0 && idx < n) ? arr[idx] : nullptr;
    }
    const KeyEntry* e = find_key_entry(kc);
    if (!e) return nullptr;
    if (e->digit_slots[0]) {
        return (idx >= 0 && idx < 2) ? e->digit_slots[idx] : nullptr;
    }
    return (idx >= 0 && idx < 4) ? e->letter_slots[idx] : nullptr;
}

} // namespace

void ImeLogic::multitap_press(mokya_keycode_t kc, int slot_count,
                              bool inverted, uint32_t now_ms) {
    if (multitap_.keycode == kc) {
        // Same key — cycle within the timeout window.
        multitap_.slot_idx = (uint8_t)((multitap_.slot_idx + 1) % slot_count);
    } else {
        // Different key — commit prior pending first.
        if (multitap_.keycode != MOKYA_KEY_NONE) multitap_commit();
        multitap_.keycode    = kc;
        multitap_.slot_idx   = 0;
        multitap_.slot_count = (uint8_t)slot_count;
        multitap_.inverted   = inverted;
    }
    multitap_.last_ms = now_ms;

    // Skip nullptr slots (e.g. L has {"l",nullptr,"L",nullptr}) — bump
    // through them so the user sees a valid label.
    int safety = multitap_.slot_count;
    while (safety-- > 0 &&
           multitap_label(this, kc, multitap_.slot_idx, mode_) == nullptr) {
        multitap_.slot_idx = (uint8_t)((multitap_.slot_idx + 1) % slot_count);
    }

    const char* lbl = multitap_label(this, kc, multitap_.slot_idx, mode_);
    display_set(lbl ? lbl : "");
    pending_style_        = inverted ? PendingStyle::Inverted : PendingStyle::PrefixBold;
    matched_prefix_bytes_ = 0;

    notify_changed();
}

void ImeLogic::multitap_commit() {
    if (multitap_.keycode == MOKYA_KEY_NONE) return;

    // Capture the committed key before resetting state so we can apply
    // post-commit formatting (trailing space for SmartEn punctuation).
    mokya_keycode_t committed_kc = multitap_.keycode;

    // The current display_ contents are the label being committed.
    if (display_len_ > 0) {
        // Copy out before did_commit / on_commit so side effects on state
        // cannot invalidate the string we pass out.
        char label[kMaxDisplayBytes + 1];
        std::memcpy(label, display_, (size_t)display_len_ + 1);
        did_commit(label);
        if (listener_) listener_->on_commit(label);
    }
    multitap_             = {};
    display_clear();
    pending_style_        = PendingStyle::None;
    matched_prefix_bytes_ = 0;

    // English sentence convention: SmartEn sentence-punctuation (SYM2
    // cycles . ? !) auto-appends a trailing space. Direct-mode letter
    // multi-tap and SmartEn digit multi-tap are literal-content input
    // (passwords, numbers) and keep no trailing space.
    if (committed_kc == MOKYA_KEY_SYM2 && mode_ == InputMode::SmartEn) {
        emit_commit(" ");
    }
}

// ── SYM1 — short-press single char / long-press opens picker ────────────────

bool ImeLogic::handle_sym1(bool pressed, uint32_t now_ms) {
    if (pressed) {
        if (!sym1_.down) {
            sym1_.down          = true;
            sym1_.long_fired    = false;
            sym1_.pressed_at_ms = now_ms;
        }
        return false;   // Visible state change waits for tick / release.
    }

    // Released.
    if (!sym1_.down) return false;
    sym1_.down = false;

    if (sym1_.long_fired) {
        // Long-press already opened the picker in tick(); nothing else here.
        sym1_.long_fired = false;
        return true;
    }

    // Short-press: commit any multi-tap pending, then emit ，/, for current
    // mode. SmartEn appends a trailing space so the punctuation follows
    // the English sentence convention ("Apple, World.").
    if (multitap_.keycode != MOKYA_KEY_NONE) multitap_commit();
    const char* s;
    if (mode_ == InputMode::SmartZh)       s = kSym1ZhShort;   // 「，」
    else if (mode_ == InputMode::SmartEn)  s = ", ";           // ASCII + space
    else                                   s = kSym1EnShort;   // Direct: ","
    emit_commit(s);
    notify_changed();
    return true;
}

// ── SYM2 — multi-tap punctuation cycling ────────────────────────────────────

bool ImeLogic::handle_sym2(uint32_t now_ms) {
    const char* const* cycle = (mode_ == InputMode::SmartZh) ? kSym2ZhCycle
                                                             : kSym2EnCycle;
    int slots = 0; while (cycle[slots]) ++slots;
    if (slots == 0) return false;

    // Commit any non-SYM2 multi-tap first.
    if (multitap_.keycode != MOKYA_KEY_NONE && multitap_.keycode != MOKYA_KEY_SYM2)
        multitap_commit();

    if (multitap_.keycode == MOKYA_KEY_SYM2) {
        multitap_.slot_idx = (uint8_t)((multitap_.slot_idx + 1) % slots);
    } else {
        multitap_.keycode    = MOKYA_KEY_SYM2;
        multitap_.slot_idx   = 0;
        multitap_.slot_count = (uint8_t)slots;
        multitap_.inverted   = true;
    }
    multitap_.last_ms = now_ms;

    display_set(cycle[multitap_.slot_idx]);
    pending_style_        = PendingStyle::Inverted;
    matched_prefix_bytes_ = 0;

    notify_changed();
    return true;
}

} // namespace mie
