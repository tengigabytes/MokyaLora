/* keypad_scan.h — 6x6 keypad matrix scanner (PIO + DMA).
 *
 * Public API for the MokyaLora Rev A keypad. Once keypad_init() has been
 * called, a PIO state machine continuously cycles through the 6 rows and
 * two DMA channels shuttle row masks / column readings between RAM and
 * the PIO FIFOs. The CPU never touches the PIO FIFOs after init — callers
 * just invoke keypad_read() to snapshot the latest matrix state.
 *
 * Hardware layout (hardware-requirements.md, bringup_pins.h):
 *   Rows    GPIO 42-47 — driven LOW to select one at a time
 *   Columns GPIO 36-41 — pull-up, read LOW when pressed
 *
 * Peripheral ownership: PIO0 SM + 2 DMA channels (claimed dynamically).
 * PIO1 is reserved for the display driver.
 *
 * Threading: keypad_init() must run once before anything calls
 * keypad_read(). keypad_read() is safe from any task — it only reads a
 * RAM ring that DMA writes to, with no intervening state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#define KEY_COL_BASE   36u
#define KEY_ROW_BASE   42u
#define KEY_COLS        6u
#define KEY_ROWS        6u

/* Claim PIO0 SM + 2 DMA channels, load the scan program, start the DMAs,
 * and enable the SM. Safe to call once from a FreeRTOS task after the
 * scheduler is running. */
void keypad_init(void);

/* Snapshot the current matrix state into out_state[KEY_ROWS]. Each byte
 * is one row's column-pressed bitmap: bit C set means row R col C is
 * pressed (inverted from the raw active-low reading). Returns in <100 ns
 * — just 6 memory reads with inversion, no PIO interaction. */
void keypad_read(volatile uint8_t out_state[KEY_ROWS]);

/* SWD-observable snapshot, updated at ~20 ms rate by the probe task in
 * main_core1_bridge.c. Equivalent to polling keypad_read() from outside. */
extern volatile uint8_t g_kp_snapshot[KEY_ROWS];
