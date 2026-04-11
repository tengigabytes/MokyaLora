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

/* ── Internal helpers ─────────────────────────────────────────────────────*/

static inline uint32_t mask_index(uint32_t idx)
{
    return idx % IPC_RING_SLOT_COUNT;
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void ipc_shared_init(void)
{
    /* memset the entire layout, then publish the magic. Using a byte loop
     * rather than memset keeps us independent of libc availability on the
     * Core 1 crt0 path. */
    volatile uint8_t *p = (volatile uint8_t *)&g_ipc_shared;
    for (size_t i = 0; i < sizeof(g_ipc_shared); ++i) {
        p[i] = 0u;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ipc_shared.boot_magic = IPC_BOOT_MAGIC;
}

bool ipc_ring_push(IpcRingCtrl *ctrl,
                   IpcRingSlot *slots,
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

    if ((head - tail) >= IPC_RING_SLOT_COUNT) {
        __atomic_fetch_add(&ctrl->overflow, 1u, __ATOMIC_RELAXED);
        return false;
    }

    IpcRingSlot *slot = &slots[mask_index(head)];
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

    IpcRingSlot *slot = &slots[mask_index(tail)];
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

uint32_t ipc_ring_pending(const IpcRingCtrl *ctrl)
{
    const uint32_t head = __atomic_load_n(&ctrl->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&ctrl->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

uint32_t ipc_ring_free_slots(const IpcRingCtrl *ctrl)
{
    return IPC_RING_SLOT_COUNT - ipc_ring_pending(ctrl);
}
