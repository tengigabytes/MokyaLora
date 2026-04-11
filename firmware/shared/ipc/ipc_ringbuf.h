/**
 * ipc_ringbuf.h — SPSC ring buffer API for the shared-SRAM IPC transport
 * SPDX-License-Identifier: MIT
 *
 * Single-producer / single-consumer ring buffer that carries IpcMsgHeader +
 * payload slots between Core 0 and Core 1. The same source file is built
 * into both cores; there is no core-specific code path.
 *
 * Usage:
 *   Producer (on its own core) calls ipc_ring_push(...); consumer on the
 *   other core calls ipc_ring_pop(...). Each ring has exactly one producer
 *   and one consumer, so head and tail can be updated with plain
 *   acquire/release atomics without a CAS loop.
 *
 * Memory order (Cortex-M33):
 *   Producer: write slot data -> __ATOMIC_RELEASE store on head
 *   Consumer: __ATOMIC_ACQUIRE load on head -> read slot data ->
 *             __ATOMIC_RELEASE store on tail
 *   GCC emits DMB ISH around these accesses, which is sufficient for normal
 *   SRAM-backed inter-core visibility on RP2350.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ipc_protocol.h"
#include "ipc_shared_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Push a message into the ring.
 *
 * @param ctrl          Ring control block (head/tail/overflow).
 * @param slots         Slot array of length IPC_RING_SLOT_COUNT.
 * @param msg_id        IpcMsgId value copied into the slot header.
 * @param seq           Rolling sequence number copied into the slot header.
 * @param payload       Pointer to payload bytes (may be NULL if payload_len == 0).
 * @param payload_len   Number of payload bytes (must be <= IPC_MSG_PAYLOAD_MAX).
 * @return true on success, false if ring is full or payload_len too large.
 *
 * On failure the overflow counter is incremented so the producer can later
 * observe back-pressure (IpcSerialStream uses this to decide when to
 * busy-wait vs drop).
 */
bool ipc_ring_push(IpcRingCtrl *ctrl,
                   IpcRingSlot *slots,
                   uint8_t      msg_id,
                   uint8_t      seq,
                   const void  *payload,
                   uint16_t     payload_len);

/**
 * Pop a message from the ring.
 *
 * @param ctrl          Ring control block.
 * @param slots         Slot array of length IPC_RING_SLOT_COUNT.
 * @param header_out    Receives the slot header (must not be NULL).
 * @param payload_out   Receives up to max_payload bytes (may be NULL if max_payload == 0).
 * @param max_payload   Size of payload_out buffer.
 * @return true if a message was popped, false if ring was empty.
 *
 * If payload_len > max_payload the message is still popped and the payload
 * is truncated; the caller can inspect header_out->payload_len to detect
 * truncation.
 */
bool ipc_ring_pop(IpcRingCtrl *ctrl,
                  IpcRingSlot *slots,
                  IpcMsgHeader *header_out,
                  void         *payload_out,
                  size_t        max_payload);

/** Number of slots currently pending (0..IPC_RING_SLOT_COUNT). */
uint32_t ipc_ring_pending(const IpcRingCtrl *ctrl);

/** Number of free slots (0..IPC_RING_SLOT_COUNT). */
uint32_t ipc_ring_free_slots(const IpcRingCtrl *ctrl);

/**
 * Zero-initialise the shared SRAM layout. Called once by Core 0 during
 * setup (before the bridge task on Core 1 starts reading). Writes
 * IPC_BOOT_MAGIC last so Core 1 can spin on it to detect "Core 0 has
 * initialised the shared region".
 */
void ipc_shared_init(void);

#ifdef __cplusplus
}
#endif
