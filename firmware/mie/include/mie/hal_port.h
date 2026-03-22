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
 */

#include <cstdint>

namespace mie {

/**
 * A single key event delivered from the platform HAL to IME-Logic.
 */
struct KeyEvent {
    uint8_t row;        ///< Matrix row index  (0–5)
    uint8_t col;        ///< Matrix column index (0–5)
    bool    pressed;    ///< true = key down, false = key up
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
