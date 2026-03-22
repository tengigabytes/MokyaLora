// hal_pc_stdin.h — PC stdin IHalPort declaration
// SPDX-License-Identifier: MIT

#pragma once
#include "../hal_port.h"

namespace mie {
namespace pc {

/// IHalPort implementation for PC host testing.
///
/// Sets the terminal to raw (non-canonical, no-echo) mode on construction
/// and restores it on destruction. poll() is non-blocking: it returns false
/// immediately if no key has been pressed.
class HalPcStdin : public IHalPort {
public:
    HalPcStdin();
    ~HalPcStdin() override;

    /// Non-blocking. Returns true and fills `out` if a key event is ready.
    bool poll(KeyEvent& out) override;

private:
    void set_raw_mode(bool enable);

    int  read_pc_key();   ///< Returns a KeyMapEntry::pc_key value or -1 if none
    bool raw_mode_active_;
};

} // namespace pc
} // namespace mie
