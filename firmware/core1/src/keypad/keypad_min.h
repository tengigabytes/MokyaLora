/* keypad_min.h — Phase A: minimal 6x6 keypad scanner for Core 1.
 *
 * This is a deliberately tiny module whose only job is to prove that
 * Core 1, running under the m1_bridge image with runtime_init_clocks /
 * post_clock_resets skipped, can control GPIO 36-47 through the Pico SDK
 * abstraction. If this module reads key presses correctly, Phase B will
 * add debounce + keymap translation + KeyEvent queue on top. If it
 * fails, the diagnosis narrows immediately to an SDK-level issue (pad
 * ISO, IO_BANK0 unreset, or similar).
 *
 * No PIO. No DMA. No interrupts. No debounce. No keycode translation.
 * Polling only, via the same register access the bringup firmware uses
 * (i2c_custom_scan.c::key_scan_matrix).
 *
 * Hardware layout (hardware-requirements.md, bringup_pins.h):
 *   Rows    GPIO 42-47 — output, driven LOW to select
 *   Columns GPIO 36-41 — input with pull-up, read LOW when pressed
 *   Diode SDM03U40: Anode = COL, Cathode = ROW
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#define KEY_COL_BASE  36u
#define KEY_ROW_BASE  42u
#define KEY_COLS       6u
#define KEY_ROWS       6u

/* Configure GPIOs for keypad scanning. Safe to call once from a task
 * after the FreeRTOS scheduler is running. */
void keypad_min_init(void);

/* Perform one full 6-row scan. out_state[r] bit c = 1 means (row r, col c)
 * is pressed. Blocks for ~60 us total (10 us settling * 6 rows). */
void keypad_min_scan_once(uint8_t out_state[KEY_ROWS]);

/* Latest scan result, updated by keypad_probe_task in main_core1_bridge.c.
 * SWD-observable via `mem8 <addr-of-g_kp_snapshot> 6`. No breadcrumb. */
extern volatile uint8_t g_kp_snapshot[KEY_ROWS];
