#pragma once
/**
 * hal_port.h — MokyaInput Engine HAL Abstract Interface
 *
 * Platform-specific implementations live in:
 *   hal/rp2350/   — RP2350 PIO keyboard scan integration
 *   hal/pc/       — PC keyboard stub for unit testing
 *
 * All public MIE core code depends only on this header, never on a
 * concrete platform implementation.
 *
 * Key events carry a compact semantic keycode (see <mie/keycode.h>); the
 * MIE layer has no knowledge of the 6×6 matrix. Matrix → keycode
 * translation is performed once inside Core 1's KeypadScan before events
 * enter the queue.
 */

#include <cstdint>
#include <mie/keycode.h>

namespace mie {

/**
 * A single key event delivered from the platform HAL to IME-Logic.
 *
 * `keycode` is a value from <mie/keycode.h> (0x01..0x3F). 0x00
 * (MOKYA_KEY_NONE) is reserved as a sentinel and must not be passed to
 * `ImeLogic::process_key` — HAL implementations that want to signal
 * non-key events (e.g. the PC REPL's ESC-to-quit) should do so
 * out-of-band, not via KeyEvent.
 */
struct KeyEvent {
    mokya_keycode_t keycode;    ///< semantic keycode (see <mie/keycode.h>)
    bool            pressed;    ///< true = key down, false = key up
};

/**
 * HAL interface that each platform must implement.
 * IME-Logic calls poll() once per scan cycle to drain pending events.
 */
class IHalPort {
public:
    virtual ~IHalPort() = default;

    /**
     * Return the next pending key event, or false if the queue is empty.
     * Must be non-blocking.
     */
    virtual bool poll(KeyEvent& out) = 0;
};

} // namespace mie
