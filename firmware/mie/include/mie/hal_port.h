// SPDX-License-Identifier: MIT
// hal_port.h — MokyaInput Engine key event structure
//
// Defines the KeyEvent passed to ImeLogic::process_key(). The v2 API is
// push-based: the platform-specific event source (Core 1 FreeRTOS queue
// drainer, PC stdin reader, USB control injection) calls process_key()
// directly. No IHalPort poll interface exists in v2.
//
// `now_ms` must be populated at the event's source with a monotonic
// millisecond timestamp. MIE's multi-tap and long-press state machines
// compare deltas against this field, so every event producer on the same
// platform must share a single time base (FreeRTOS ticks on RP2350,
// clock_gettime on host, etc.).

#pragma once

#include <cstdint>
#include <mie/keycode.h>

namespace mie {

/// Producer-side event flags. Aliases of the C-callable
/// MOKYA_KEY_FLAG_* constants in <mie/keycode.h>; kept here for the
/// readable C++ namespace.
constexpr uint8_t KEY_FLAG_LONG_PRESS = MOKYA_KEY_FLAG_LONG_PRESS;
constexpr uint8_t KEY_FLAG_HINT_ANY   = MOKYA_KEY_FLAG_HINT_ANY;

struct KeyEvent {
    mokya_keycode_t keycode;    ///< semantic keycode (see <mie/keycode.h>)
    bool            pressed;    ///< true = key-down, false = key-up
    uint32_t        now_ms;     ///< monotonic ms at event source
    uint8_t         flags = 0;  ///< KEY_FLAG_* bitmask (0 = short tap)
};

} // namespace mie
