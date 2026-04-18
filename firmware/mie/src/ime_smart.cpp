// SPDX-License-Identifier: MIT
// ime_smart.cpp — SmartZh and SmartEn key handler.
//
// Reached only after ime_logic.cpp's top-level dispatcher has filtered out
// MODE / DEL / DPAD / OK / SYM1 / SYM2. The event here is either SPACE /
// TAB / a dictionary-input key.

#include "ime_internal.h"
#include <cstring>

namespace mie {

bool ImeLogic::handle_smart(const KeyEvent& ev) {
    const mokya_keycode_t kc = ev.keycode;

    // SPACE behavior differs between modes and between "idle" and "pending".
    if (kc == MOKYA_KEY_SPACE) {
        if (key_seq_len_ == 0 && multitap_.keycode == MOKYA_KEY_NONE) {
            // Idle — insert a half-width space.
            emit_commit(" ");
            notify_changed();
            return true;
        }
        if (mode_ == InputMode::SmartZh) {
            // Append first-tone marker (0x20). Duplicate press = no-op.
            if (key_seq_len_ > 0 &&
                (uint8_t)key_seq_[key_seq_len_ - 1] != 0x20 &&
                key_seq_len_ < kMaxKeySeq) {
                key_seq_[key_seq_len_++] = 0x20;
                key_seq_[key_seq_len_]   = '\0';
                run_search();
                notify_changed();
            }
            return true;
        }
        // SmartEn pending: commit best candidate (if any) + auto-space.
        if (has_candidates()) {
            commit_selected_candidate();
        } else if (key_seq_len_ > 0) {
            // Nothing matched — discard partial key_seq (typo).
            key_seq_len_ = 0;
            key_seq_[0]  = '\0';
            display_clear();
            pending_style_        = PendingStyle::None;
            matched_prefix_bytes_ = 0;
            matched_prefix_keys_  = 0;
        }
        emit_commit(" ");
        notify_changed();
        return true;
    }

    // TAB — advance to the next candidate page (wraps).
    if (kc == MOKYA_KEY_TAB) {
        if (has_candidates()) {
            int next  = (page() + 1) * kPageSize;
            selected_ = (next >= cand_count_) ? 0 : next;
            notify_changed();
            return true;
        }
        return false;
    }

    // Dictionary-input key.
    int slot = keycode_to_input_slot(kc);
    if (slot >= 0) {
        const KeyEntry& e = kKeyTable[slot];

        // SmartEn row-0 digit keys: digits are not in the English dict,
        // so cycle them like Direct mode.
        if (mode_ == InputMode::SmartEn && e.digit_slots[0]) {
            // Commit any pending letter composition first.
            if (key_seq_len_ > 0) {
                if (has_candidates()) commit_selected_candidate();
                else {
                    key_seq_len_ = 0;
                    key_seq_[0]  = '\0';
                    display_clear();
                    pending_style_        = PendingStyle::None;
                    matched_prefix_bytes_ = 0;
                    matched_prefix_keys_  = 0;
                }
            }
            int slots = count_slots(e.digit_slots);
            multitap_press(kc, slots, /*inverted=*/true, ev.now_ms);
            return true;
        }

        // Smart dictionary search: append slot byte and re-run.
        if (key_seq_len_ < kMaxKeySeq) {
            key_seq_[key_seq_len_++] = (char)(slot + 0x21);
            key_seq_[key_seq_len_]   = '\0';
            run_search();
            notify_changed();
        }
        return true;
    }

    return false;
}

} // namespace mie
