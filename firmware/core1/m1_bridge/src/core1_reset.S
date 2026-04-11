/* MokyaLora Phase 2 M1.1-B — Core 1 custom vector table + reset handler
 *
 * The Pico SDK's crt0.S `_reset_handler` unconditionally bounces any core
 * with CPUID != 0 back into the bootrom FIFO wait handler — it assumes
 * Core 1 is launched via `multicore_launch_core1(func)` which writes a
 * one-shot entry point, NOT as a standalone image with its own reset
 * handler. We launch this image via `multicore_launch_core1_raw()` at
 * flash 0x10200000, so we need our own vector table + reset handler that
 * skips the CPUID check and runs the standard C runtime startup path
 * (runtime_init → main → exit).
 *
 * Layout requirement:
 *   This file's .vectors section MUST land at the start of flash
 *   (0x10200000) so that Core 0's multicore_launch_core1_raw call reads
 *   word[0]=__StackTop and word[1]=mokya_core1_reset_handler. The linker
 *   script in memmap_core1_bridge.ld explicitly orders this file's
 *   .vectors BEFORE the SDK's crt0.S .vectors so our table wins slot 0.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

.syntax unified
.cpu cortex-m33
.thumb

/* ── Vector table ────────────────────────────────────────────────────────── */
.section .vectors, "ax"
.align 2
.global mokya_core1_vectors
.type   mokya_core1_vectors, %object
mokya_core1_vectors:
    .word __StackTop                      @  0  Initial SP
    .word mokya_core1_reset_handler + 1   @  1  Reset
    .word mokya_core1_default_isr + 1     @  2  NMI
    .word mokya_core1_default_isr + 1     @  3  HardFault
    .word mokya_core1_default_isr + 1     @  4  MemManage
    .word mokya_core1_default_isr + 1     @  5  BusFault
    .word mokya_core1_default_isr + 1     @  6  UsageFault
    .word mokya_core1_default_isr + 1     @  7  SecureFault
    .word 0                                @  8  Reserved
    .word 0                                @  9  Reserved
    .word 0                                @ 10  Reserved
    .word mokya_core1_default_isr + 1     @ 11  SVC  (overwritten by FreeRTOS)
    .word mokya_core1_default_isr + 1     @ 12  DebugMon
    .word 0                                @ 13  Reserved
    .word mokya_core1_default_isr + 1     @ 14  PendSV  (overwritten by FreeRTOS)
    .word mokya_core1_default_isr + 1     @ 15  SysTick (overwritten by FreeRTOS)
    /* 52 user IRQs — all weakly aliased to the default isr. Runtime handlers
     * (TinyUSB's USBCTRL_IRQ in particular) are installed via
     * irq_set_exclusive_handler(), which writes to the SDK's ram_vector_table
     * that VTOR is pointed at by runtime_init_install_ram_vector_table. So the
     * entries below are only relevant for the brief window between reset and
     * the ram_vector_table copy inside runtime_init. */
    .rept 52
    .word mokya_core1_default_isr + 1
    .endr
.size mokya_core1_vectors, . - mokya_core1_vectors

/* ── Reset handler ───────────────────────────────────────────────────────── */
.section .text.mokya_core1_reset_handler, "ax"
.align 2
.thumb_func
.global mokya_core1_reset_handler
.type   mokya_core1_reset_handler, %function
mokya_core1_reset_handler:
    @ On entry:
    @   - VTOR points at our vector table (written by
    @     multicore_launch_core1_raw via the bootrom handshake).
    @   - MSP has been loaded from word[0] of our vector table (0x20050000).
    @   - Core 0 has already brought up PLLs / clocks / flash XIP / USB regs
    @     and has zero-initialised g_ipc_shared in shared SRAM.
    @
    @ Our job is everything crt0.S `_reset_handler` does AFTER its CPUID
    @ check: .bss zero, .data copy, runtime_init, main, exit.

    /* 1. Zero .bss */
    ldr  r0, =__bss_start__
    ldr  r1, =__bss_end__
    movs r2, #0
1:  cmp  r0, r1
    bhs  2f
    str  r2, [r0], #4
    b    1b
2:

    /* 2. Copy .data from flash LMA to RAM VMA */
    ldr  r0, =__etext
    ldr  r1, =__data_start__
    ldr  r2, =__data_end__
3:  cmp  r1, r2
    bhs  4f
    ldr  r3, [r0], #4
    str  r3, [r1], #4
    b    3b
4:

    /* 3. Call into the C runtime: runtime_init → main → exit */
    ldr  r4, =runtime_init
    blx  r4
    ldr  r4, =main
    blx  r4
    ldr  r4, =exit
    blx  r4

    /* Should never return — hang the core if it does. */
5:  bkpt #0
    b    5b
.size mokya_core1_reset_handler, . - mokya_core1_reset_handler

/* ── Default ISR ─────────────────────────────────────────────────────────── */
.section .text.mokya_core1_default_isr, "ax"
.align 2
.thumb_func
.global mokya_core1_default_isr
.type   mokya_core1_default_isr, %function
mokya_core1_default_isr:
    b    .
.size mokya_core1_default_isr, . - mokya_core1_default_isr
