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
                phoneme_hint_[key_seq_len_]   = 0xFFu;  // tone-1 marker: any phoneme
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
        // The long-press / long-long flag on the press event picks which
        // phoneme position the user intends; for SmartEn (letter mode) we
        // treat every press as "primary" — English letters only have two
        // phonemes on a key and the existing SmartEn dispatch routes
        // letter-picking through its own path.
        // Phase 1.4 UX semantic (final, multitap-style precision):
        //   short tap → append byte with hint = ANY (fuzzy half-keyboard).
        //   long  tap → enter / continue a cycling precise mode for the
        //               most recent byte:
        //                 1st long tap  → primary phoneme
        //                 2nd long tap  → secondary
        //                 3rd long tap  → tertiary (slot 4 only; else
        //                                 wraps back to primary mod N)
        //
        // Any non-cycling event (different key, short-tap, mode/del/ok)
        // locks the cycle in place — the last cycled phoneme stays in
        // phoneme_hint_ and further long-presses of the same slot start
        // a NEW cycle on a fresh byte.
        const bool is_long = (mode_ == InputMode::SmartZh) &&
                             (ev.flags & KEY_FLAG_LONG_PRESS);

        if (is_long) {
            int p_count = 0;
            for (int k = 0; k < 3 && e.phonemes[k]; ++k) ++p_count;
            if (p_count == 0) p_count = 1;

            const bool can_cycle =
                (lp_cycle_.byte_index >= 0) &&
                (lp_cycle_.slot == slot) &&
                ((uint32_t)(ev.now_ms - lp_cycle_.last_ms) < kMultiTapTimeoutMs);

            if (can_cycle) {
                lp_cycle_.phoneme_idx = (lp_cycle_.phoneme_idx + 1) % p_count;
                lp_cycle_.last_ms     = ev.now_ms;
                phoneme_hint_[lp_cycle_.byte_index] =
                    (uint8_t)lp_cycle_.phoneme_idx;
                run_search();
                notify_changed();
                return true;
            }
            // Fresh long-press → append a new byte at primary (idx 0).
            if (key_seq_len_ < kMaxKeySeq) {
                phoneme_hint_[key_seq_len_]   = 0;   // primary, strict
                key_seq_[key_seq_len_++] = (char)(slot + 0x21);
                key_seq_[key_seq_len_]   = '\0';
                lp_cycle_.byte_index  = key_seq_len_ - 1;
                lp_cycle_.slot        = slot;
                lp_cycle_.phoneme_idx = 0;
                lp_cycle_.last_ms     = ev.now_ms;
                run_search();
                notify_changed();
            }
            return true;
        }

        // Short-tap (or KEY_FLAG_HINT_ANY) → append fuzzy byte and
        // close any active cycle.
        if (key_seq_len_ < kMaxKeySeq) {
            uint8_t hint = (mode_ == InputMode::SmartZh) ? 0xFFu : 0xFFu;
            phoneme_hint_[key_seq_len_]   = hint;
            key_seq_[key_seq_len_++] = (char)(slot + 0x21);
            key_seq_[key_seq_len_]   = '\0';
            lp_cycle_.byte_index = -1;   // lock prior cycle (if any)
            run_search();
            notify_changed();
        }
        return true;
    }

    return false;
}

} // namespace mie
