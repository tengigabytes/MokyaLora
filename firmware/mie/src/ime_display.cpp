// ime_display.cpp — Display buffer helpers and compound input string builders.
// SPDX-License-Identifier: MIT

#include "ime_internal.h"
#include <cstring>

namespace mie {

// ── Display buffer helpers ────────────────────────────────────────────────

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
    input_len_      = pos;
    input_buf_[pos] = '\0';
}

void ImeLogic::set_display(const char* utf8) {
    if (!utf8) { input_len_ = 0; input_buf_[0] = '\0'; return; }
    int len = (int)strlen(utf8);
    if (len > kMaxDisplayBytes) len = kMaxDisplayBytes;
    memcpy(input_buf_, utf8, (size_t)len);
    input_len_ = len;
    input_buf_[input_len_] = '\0';
}

// ── Mode indicator ────────────────────────────────────────────────────────

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

// ── Compound input string ─────────────────────────────────────────────────
// SmartZh: each key → "[ph0ph1]" (or "ph0" if only one phoneme).
//          First-tone marker (0x20) → "ˉ" (U+02C9).
// SmartEn / other: falls back to input_str().

const char* ImeLogic::compound_input_str() const {
    static char buf[640];
    if (mode_ != InputMode::SmartZh) return input_str();
    int pos = 0;
    for (int i = 0; i < key_seq_len_ && pos < 630; ++i) {
        uint8_t b = (uint8_t)key_seq_buf_[i];
        if (b == 0x20) {
            if (pos + 2 < 639) { memcpy(buf + pos, "\xcb\x89", 2); pos += 2; }
            continue;
        }
        int slot = (int)b - 0x21;
        if (slot < 0 || slot >= 20) continue;
        const DirectEntry* e = find_direct_entry(input_slot_to_keycode(slot));
        if (!e) continue;
        const char* phs[3] = { e->labels[0], e->labels[1], e->labels[2] };
        int np = 0;
        while (np < 3 && phs[np]) ++np;
        if (np == 0) continue;
        if (np == 1) {
            int n = (int)strlen(phs[0]);
            if (pos + n < 639) { memcpy(buf + pos, phs[0], (size_t)n); pos += n; }
        } else {
            buf[pos++] = '[';
            for (int p = 0; p < np && pos < 638; ++p) {
                int n = (int)strlen(phs[p]);
                if (pos + n < 638) { memcpy(buf + pos, phs[p], (size_t)n); pos += n; }
            }
            if (pos < 639) buf[pos++] = ']';
        }
    }
    buf[pos] = '\0';
    return buf;
}

int ImeLogic::compound_input_bytes() const {
    return (int)strlen(compound_input_str());
}

// Bytes of compound_input_str() corresponding to the matched-prefix keys.
int ImeLogic::matched_prefix_compound_bytes() const {
    if (mode_ != InputMode::SmartZh) return matched_prefix_display_bytes();
    int pos = 0;
    for (int i = 0; i < matched_prefix_len_ && i < key_seq_len_ && pos < 630; ++i) {
        uint8_t b = (uint8_t)key_seq_buf_[i];
        if (b == 0x20) { pos += 2; continue; }  // "ˉ" = 2 UTF-8 bytes
        int slot = (int)b - 0x21;
        if (slot < 0 || slot >= 20) continue;
        const DirectEntry* e = find_direct_entry(input_slot_to_keycode(slot));
        if (!e) continue;
        const char* phs[3] = { e->labels[0], e->labels[1], e->labels[2] };
        int np = 0; while (np < 3 && phs[np]) ++np;
        if (np == 0) continue;
        if (np == 1) { pos += (int)strlen(phs[0]); }
        else {
            pos += 1;  // '['
            for (int p = 0; p < np; ++p) pos += (int)strlen(phs[p]);
            pos += 1;  // ']'
        }
    }
    return pos;
}

} // namespace mie
