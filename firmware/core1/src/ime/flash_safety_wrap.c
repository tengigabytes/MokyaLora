/* flash_safety_wrap.c — Core 1 side P2-11 fix for Core-1-originated flash writes.
 *
 * This is the mirror image of
 * firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/flash_safety_wrap.c.
 * That file covers the case where Core 0 writes flash and parks Core 1; this
 * file covers the reverse direction introduced in Phase 1.6 (LRU persist).
 *
 * Protocol (Core 1 writes, Core 0 parks):
 *   1. Core 1 sets flash_lock_c0 = REQUEST and rings IPC_FLASH_DOORBELL_C0.
 *   2. Core 0's SIO_IRQ_BELL ISR (installed by flash_park_listener.c in the
 *      Meshtastic variant dir) acknowledges with PARKED and spins in RAM.
 *   3. Core 1 disables its own interrupts, runs the real flash op, re-enables
 *      XIP cache, and restores interrupts.
 *   4. Core 1 clears flash_lock_c0 back to IDLE + __SEV().
 *
 * Linked in via -Wl,--wrap=flash_range_erase -Wl,--wrap=flash_range_program
 * (see firmware/core1/m1_bridge/CMakeLists.txt).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>

#include "pico.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"

#include "ipc_shared_layout.h"

/* XIP_CTRL register — hardcoded to avoid header dependency.
 * Bits 0-1 (EN_SECURE, EN_NONSECURE) enable the XIP cache.  ROM
 * flash_exit_xip() clears the register and boot2 only restores QMI —
 * never XIP_CTRL.  Re-enable after each flash op.
 * Use the SET alias for atomic bit-set without RMW.                      */
#define MOKYA_XIP_CTRL_SET  (*(volatile uint32_t *)0x400CA000u)
#define MOKYA_XIP_CACHE_EN  0x00000003u  /* EN_SECURE | EN_NONSECURE */

/* Bounded wait budget: ~5 ms at 150 MHz. Matches Core 0's wrap budget.  */
#define MOKYA_PARK_WAIT_SPINS  750000u

/* Provided by the --wrap linker mechanism */
extern void __real_flash_range_erase(uint32_t flash_offs, size_t count);
extern void __real_flash_range_program(uint32_t flash_offs,
                                       const uint8_t *data, size_t count);

/* ── Park / unpark helpers (MUST reside in RAM) ────────────────────────── */

static void __no_inline_not_in_flash_func(mokya_flash_park_core0)(
    uint32_t *saved_irq)
{
    /* Only try to park Core 0 if the Meshtastic side has a park listener
     * installed. The c0_ready flag is set by Core 0 once its IPC listener
     * (including flash park) is fully up; before that, the doorbell IRQ
     * won't be claimed and the request would hang forever. We still
     * disable our own interrupts below either way.                         */
    if (__atomic_load_n(&g_ipc_shared.c0_ready, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_ipc_shared.flash_lock_c0,
                         IPC_FLASH_LOCK_REQUEST, __ATOMIC_RELEASE);
        __compiler_memory_barrier();
        sio_hw->doorbell_out_set = 1u << IPC_FLASH_DOORBELL_C0;
        for (uint32_t i = 0; i < MOKYA_PARK_WAIT_SPINS; i++) {
            if (__atomic_load_n(&g_ipc_shared.flash_lock_c0,
                                __ATOMIC_ACQUIRE) == IPC_FLASH_LOCK_PARKED) {
                break;
            }
        }
    }
    *saved_irq = save_and_disable_interrupts();
}

static void __no_inline_not_in_flash_func(mokya_flash_unpark_core0)(
    uint32_t saved_irq)
{
    restore_interrupts(saved_irq);
    __atomic_store_n(&g_ipc_shared.flash_lock_c0,
                     IPC_FLASH_LOCK_IDLE, __ATOMIC_RELEASE);
    __compiler_memory_barrier();
    __sev();
}

/* ── Wrapped flash functions ───────────────────────────────────────────── */

void __no_inline_not_in_flash_func(__wrap_flash_range_erase)(
    uint32_t flash_offs, size_t count)
{
    uint32_t saved;
    mokya_flash_park_core0(&saved);
    __real_flash_range_erase(flash_offs, count);
    MOKYA_XIP_CTRL_SET = MOKYA_XIP_CACHE_EN;  /* re-enable cache */
    mokya_flash_unpark_core0(saved);
}

void __no_inline_not_in_flash_func(__wrap_flash_range_program)(
    uint32_t flash_offs, const uint8_t *data, size_t count)
{
    uint32_t saved;
    mokya_flash_park_core0(&saved);
    __real_flash_range_program(flash_offs, data, count);
    MOKYA_XIP_CTRL_SET = MOKYA_XIP_CACHE_EN;  /* re-enable cache */
    mokya_flash_unpark_core0(saved);
}
