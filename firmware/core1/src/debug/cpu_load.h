/* cpu_load.h — Core 1 CPU load estimator.
 *
 * Method: vApplicationIdleHook() bumps a 32-bit counter on every iteration
 * of the FreeRTOS idle task. A separate 1 Hz sample task computes the
 * delta over each 1-second window. The first non-zero delta is taken as
 * the "100% idle" baseline; subsequent windows compute
 *   busy_pct = max(0, 100 - 100 * delta / baseline)
 *
 * Limitations:
 *  - Resolution: 1 second windows. For sub-second spikes use RTT trace.
 *  - Approximation: assumes the idle hook runs at a fixed rate when
 *    there's no other work. Holds within ~5% if no FPU-heavy idle task
 *    pre-emption occurs (we don't have any).
 *  - Auto-recalibration: if a future window's delta exceeds the recorded
 *    baseline by >5%, the baseline is bumped up to that delta. Handles
 *    clock changes / first-boot transient correctly.
 *
 * Optional 10s rolling average is kept so the CPU page can show both
 * instantaneous and smoothed values.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "portmacro.h"

/* Start the 1 Hz sample task. Must be called after the FreeRTOS scheduler
 * is running (or queued before vTaskStartScheduler — task creation is
 * scheduler-safe pre-start). Returns true on success. */
bool     cpu_load_start(UBaseType_t priority);

/* Latest 1-second window CPU busy percent, 0..100. UINT8_MAX = baseline
 * not yet established (first ~2 seconds of boot). */
uint8_t  cpu_load_pct_instant(void);

/* 10-second rolling average busy percent, 0..100. UINT8_MAX = not yet
 * 10 windows since boot. */
uint8_t  cpu_load_pct_avg10(void);

/* Number of valid 1 s windows captured since boot. Caps at UINT32_MAX. */
uint32_t cpu_load_window_count(void);

/* SWD-readable mirrors. */
extern volatile uint32_t g_cpu_idle_count;          /* total idle ticks since boot */
extern volatile uint32_t g_cpu_idle_baseline;       /* "100% idle" delta calibration */
extern volatile uint8_t  g_cpu_load_pct_instant;    /* mirror of cpu_load_pct_instant */
extern volatile uint8_t  g_cpu_load_pct_avg10;      /* mirror of cpu_load_pct_avg10 */
extern volatile uint32_t g_cpu_load_windows;
