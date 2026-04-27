/* msp_canary.h — Core 1 main-stack-pointer (MSP) high-water tracker.
 *
 * Background: Core 1's linker carve-out (memmap_core1_bridge.ld) is fully
 * packed — `.bss` ends at 0x200797EC and `.heap` (2 KB) abuts it up to
 * `__StackTop = 0x2007A000`. The MSP starts at `__StackTop` and grows
 * down through the `.heap` region first; once that 2 KB is consumed it
 * silently corrupts the tail of `.bss` (i.e. `ucHeap` / LVGL pool).
 * `configCHECK_FOR_STACK_OVERFLOW = 2` only watches PSP task stacks, not
 * MSP, so an ISR-side overflow has no detector today.
 *
 * Design: at boot, fill the unused part of `.heap` (from `__end__` up to
 * a safety margin below the live MSP) with a known canary word. A scan
 * walks the same range from the bottom and reports the first non-canary
 * address — that is the deepest point MSP has ever reached.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSP_CANARY_WORD  0xDEADBEEFu

/* Fill the `.heap` region below the current SP with MSP_CANARY_WORD.
 * Call as early as possible from main() — before any deep call chain.
 * Internally leaves a 256 B safety margin below the current SP so the
 * caller's own frame is never overwritten. Idempotent. */
void msp_canary_init(void);

/* Walk the canary region from `__end__` upward and return the lowest
 * address whose word is no longer MSP_CANARY_WORD. That address is the
 * historical low-water mark of MSP (= deepest stack reach).
 *
 * Returns the address as a uint32_t; if every canary word is intact the
 * function returns __HeapLimit (= __StackTop), meaning MSP never dipped
 * into the canary region. */
uint32_t msp_canary_low_water_addr(void);

/* Convenience: bytes between historical MSP low-water and __StackTop.
 * Equivalent to (__StackTop - msp_canary_low_water_addr()). */
uint32_t msp_canary_peak_used(void);

/* SWD-readable mirror, refreshed by msp_canary_refresh(). */
extern volatile uint32_t g_msp_peak_used;
extern volatile uint32_t g_msp_low_water_addr;

/* Recompute g_msp_peak_used and g_msp_low_water_addr. Cheap (linear
 * scan over at most 2 KB / 4 = 512 words). Safe to call from any task
 * context. */
void msp_canary_refresh(void);

#ifdef __cplusplus
}
#endif
