/* MokyaLora Phase 2 M1.1-A — Core 1 IPC bridge (bare metal)
 *
 * Successor to the M1.0b bootspike. Still bare-metal (no Pico SDK, no
 * FreeRTOS, no TinyUSB — those come in M1.1-B), but this time the image is
 * linked against the shared IPC ring code and actively drains the
 * c0_to_c1 direction while echoing traffic back on c1_to_c0.
 *
 * Verification is still SWD-only:
 *   0x20078000   uint32 — alive sentinel, set to BRIDGE_SENTINEL_VALUE at boot
 *   0x20078004   uint32 — rx byte counter (total bytes drained from c0_to_c1)
 *   0x20078008   uint32 — tx byte counter (total bytes pushed to c1_to_c0)
 *   0x2007800C   uint32 — last drain iteration loop count (liveness)
 *
 * Behaviour:
 *   1. Zero .bss, copy .data from flash, then enter main_core1().
 *   2. Spin on g_ipc_shared.boot_magic until Core 0 publishes IPC_BOOT_MAGIC.
 *   3. Publish c1_ready = 1 and push a single IPC_MSG_LOG_LINE greeting.
 *   4. Enter a forever loop that:
 *        a. pops at most IPC_MSG_PAYLOAD_MAX bytes from c0_to_c1
 *        b. immediately pushes them back into c1_to_c0 as IPC_MSG_SERIAL_BYTES
 *        c. updates the breadcrumbs at 0x20078004..0x2007800C
 *        d. wfe's briefly when both rings are idle to avoid pegging SRAM
 *
 * This is enough to prove end-to-end: Core 0's Meshtastic Serial output
 * (every MESH log line, every protobuf fragment) will rotate through the
 * ring and come back on the reverse ring, which SWD can inspect directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ipc_protocol.h"
#include "ipc_shared_layout.h"
#include "ipc_ringbuf.h"

#define BRIDGE_SENTINEL_ADDR   0x20078000u
#define BRIDGE_SENTINEL_VALUE  0xC1B01100u
#define BRIDGE_RX_COUNT_ADDR   0x20078004u
#define BRIDGE_TX_COUNT_ADDR   0x20078008u
#define BRIDGE_LOOP_COUNT_ADDR 0x2007800Cu

/* ── Minimal libc stubs ──────────────────────────────────────────────────
 *
 * ipc_ringbuf.c calls memcpy() with a runtime-variable length, which GCC
 * emits as an external call. The m1_bridge image is linked with -nostdlib,
 * so we provide a tiny implementation here rather than pull in newlib.
 * Declared non-static and with the exact `memcpy` name so the linker
 * resolves ipc_ringbuf.c's reference.
 *
 * Likewise memset is provided on the off chance GCC lowers a large
 * zero-initialisation through it (currently none, but future ring-side
 * helpers may).
 */
__attribute__((used))
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) { *d++ = *s++; }
    return dst;
}

__attribute__((used))
void *memset(void *dst, int v, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) { *d++ = (uint8_t)v; }
    return dst;
}

extern uint32_t __stack_top__;
extern uint32_t __bss_start__;
extern uint32_t __bss_end__;
extern uint32_t __data_start__;
extern uint32_t __data_end__;
extern uint32_t __data_load__;

static void main_core1(void);

__attribute__((used, noreturn))
void reset_handler(void)
{
    /* Copy initialised data (LMA=FLASH, VMA=RAM). */
    {
        const uint32_t *src = &__data_load__;
        uint32_t *dst = &__data_start__;
        while (dst < &__data_end__) {
            *dst++ = *src++;
        }
    }

    /* Zero .bss. NOTE: the linker places .shared_ipc in its own NOLOAD region
     * above this image's RAM, so *(.bss*) does NOT cover it and this loop
     * will not clobber the IPC state Core 0 published. */
    for (uint32_t *p = &__bss_start__; p < &__bss_end__; ++p) {
        *p = 0u;
    }

    main_core1();
    for (;;) {
        __asm volatile ("wfi");
    }
}

/* Cortex-M vector table — only [0] (initial SP) and [1] (reset handler) are
 * consumed by Core 0's multicore_launch_core1_raw() handshake. */
