/* MokyaLora Phase 2 M1.0b — Core 1 bootspike
 *
 * Minimal Core 1 Apache-2.0 image that proves Core 0 can hand execution to a
 * separate flash image at 0x10200000 via multicore_launch_core1_raw().
 *
 * Behaviour:
 *   1. Write a well-known sentinel value to a fixed SRAM address.
 *   2. Issue a data memory barrier so Core 0 observes the write.
 *   3. Halt in WFI forever.
 *
 * Core 0 verifies the sentinel via SWD after boot. There is no IPC ring, no
 * peripheral access, no clock setup — the sentinel write and CPUID reads are
 * the entire proof of life.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#define SENTINEL_ADDR  0x20078000u
#define SENTINEL_VALUE 0xC1B00701u

extern uint32_t __stack_top__;
extern uint32_t __bss_start__;
extern uint32_t __bss_end__;

static void main_core1(void);

__attribute__((used, noreturn))
void reset_handler(void)
{
    /* Zero-initialise .bss. main_core1() uses no static data today, but do
     * this unconditionally so the spike generalises to any future addition. */
    for (uint32_t *p = &__bss_start__; p < &__bss_end__; ++p) {
        *p = 0u;
    }
    main_core1();
    for (;;) {
        __asm volatile ("wfi");
    }
}

/* Cortex-M vector table. Only the first two entries matter — Core 0 reads
 * them from 0x10200000 and passes them to multicore_launch_core1_raw() as
 * (sp, entry). The remaining slots are zeroed; no interrupts are enabled. */
__attribute__((section(".vectors"), used))
const void *const vector_table[48] = {
    (void *)(&__stack_top__),   /* 0:  initial MSP                        */
    (void *)(reset_handler),    /* 1:  reset handler (Thumb bit via fn ptr) */
    /* 2..47 NMI, HardFault, SVCall, PendSV, SysTick, IRQ0..31: all zero. */
};

static void main_core1(void)
{
    volatile uint32_t *const sentinel = (uint32_t *)SENTINEL_ADDR;
    *sentinel = SENTINEL_VALUE;
    __asm volatile ("dmb 0xF" ::: "memory");
}
