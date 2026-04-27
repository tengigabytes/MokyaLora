/**
 * mokya_postmortem.h — Cross-reset postmortem capture
 * SPDX-License-Identifier: MIT
 *
 * RP2350 SRAM contents survive watchdog reset and SYSRESETREQ; only POR/BOR
 * clears it. By placing two `mokya_postmortem_t` slots at the tail of
 * `g_ipc_shared` and excluding them from `ipc_shared_init`'s zero-init,
 * we get a writable region that persists across the reset boundary.
 *
 * Capture points:
 *   1. wd_task on Core 1 — when c0_heartbeat silence ≥ 4 s and the kicker
 *      is about to stop, it snapshots state with cause=WD_SILENT before
 *      letting the HW watchdog reset the chip.
 *   2. Cortex-M fault handlers on both cores — `isr_hardfault`,
 *      `isr_memmanage`, `isr_busfault`, `isr_usagefault` (overrides the
 *      pico-sdk weak symbols) capture the stacked exception frame plus
 *      CFSR/HFSR/MMFAR/BFAR, then trigger SYSRESETREQ.
 *   3. RebootNotifier on Core 0 — graceful reboot path tags
 *      cause=GRACEFUL_REBOOT for visibility.
 *
 * Surface point: each core's main() reads its slot's `magic` very early
 * after boot. If valid, the slot is printed via RTT (Core 1) or Serial
 * (Core 0) and then the magic is cleared so the same event isn't logged
 * twice on a subsequent reboot.
 *
 * The struct is wire-format-stable across both cores. Layout assertions
 * below catch field drift.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOKYA_PM_MAGIC          0xDEADC0DEu
#define MOKYA_PM_TASK_NAME_MAX  16u

typedef enum {
    MOKYA_PM_CAUSE_NONE             = 0,
    MOKYA_PM_CAUSE_WD_SILENT        = 1,  /* Core 1 wd_task silence detect */
    MOKYA_PM_CAUSE_HARDFAULT        = 2,
    MOKYA_PM_CAUSE_MEMMANAGE        = 3,
    MOKYA_PM_CAUSE_BUSFAULT         = 4,
    MOKYA_PM_CAUSE_USAGEFAULT       = 5,
    MOKYA_PM_CAUSE_PANIC            = 6,  /* explicit panic() / __breakpoint */
    MOKYA_PM_CAUSE_GRACEFUL_REBOOT  = 7,  /* Core 0 RebootNotifier path */
} mokya_pm_cause_t;

/**
 * Per-core postmortem slot. 128 B fixed — layout asserted below.
 *
 * Field semantics:
 *   magic        — set to MOKYA_PM_MAGIC by capture path; cleared by surface path.
 *   cause        — mokya_pm_cause_t.
 *   timestamp_us — microseconds since boot (timer_hw->timerawl).
 *   core         — originating core id (0 or 1).
 *   pc..r12      — Cortex-M stacked exception frame copies. For non-fault
 *                  causes (WD_SILENT, GRACEFUL_REBOOT) these are zero.
 *   exc_return   — value of LR at handler entry (the EXC_RETURN magic that
 *                  encodes whether MSP/PSP was active and FP frame state).
 *   cfsr/hfsr    — SCB Configurable / HardFault status (SCB_CFSR/HFSR).
 *   mmfar/bfar   — MemManage / BusFault address registers.
 *   c0_heartbeat — value of g_ipc_shared.c0_heartbeat at capture time.
 *   wd_state     — Core 1 g_wd_state at capture (high byte = last action).
 *   wd_silent_max— Core 1 g_wd_silent_max at capture.
 *   wd_pause     — value of g_ipc_shared.wd_pause at capture (≠ 0 means
 *                  some long blocking op was opted out of silence detect).
 *   task_name    — NUL-terminated FreeRTOS current task name (or "" if
 *                  scheduler hasn't started). Useful to correlate wd_silent
 *                  with which task starved Core 0 idle.
 *   reserved     — pads to 128 B; kept zero so future fields can be added
 *                  without breaking surface-path parsers.
 */
typedef struct {
    uint32_t magic;          /* +0x00 */
    uint32_t cause;          /* +0x04 */
    uint32_t timestamp_us;   /* +0x08 */
    uint8_t  core;           /* +0x0C */
    uint8_t  _rsv0;          /* +0x0D */
    uint16_t _rsv1;          /* +0x0E */

    uint32_t pc;             /* +0x10 */
    uint32_t lr;             /* +0x14 */
    uint32_t sp;             /* +0x18 */
    uint32_t psr;            /* +0x1C */

    uint32_t r0;             /* +0x20 */
    uint32_t r1;             /* +0x24 */
    uint32_t r2;             /* +0x28 */
    uint32_t r3;             /* +0x2C */

    uint32_t r12;            /* +0x30 */
    uint32_t exc_return;     /* +0x34 */
    uint32_t cfsr;           /* +0x38 */
    uint32_t hfsr;           /* +0x3C */

    uint32_t mmfar;          /* +0x40 */
    uint32_t bfar;           /* +0x44 */
    uint32_t c0_heartbeat;   /* +0x48 */
    uint32_t wd_state;       /* +0x4C */

    uint32_t wd_silent_max;  /* +0x50 */
    uint32_t wd_pause;       /* +0x54 */
    uint32_t _rsv2[2];       /* +0x58..0x5F */

    char     task_name[MOKYA_PM_TASK_NAME_MAX];  /* +0x60..0x6F */

    uint32_t _rsv3[4];       /* +0x70..0x7F — fill to 128 B */
} mokya_postmortem_t;

_Static_assert(sizeof(mokya_postmortem_t) == 128, "mokya_postmortem_t must be 128 B");

#ifdef __cplusplus
}
#endif
