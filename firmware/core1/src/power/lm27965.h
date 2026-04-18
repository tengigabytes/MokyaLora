/* lm27965.h — TI LM27965 3-bank white-LED driver on the power I2C bus.
 *
 * MokyaLora Rev A channel mapping (see docs/requirements/hardware-
 * requirements.md §8.4 + rev-a-bringup-log Issue #6/#12):
 *
 *   Bank A (D1A..D5A)  — TFT backlight (5 LEDs in parallel). Bank-A duty
 *                        controls the LCD backlight brightness. D37 (a
 *                        Rev A indicator) is miswired onto this bank;
 *                        accept the coupling for Rev A.
 *   Bank B (D1B, D2B)  — Keypad backlight (2 LED strings × 3 LEDs each).
 *                        The green status LED D3B is gated by a separate
 *                        GP.EN3B bit but shares Bank-B duty, so the
 *                        keypad BL and green LED cannot be dimmed
 *                        independently on Rev A.
 *   Bank C (D1C)       — Red status LED, single-LED string.
 *
 * Duty resolution (§ LM27965 datasheet):
 *   Bank A/B : 5-bit code, 32 steps. 0x00-0x0F=20%, 0x10-0x16=40%,
 *              0x17-0x1C=70%, 0x1D-0x1F=100%.
 *   Bank C   : 2-bit code; 00=20% 01=40% 10=70% 11=100%.
 *
 * Driver maintains a cached copy of the GP register so partial updates
 * (e.g. toggling the keypad-BL enable bit) do not clobber other banks.
 * No FreeRTOS task — purely API-driven. Bus access serialised by
 * i2c_bus_acquire / i2c_bus_release (see i2c_bus.h).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_LM27965_H
#define MOKYA_CORE1_LM27965_H

#include <stdbool.h>
#include <stdint.h>

/* Duty limits (inclusive). */
#define LM27965_DUTY_AB_MAX   31u   /* Bank A and B */
#define LM27965_DUTY_C_MAX     3u   /* Bank C (2-bit) */

/* Boot-time setup. Verifies I2C presence (best-effort) and applies the
 * requested TFT backlight duty with the panel backlight ON. Bank B (keypad
 * + green) and Bank C (red) start off. Returns true on I2C success.
 *
 * Typical call site: lvgl_task after display_init() so the panel is
 * initialised before the backlight turns on (avoids a brief garbage flash). */
bool     lm27965_init(uint8_t tft_duty);

/* TFT backlight. `duty` = 0 turns Bank A off; otherwise enables ENA with
 * the requested duty code (clamped to LM27965_DUTY_AB_MAX). */
bool     lm27965_set_tft_backlight(uint8_t duty);

/* Keypad backlight (ENB) and green status LED (EN3B) share Bank-B duty.
 * `kbd_on` controls D1B/D2B, `green_on` controls D3B. Any call updates
 * the bank duty atomically with the enable bits. */
bool     lm27965_set_keypad_backlight(bool kbd_on, bool green_on,
                                      uint8_t duty);

/* Red status LED (Bank C). `duty` is 0..LM27965_DUTY_C_MAX. */
bool     lm27965_set_led_red(bool on, uint8_t duty);

/* All-off: GP = 0x20 (ENA/ENB/ENC all cleared, reserved bit5 kept set).
 * Bank duty registers are not touched — last values persist for a fast
 * wake-up. Intended for the M3.5 sleep / DORMANT state machine. */
bool     lm27965_all_off(void);

/* Snapshot of driver state for UI / SWD observers. Not a direct register
 * read — reflects what the driver most recently wrote (cache), which is
 * what the chip contains as long as no other master touches 0x36. */
typedef struct {
    bool    online;         /* last I2C op succeeded                    */
    uint8_t gp;             /* GP register cache                        */
    uint8_t bank_a_duty;    /* 0..31                                    */
    uint8_t bank_b_duty;    /* 0..31                                    */
    uint8_t bank_c_duty;    /* 0..3                                     */
} lm27965_state_t;

const lm27965_state_t *lm27965_get_state(void);

#endif /* MOKYA_CORE1_LM27965_H */
