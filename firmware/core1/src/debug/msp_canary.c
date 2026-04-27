/* msp_canary.c — see msp_canary.h for rationale.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "msp_canary.h"

#include <stdint.h>

/* Linker-provided bounds. `__end__` is the lowest address of `.heap`
 * (just past `.bss`); `__HeapLimit` is one past the last byte of the
 * RAM region (= `__StackTop`). Both are 4-byte aligned by the linker
 * script. */
extern uint32_t __end__;
extern uint32_t __HeapLimit;

volatile uint32_t g_msp_peak_used      = 0;
volatile uint32_t g_msp_low_water_addr = 0;

/* 256 B is comfortably above any plausible single-frame stack usage
 * during boot — main() entry, msp_canary_init() itself, and the SDK
 * crt0 epilogue all fit in well under that. */
#define MSP_FILL_SAFETY_MARGIN_B  256u

void msp_canary_init(void)
{
    register uint32_t sp;
    __asm volatile ("mov %0, sp" : "=r"(sp));

    uint32_t fill_lo = (uint32_t)&__end__;
    uint32_t fill_hi = (sp - MSP_FILL_SAFETY_MARGIN_B) & ~3u;

    /* If the safety margin already pushes us below `.heap`, there is
     * nothing safe to fill — bail out without touching memory. */
    if (fill_hi <= fill_lo) {
        return;
    }

    uint32_t *p   = (uint32_t *)fill_lo;
    uint32_t *end = (uint32_t *)fill_hi;
    while (p < end) {
        *p++ = MSP_CANARY_WORD;
    }
}

uint32_t msp_canary_low_water_addr(void)
{
    const uint32_t *p   = (const uint32_t *)&__end__;
    const uint32_t *end = (const uint32_t *)&__HeapLimit;

    while (p < end && *p == MSP_CANARY_WORD) {
        ++p;
    }
    return (uint32_t)p;
}

uint32_t msp_canary_peak_used(void)
{
    return (uint32_t)&__HeapLimit - msp_canary_low_water_addr();
}

void msp_canary_refresh(void)
{
    uint32_t addr = msp_canary_low_water_addr();
    g_msp_low_water_addr = addr;
    g_msp_peak_used      = (uint32_t)&__HeapLimit - addr;
}
