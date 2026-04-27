/* watchdog_task.h — Core 1 hardware watchdog kicker + Core 0 liveness check.
 *
 * Owns the RP2350 hardware watchdog. Once started, this task:
 *   1. Polls g_ipc_shared.c0_heartbeat every WD_TICK_MS (200 ms).
 *   2. If the heartbeat advanced since the last tick, kicks the HW
 *      watchdog (watchdog_update) and resets the silent-tick counter.
 *   3. If the heartbeat has been stalled for ≥ WD_SILENT_LIMIT_TICKS
 *      consecutive ticks AND wd_pause is zero, STOPS kicking — letting
 *      the HW watchdog (3 s timeout) fire a chip-wide reset.
 *   4. While wd_pause != 0, kicks unconditionally and skips silence
 *      detection (long blocking ops use this to opt out — see
 *      mokya_watchdog_pause/resume in ipc_shared_layout.h).
 *
 * Hang model:
 *   - Core 0 hangs   → c0_heartbeat stalls → wd_task stops kicking →
 *                      HW watchdog resets chip after ~3 s. Total
 *                      detection-to-reset budget ≈ 7 s.
 *   - Core 1 hangs   → wd_task no longer runs → no kicks → HW watchdog
 *                      resets chip after 3 s.
 *   - Both hang      → same as Core 1 hang (HW watchdog wins).
 *   - Long flash op  → caller wraps in pause/resume → kicks continue,
 *                      no false reset.
 *
 * Watchdog enable timing:
 *   wd_task itself enables the HW watchdog on its first iteration —
 *   never in main() before the scheduler. This guarantees the
 *   3 s budget begins after Core 0 setup() and Core 1 boot are
 *   complete, dodging the slow-boot flash-burst race that would fire
 *   spuriously if we enabled in initVariant().
 *
 * NOTE: this task uses watchdog_update() to kick. It does NOT call
 * watchdog_reboot_pc() (which would write WATCHDOG.SCRATCH4..7 — the
 * same registers used by the QMI-wedge SWD recovery escape hatch in
 * `reference_qmi_wedge_swd_recovery.md`). If reboot is needed elsewhere,
 * use watchdog_reboot(0,0,0) which leaves SCRATCH alone.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WD_HW_TIMEOUT_MS       3000u  /* HW watchdog fires after this much un-kicked time */
#define WD_TICK_MS              200u  /* wd_task polling cadence */
#define WD_SILENT_LIMIT_TICKS    20u  /* 20 × 200 ms = 4 s without c0_heartbeat advance */

/* SWD-observable counters — diagnose which path triggered a reset.
 * The high byte of g_wd_state encodes the last action (1=kick,
 * 2=paused-kick, 3=silent-no-kick), low 24 bits = monotonic kick
 * count. Packed to keep BSS overhead minimal. */
extern volatile uint32_t g_wd_state;
extern volatile uint32_t g_wd_silent_max;     /* highest silent_ticks observed so far */

/* Create the watchdog task. Returns pdPASS on success.  Idempotent —
 * second call is a no-op. */
BaseType_t watchdog_task_start(UBaseType_t priority);

#ifdef __cplusplus
}
#endif
