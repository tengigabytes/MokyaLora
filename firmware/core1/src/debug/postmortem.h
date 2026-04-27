/* postmortem.h — Core 1 panic / fault / watchdog-silence snapshot.
 *
 * Counterpart to mokya_postmortem.h (shared struct, MIT). This file
 * provides the Core-1-specific glue:
 *
 *   1. `mokya_pm_snapshot_silent` — called by wd_task right before it
 *      stops kicking the HW watchdog, so the about-to-fire reset has
 *      a record of who was running and what the WD state looked like.
 *
 *   2. Strong overrides of pico-sdk's weak `isr_hardfault` /
 *      `isr_memmanage` / `isr_busfault` / `isr_usagefault` symbols.
 *      The naked stub passes the active stack frame and EXC_RETURN
 *      magic to a C handler which captures CPU + SCB context, then
 *      forces a watchdog-style reset via SCB AIRCR SYSRESETREQ.
 *
 *   3. `mokya_pm_surface` — runs once at the top of main(), checks
 *      g_ipc_shared.postmortem_c1.magic, and if valid emits a TRACE()
 *      line via SEGGER RTT before clearing the magic. Idempotent.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mokya_postmortem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Capture a WD_SILENT snapshot. Call from wd_task once it observes that
 * c0_heartbeat has been stalled long enough to stop kicking. Idempotent
 * if magic is already set (subsequent calls do nothing — first event
 * wins). */
void mokya_pm_snapshot_silent(uint32_t c0_heartbeat,
                              uint32_t wd_state,
                              uint32_t wd_silent_max,
                              uint32_t wd_pause);

/* Print and clear any pending postmortem record left by the previous
 * boot. Safe to call before SEGGER_RTT_Init(); writes go through TRACE
 * which init's RTT lazily. */
void mokya_pm_surface_on_boot(void);

/* SWD-controllable fault injector. Polled from bridge_task each loop;
 * when g_mokya_pm_test_force_fault != 0 the polled core takes a load
 * BusFault, which exercises the fault handler / postmortem capture +
 * SYSRESETREQ path end-to-end. Default 0 → no behaviour. */
extern volatile uint32_t g_mokya_pm_test_force_fault;
void mokya_pm_test_poll(void);

#ifdef __cplusplus
}
#endif
