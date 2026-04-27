/**
 * ipc_ringbuf.c — SPSC ring buffer implementation
 * SPDX-License-Identifier: MIT
 *
 * Compiled into both Core 0 (via Meshtastic PlatformIO build_src_filter) and
 * Core 1 (via the m1_bridge CMake target). This TU also provides the single
 * definition of g_ipc_shared — the linker places it in the .shared_ipc
 * NOLOAD section at absolute address 0x2007A000 on both sides.
 */

#include "ipc_ringbuf.h"
#include <string.h>

/* ── The single shared-SRAM instance ──────────────────────────────────────
 *
 * __attribute__((section(".shared_ipc"))) puts g_ipc_shared in a distinct
 * section. Both cores' linker scripts map .shared_ipc (NOLOAD) to
 * 0x2007A000 — the section is uninitialised (not part of the flash image),
 * not cleared by startup code, and survives reset until Core 0 explicitly
 * zeroes it via ipc_shared_init().
 *
 * `used` prevents LTO from GC'ing the symbol when a TU happens to only read
 * through &g_ipc_shared.c0_to_c1_ctrl (etc.).
 */
__attribute__((section(".shared_ipc"), used))
IpcSharedSram g_ipc_shared;

/* ── Public API ───────────────────────────────────────────────────────────*/

void ipc_shared_init(void)
{
    /* Zero EVERYTHING except the postmortem window — that is the one
     * region whose contents we want to preserve across watchdog and
     * SYSRESETREQ resets so the previous boot's panic snapshot can be
     * read by the surface path on the next boot.
     *
     * The breadcrumb slots at offsets 0x5FC4..0x5FCF (registered in
     * firmware-architecture §9.3) live inside _tail_pad_post — those
     * DO get cleared every boot, matching the original behaviour. */
    volatile uint8_t *p = (volatile uint8_t *)&g_ipc_shared;
    const size_t pm_start = offsetof(IpcSharedSram, postmortem_c0);
    const size_t pm_end   = offsetof(IpcSharedSram, postmortem_c1)
                          + sizeof(g_ipc_shared.postmortem_c1);
    const size_t total    = sizeof(g_ipc_shared);
    for (size_t i = 0; i < pm_start; ++i) p[i] = 0u;
    for (size_t i = pm_end; i < total; ++i) p[i] = 0u;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ipc_shared.boot_magic = IPC_BOOT_MAGIC;
}

bool ipc_ring_push(IpcRingCtrl *ctrl,
                   IpcRingSlot *slots,
                   uint32_t     slot_count,
                   uint8_t      msg_id,
                   uint8_t      seq,
                   const void  *payload,
                   uint16_t     payload_len)
{
    if (payload_len > IPC_MSG_PAYLOAD_MAX) {
        __atomic_fetch_add(&ctrl->overflow, 1u, __ATOMIC_RELAXED);
        return false;
    }

    /* Producer owns head (plain load). Consumer may advance tail at any
     * time, so tail is acquire-loaded. */
    const uint32_t head = ctrl->head;
    const uint32_t tail = __atomic_load_n(&ctrl->tail, __ATOMIC_ACQUIRE);

    if ((head - tail) >= slot_count) {
        __atomic_fetch_add(&ctrl->overflow, 1u, __ATOMIC_RELAXED);
        return false;
    }

    IpcRingSlot *slot = &slots[head % slot_count];
    slot->header.msg_id      = msg_id;
    slot->header.seq         = seq;
    slot->header.payload_len = payload_len;
    if (payload_len > 0u && payload != NULL) {
        memcpy(slot->payload, payload, payload_len);
    }

    /* Publish the slot: release-store head so the consumer's acquire-load
     * observes the slot contents. */
    __atomic_store_n(&ctrl->head, head + 1u, __ATOMIC_RELEASE);
    return true;
}

bool ipc_ring_pop(IpcRingCtrl *ctrl,
                  IpcRingSlot *slots,
                  uint32_t     slot_count,
                  IpcMsgHeader *header_out,
                  void         *payload_out,
                  size_t        max_payload)
{
    /* Consumer owns tail (plain load). Head is acquire-loaded so we pair
     * with the producer's release-store. */
    const uint32_t tail = ctrl->tail;
    const uint32_t head = __atomic_load_n(&ctrl->head, __ATOMIC_ACQUIRE);

    if (head == tail) {
        return false;
    }

    IpcRingSlot *slot = &slots[tail % slot_count];
    *header_out = slot->header;

    const uint16_t len = slot->header.payload_len;
    const size_t   n   = (len < max_payload) ? len : max_payload;
    if (n > 0u && payload_out != NULL) {
        memcpy(payload_out, slot->payload, n);
    }

    /* Release-store tail so the producer's tail-load sees the slot as free. */
    __atomic_store_n(&ctrl->tail, tail + 1u, __ATOMIC_RELEASE);
    return true;
}

uint32_t ipc_ring_pending(const IpcRingCtrl *ctrl, uint32_t slot_count)
{
    (void)slot_count;  /* pending count is head - tail regardless of ring size */
    const uint32_t head = __atomic_load_n(&ctrl->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&ctrl->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

uint32_t ipc_ring_free_slots(const IpcRingCtrl *ctrl, uint32_t slot_count)
{
    return slot_count - ipc_ring_pending(ctrl, slot_count);
}
