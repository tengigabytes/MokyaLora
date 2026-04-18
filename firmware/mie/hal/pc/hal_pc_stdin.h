// SPDX-License-Identifier: MIT
// hal_pc_stdin.h — PC stdin key-event source (host-only test tooling).
//
// v2 is push-based: there is no IHalPort. This class sets the terminal to
// raw mode and exposes a non-blocking poll() returning KeyEvents ready for
// ImeLogic::process_key. now_ms must be supplied by the caller from the
// same monotonic clock used for tick().

#pragma once

#include <cstdint>
#include <mie/hal_port.h>

namespace mie {
namespace pc {

class HalPcStdin {
public:
    HalPcStdin();
    ~HalPcStdin();

    /// Non-blocking. On a mapped key press, fills `out` with a KeyEvent
    /// (keycode + pressed=true + now_ms) and returns true. On ESC, returns
    /// true with keycode == MOKYA_KEY_NONE (signal to caller to quit).
    /// Returns false if no input is pending or the key is unmapped.
    bool poll(KeyEvent& out, uint32_t now_ms);

private:
    void set_raw_mode(bool enable);
    int  read_pc_key();

    bool raw_mode_active_;
};

} // namespace pc
} // namespace mie
