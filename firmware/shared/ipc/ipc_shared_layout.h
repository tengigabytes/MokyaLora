/**
 * ipc_shared_layout.h — MokyaLora shared SRAM layout for Phase 2 IPC
 * SPDX-License-Identifier: MIT
 *
 * Defines the single POD that lives at the top of SRAM and is visible to
 * both Core 0 (Meshtastic, GPL-3.0) and Core 1 (UI/bridge, Apache-2.0).
 * This file is MIT so it can be included from both cores without license
 * contamination — it is the ONLY shared layout header.
 *
 * Placement:
 *   Absolute address 0x2007_A000 .. 0x2007_FFFF  (24 KB)
 *   Both cores' linker scripts must define a .shared_ipc NOLOAD section at
 *   this fixed address; nm of both ELFs must report matching
 *   `g_ipc_shared` symbol addresses (verified post-build).
 *
 *   Why 24 KB and not 32 KB: the Arduino-Pico linker script gives Core 0 a
 *   .heap section that grows up to ORIGIN(RAM)+LENGTH(RAM) = 0x20080000,
 *   so the RAM region must be shrunk by a matching amount. 24 KB is
 *   carved off the top of the 512 KB main SRAM, leaving the heap to grow
 *   to 0x2007A000. SCRATCH_X / SCRATCH_Y (0x20080000..0x20082000) are
 *   untouched — they still host Arduino-Pico's Core 0 task stacks.
 *
 *   Debug breadcrumbs live in the LAST 64 B of this 24 KB region
 *   (0x2007FFC0..0x2007FFFF) inside `_tail_pad`, which both cores'
 *   linker scripts reserve as NOLOAD and ring traffic never touches.
 *   The 8 KB below (0x20078000..0x2007A000) is part of Core 0's
 *   Arduino-Pico heap region — do NOT place long-lived data there;
 *   M1.0b / M1.1-A legacy sentinel at 0x20078000 was only reliable
 *   during early boot before heap pressure built (see phase2-log P2-4).
 *
 * Transport model:
 *   Two SPSC rings carry message-level traffic (IpcMsgHeader + payload).
 *   Phase 2 M1 uses IPC_MSG_SERIAL_BYTES slots to tunnel Meshtastic's raw
 *   USB CDC byte stream; Phase 2 M4+ reuses the same rings for structured
 *   messages (RX_TEXT, NODE_UPDATE, ...).
 *
 *   Control blocks are placed on 32-byte boundaries away from the payload
 *   slots so producer/consumer never touch the same cache-line-sized region
 *   (RP2350 has no data cache but this keeps us honest for M2+ when PSRAM
 *   caching is enabled).
 *
 * No forward-looking abstractions: only the M1 byte-bridge is wired up.
 * GPS double-buffer is reserved (declared) but not populated until M4.
 */

#pragma once
#include <stdint.h>
#include "ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Absolute placement (match linker scripts on both sides) ──────────────── */

#define IPC_SHARED_ORIGIN   0x2007A000u
#define IPC_SHARED_SIZE     0x00006000u  /* 24 KB */
#define IPC_BOOT_MAGIC      0x4D4F4B59u  /* 'MOKY' little-endian */
#define IPC_DOORBELL_NUM    0u             /* RP2350 doorbell for ring push notification */
#define IPC_FLASH_DOORBELL  1u             /* RP2350 doorbell for flash-park request */

/* flash_lock protocol (shared SRAM):
 *   0 = normal operation
 *   1 = Core 0 requests Core 1 to park (set by Core 0)
 *   2 = Core 1 has parked, Core 0 may proceed (set by Core 1 ISR)
 * Core 0 clears back to 0 after flash write completes, then __SEV(). */
#define IPC_FLASH_LOCK_IDLE     0u
#define IPC_FLASH_LOCK_REQUEST  1u
#define IPC_FLASH_LOCK_PARKED   2u

/* ── Per-slot layout ───────────────────────────────────────────────────────
 *
 * Each slot holds one IpcMsgHeader (4 B) + up to IPC_MSG_PAYLOAD_MAX bytes
 * of payload. The slot is padded to IPC_RING_SLOT_STRIDE so slots stay
 * 8-byte aligned and the arithmetic is trivially vectorisable.
 */
#define IPC_RING_SLOT_STRIDE   264u   /* 4 header + 256 payload + 4 padding */

