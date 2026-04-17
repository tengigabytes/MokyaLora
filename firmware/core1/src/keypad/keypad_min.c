/* keypad_min.c — Phase A minimal scanner. See keypad_min.h for rationale.
 *
 * Implementation mirrors firmware/tools/bringup/i2c_custom_scan.c line for
 * line. The only intentional difference is that this module does not call
 * any Meshtastic or Arduino-Pico API — it is pure Pico SDK, so it can
 * link into the Core 1 m1_bridge image (Apache-2.0, no Arduino-Pico).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "keypad_min.h"

#include "hardware/gpio.h"
#include "pico/time.h"

volatile uint8_t g_kp_snapshot[KEY_ROWS];

void keypad_min_init(void)
{
    /* Columns: input with pull-up. When the selected row is driven LOW
     * and a key is pressed, current flows COL -> diode -> ROW, pulling
     * the column pin below VIL. */
    for (uint32_t c = 0; c < KEY_COLS; c++) {
        gpio_init(KEY_COL_BASE + c);
        gpio_set_dir(KEY_COL_BASE + c, GPIO_IN);
        gpio_pull_up(KEY_COL_BASE + c);
    }

    /* Rows: output, idle HIGH (unselected). Scan will drive one LOW at
     * a time and restore HIGH before moving to the next row. */
    for (uint32_t r = 0; r < KEY_ROWS; r++) {
        gpio_init(KEY_ROW_BASE + r);
        gpio_set_dir(KEY_ROW_BASE + r, GPIO_OUT);
        gpio_put(KEY_ROW_BASE + r, 1);
    }
}

void keypad_min_scan_once(uint8_t out_state[KEY_ROWS])
{
    for (uint32_t r = 0; r < KEY_ROWS; r++) {
        out_state[r] = 0u;
    }

    for (uint32_t r = 0; r < KEY_ROWS; r++) {
        gpio_put(KEY_ROW_BASE + r, 0);
        sleep_us(10);                  /* RC + diode settling */

        for (uint32_t c = 0; c < KEY_COLS; c++) {
            if (!gpio_get(KEY_COL_BASE + c)) {
                out_state[r] |= (uint8_t)(1u << c);
            }
        }

        gpio_put(KEY_ROW_BASE + r, 1);
    }
}