__attribute__((section(".vectors"), used))
const void *const vector_table[48] = {
    (void *)(&__stack_top__),
    (void *)(reset_handler),
};

/* ── Breadcrumb writers ──────────────────────────────────────────────────── */

static inline void dbg_u32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

static void main_core1(void)
{
    /* 1. Publish alive sentinel. Distinct value from the M1.0b bootspike
     *    (0xC1B00701) so a quick SWD read of 0x20078000 tells us which
     *    image is actually running. */
    dbg_u32(BRIDGE_SENTINEL_ADDR,   BRIDGE_SENTINEL_VALUE);
    dbg_u32(BRIDGE_RX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_TX_COUNT_ADDR,   0u);
    dbg_u32(BRIDGE_LOOP_COUNT_ADDR, 0u);
    __asm volatile ("dmb 0xF" ::: "memory");

    /* 2. Wait for Core 0 to publish the shared-SRAM boot magic. This is the
     *    handshake that tells us ipc_shared_init() has zeroed the rings. If
     *    Core 0 never gets that far we spin here — SWD can still see the
     *    sentinel, and the boot_magic word can be inspected directly. */
    while (__atomic_load_n(&g_ipc_shared.boot_magic, __ATOMIC_ACQUIRE)
           != IPC_BOOT_MAGIC) {
        __asm volatile ("yield");
    }

    /* 3. Announce ourselves to Core 0 and stamp c1_ready. The greeting uses
     *    IPC_MSG_LOG_LINE so Core 0's Stream consumer (M1.1-B and beyond)
     *    can recognise it as a debug log rather than opaque bytes. */
    {
        const IpcPayloadLogLine hdr = {
            .level    = 1u,  /* INFO */
            .core     = 1u,
            .text_len = 16u, /* len("core1 bridge up") + 1 NUL */
        };
        uint8_t packet[sizeof(hdr) + 16u];
        __builtin_memcpy(packet, &hdr, sizeof(hdr));
        __builtin_memcpy(packet + sizeof(hdr), "core1 bridge up", 16u);
        (void)ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                            g_ipc_shared.c1_to_c0_slots,
                            IPC_MSG_LOG_LINE,
                            0u,
                            packet,
                            (uint16_t)sizeof(packet));
    }

    __atomic_store_n(&g_ipc_shared.c1_ready, 1u, __ATOMIC_RELEASE);

    /* 4. Forever: drain c0_to_c1, echo back on c1_to_c0, update breadcrumbs. */
    uint32_t rx_total = 0;
    uint32_t tx_total = 0;
    uint32_t loops    = 0;
    uint8_t  scratch[IPC_MSG_PAYLOAD_MAX];
    uint8_t  echo_seq = 0;

    for (;;) {
        loops++;
        if ((loops & 0xFFu) == 0u) {
            dbg_u32(BRIDGE_LOOP_COUNT_ADDR, loops);
        }

        IpcMsgHeader hdr;
        bool got = ipc_ring_pop(&g_ipc_shared.c0_to_c1_ctrl,
                                g_ipc_shared.c0_to_c1_slots,
                                &hdr,
                                scratch,
                                sizeof(scratch));
        if (!got) {
            /* Ring empty — spin. We deliberately do NOT wfe here: Core 0's
             * ipc_ring_push issues no sev, so once Core 1 parked in WFE it
             * would never wake until an interrupt fired. A tight spin is
             * the simplest M1.1-A solution; M1.1-B replaces this with a
             * FreeRTOS task notification / sev pair. */
            continue;
        }

        if (hdr.msg_id == IPC_MSG_SERIAL_BYTES && hdr.payload_len > 0u) {
            rx_total += hdr.payload_len;
            dbg_u32(BRIDGE_RX_COUNT_ADDR, rx_total);

            bool pushed = ipc_ring_push(&g_ipc_shared.c1_to_c0_ctrl,
                                        g_ipc_shared.c1_to_c0_slots,
                                        IPC_MSG_SERIAL_BYTES,
                                        echo_seq++,
                                        scratch,
                                        hdr.payload_len);
            if (pushed) {
                tx_total += hdr.payload_len;
                dbg_u32(BRIDGE_TX_COUNT_ADDR, tx_total);
            }
        }
        /* Non-SERIAL_BYTES messages are dropped silently for M1.1-A. */
    }
}