typedef struct {
    IpcMsgHeader header;                      ///< msg_id, seq, payload_len
    uint8_t      payload[IPC_MSG_PAYLOAD_MAX]; ///< up to 256 bytes
    uint8_t      _pad[IPC_RING_SLOT_STRIDE - sizeof(IpcMsgHeader) - IPC_MSG_PAYLOAD_MAX];
} IpcRingSlot;

/* ── Ring control block ────────────────────────────────────────────────────
 *
 * SPSC invariant:
 *   producer owns  head (write-only)  + reads tail
 *   consumer owns  tail (write-only)  + reads head
 * All indices are monotonically increasing uint32; ring depth is folded in
 * via modulo IPC_RING_SLOT_COUNT at access time. This lets us distinguish
 * "full" (head - tail == slot_count) from "empty" (head == tail) without a
 * sentinel slot.
 */
typedef struct {
    volatile uint32_t head;      ///< Producer writes; consumer reads
    volatile uint32_t tail;      ///< Consumer writes; producer reads
    volatile uint32_t overflow;  ///< Monotonic count of push attempts while full (debug)
    uint32_t          _pad[5];   ///< Pad control block to 32 B
} IpcRingCtrl;

/* ── Full shared SRAM layout ───────────────────────────────────────────────
 *
 * Offsets are enforced via _Static_assert below. Any edit must keep them
 * identical across cores — post-build nm check catches drift.
 */
typedef struct {
    /* Handshake word — first 16 B */
    volatile uint32_t boot_magic;   ///< IPC_BOOT_MAGIC once Core 0 inits
    volatile uint32_t c0_ready;     ///< Core 0 published setup-complete
    volatile uint32_t c1_ready;     ///< Core 1 published bridge-task-running
    volatile uint32_t flash_lock;   ///< Flash-park handshake (IPC_FLASH_LOCK_*)

    /* Control blocks — 32 B each, separated from slots for cache-friendliness */
    uint32_t          _pad_to_0x20[4];
    IpcRingCtrl       c0_to_c1_ctrl;      ///< Core 0 → Core 1 DATA ring (producer = C0)
    IpcRingCtrl       c0_log_to_c1_ctrl;  ///< Core 0 → Core 1 LOG ring (producer = C0)
    IpcRingCtrl       c1_to_c0_ctrl;      ///< Core 1 → Core 0 CMD ring (producer = C1)

    /* Slot arrays */
    IpcRingSlot       c0_to_c1_slots[IPC_RING_SLOT_COUNT];            ///< 32 × 264 B — protobuf data
    IpcRingSlot       c0_log_to_c1_slots[IPC_LOG_RING_SLOT_COUNT];    ///< 16 × 264 B — log lines
    IpcRingSlot       c1_to_c0_slots[IPC_RING_SLOT_COUNT];            ///< 32 × 264 B — host commands

    /* Reserved for Phase 2 M4 (GPS bridge) */
    uint8_t           gps_buf[260];

    /* Fill to 24 KB; compile-time checked below */
    uint8_t           _tail_pad[IPC_SHARED_SIZE
                                - 16                                    /* magic + ready words */
                                - 16                                    /* _pad_to_0x20 */
                                - 3 * sizeof(IpcRingCtrl)               /* three ctrl blocks */
                                - 2 * IPC_RING_SLOT_COUNT * sizeof(IpcRingSlot)  /* data + cmd */
                                - IPC_LOG_RING_SLOT_COUNT * sizeof(IpcRingSlot)  /* log */
                                - 260];
} IpcSharedSram;

_Static_assert(sizeof(IpcRingSlot)  == IPC_RING_SLOT_STRIDE,      "IpcRingSlot must equal stride");
_Static_assert(sizeof(IpcRingCtrl)  == 32u,                       "IpcRingCtrl must be 32 B");
_Static_assert(sizeof(IpcSharedSram) == IPC_SHARED_SIZE,          "IpcSharedSram must fill exactly 24 KB");

/* ── The single shared-SRAM instance ───────────────────────────────────────
 *
 * Defined in ipc_ringbuf.c (compiled into both cores). The linker places the
 * .shared_ipc section at IPC_SHARED_ORIGIN on both sides.
 */
extern IpcSharedSram g_ipc_shared;

#ifdef __cplusplus
}
#endif
