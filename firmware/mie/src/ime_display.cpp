// SPDX-License-Identifier: MIT
// ime_display.cpp — Display buffer helpers, mode_indicator, pending_view.

#include "ime_internal.h"
#include <cstring>

namespace mie {

void ImeLogic::display_clear() {
    display_len_ = 0;
    display_[0]  = '\0';
}

void ImeLogic::display_set(const char* utf8) {
    if (!utf8) { display_clear(); return; }
    int len = (int)strlen(utf8);
    if (len > kMaxDisplayBytes) len = kMaxDisplayBytes;
    memcpy(display_, utf8, (size_t)len);
    display_len_ = len;
    display_[display_len_] = '\0';
}

const char* ImeLogic::mode_indicator() const {
    switch (mode_) {
        case InputMode::SmartZh: return "\xe4\xb8\xad";  // 中
        case InputMode::SmartEn: return "EN";
        case InputMode::Direct:  return "ABC";
    }
    return "?";
}

PendingView ImeLogic::pending_view() const {
    PendingView pv;
    pv.str                  = display_;
    pv.byte_len             = display_len_;
    pv.matched_prefix_bytes = matched_prefix_bytes_;
    pv.style                = pending_style_;
    return pv;
}

} // namespace mie
