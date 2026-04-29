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
#include <stddef.h>
#include "ipc_protocol.h"
#include "mokya_postmortem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Absolute placement (match linker scripts on both sides) ──────────────── */

#define IPC_SHARED_ORIGIN   0x2007A000u
#define IPC_SHARED_SIZE     0x00006000u  /* 24 KB */
#define IPC_BOOT_MAGIC      0x4D4F4B59u  /* 'MOKY' little-endian */
#define IPC_DOORBELL_NUM          0u         /* RP2350 doorbell for ring push notification */
#define IPC_FLASH_DOORBELL        1u         /* Core 0 → Core 1 park request (legacy name) */
#define IPC_FLASH_DOORBELL_C1     1u         /* Alias: doorbell ringing C1's park handler */
#define IPC_FLASH_DOORBELL_C0     2u         /* Core 1 → Core 0 park request (Phase 1.6) */

/* flash_lock / flash_lock_c0 protocol (shared SRAM):
 *   0 = normal operation
 *   1 = writer core has requested parker to park
 *   2 = parker has parked, writer may proceed
 * Writer clears back to 0 after flash op completes, then __SEV().
 *
 * Two independent locks, one per direction, so a simultaneous attempt
 * from both cores (rare but possible) deadlocks into explicit mutual
 * waiting rather than racing on a single state word:
 *   flash_lock      — used when Core 0 writes flash and Core 1 parks.
 *   flash_lock_c0   — used when Core 1 writes flash and Core 0 parks.  */
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
    volatile uint32_t flash_lock;       ///< C0-writes / C1-parks handshake (IPC_FLASH_LOCK_*)
    volatile uint32_t flash_lock_c0;    ///< C1-writes / C0-parks handshake (Phase 1.6)

    /* Watchdog liveness chain (post-B2 PR). Core 0 increments c0_heartbeat
     * from vApplicationIdleHook at FreeRTOS tick rate; Core 1's wd_task
     * polls c0_heartbeat every 200 ms and kicks the HW watchdog. If
     * c0_heartbeat stalls for ≥ 4 s, wd_task stops kicking and the HW
     * watchdog (3 s timeout) resets the chip.
     *
     * wd_pause is a nesting counter — non-zero suppresses silence
     * detection but kicks continue. Used by long blocking ops:
     * flash_range_erase/program (P2-11 park ≥ 500 ms), Power::reboot()
     * delay (500 ms before watchdog_reboot()), LittleFS format, etc.
     * mokya_watchdog_pause()/resume() in this header are the public
     * helpers; they atomic-fetch-add with __ATOMIC_RELAXED. */
    volatile uint32_t c0_heartbeat;     ///< Core 0 monotonic liveness counter
    volatile uint32_t wd_pause;         ///< Pause counter — non-zero = no silence detect

    /* Control blocks — 32 B each, separated from slots for cache-friendliness */
    uint32_t          _pad_to_0x20[1];
    IpcRingCtrl       c0_to_c1_ctrl;      ///< Core 0 → Core 1 DATA ring (producer = C0)
    IpcRingCtrl       c0_log_to_c1_ctrl;  ///< Core 0 → Core 1 LOG ring (producer = C0)
    IpcRingCtrl       c1_to_c0_ctrl;      ///< Core 1 → Core 0 CMD ring (producer = C1)

    /* Slot arrays */
    IpcRingSlot       c0_to_c1_slots[IPC_RING_SLOT_COUNT];            ///< 32 × 264 B — protobuf data
    IpcRingSlot       c0_log_to_c1_slots[IPC_LOG_RING_SLOT_COUNT];    ///< 16 × 264 B — log lines
    IpcRingSlot       c1_to_c0_slots[IPC_RING_SLOT_COUNT];            ///< 32 × 264 B — host commands

    /* GPS NMEA double-buffer (M3.5 — Core 1 writer, Core 0 reader) */
    IpcGpsBuf         gps_buf;

    /* _tail_pad_pre: zeroed by ipc_shared_init, fills space up to the
     * postmortem slots. The postmortem block now sits at absolute
     * address 0x2007FB00..0x2007FDFF — 768 bytes total since the
     * struct grew to 384 B per slot (was 256 B; bumped again to carry
     * a 64-word stack snapshot, sized to clear the 18-word FP register
     * save area when EXC_RETURN.FType=0 plus enough caller frames for
     * a back-trace). Region still ends BEFORE the ime_view_debug
     * snapshot at 0x2007FE00..0x2007FFBF (firmware/core1/src/ui/
     * ime_view.c) and BEFORE the bridge breadcrumbs at the last 64 B
     * (0x2007FFC0..0x2007FFFF, registered in firmware-architecture
     * §9.3). */
    uint8_t           _tail_pad_pre[IPC_SHARED_SIZE
                                - 28                                    /* magic + ready/lock + flash_lock_c0 + heartbeat + wd_pause */
                                - 4                                     /* _pad_to_0x20[1] */
                                - 3 * sizeof(IpcRingCtrl)               /* three ctrl blocks */
                                - 2 * IPC_RING_SLOT_COUNT * sizeof(IpcRingSlot)  /* data + cmd */
                                - IPC_LOG_RING_SLOT_COUNT * sizeof(IpcRingSlot)  /* log */
                                - sizeof(IpcGpsBuf)
                                - 2 * sizeof(mokya_postmortem_t)
                                - 512u];                                /* _tail_pad_post = ime_view_debug (448 B) + breadcrumbs (64 B) */

    /* Postmortem slots — survive watchdog/SYSRESETREQ; cleared only by
     * POR/BOR. ipc_shared_init() skips this 512-byte window. */
    mokya_postmortem_t postmortem_c0;
    mokya_postmortem_t postmortem_c1;

    /* _tail_pad_post: zeroed by ipc_shared_init. Absolute layout:
     *   0x2007FE00..0x2007FFBF — ime_view_debug_t snapshot (448 B,
     *     written by Core 1's ime_view at runtime).
     *   0x2007FFC0..0x2007FFFF — registered breadcrumb slots
     *     (firmware-architecture §9.3): RX/TX byte counters + USB
     *     state. Both regions are intentionally cleared on boot. */
    uint8_t           _tail_pad_post[512];
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

/* ── Watchdog pause helpers (post-B2 PR) ─────────────────────────────── *
 *
 * Nesting counter — each pause() must be matched with exactly one
 * resume(). While the counter is non-zero, Core 1's wd_task continues
 * to kick the HW watchdog but skips the c0_heartbeat silence check.
 * That is precisely what long blocking ops need: they can stall Core 0
 * for ≥ 4 s (LittleFS format, P2-11 park, Power::reboot delay) without
 * triggering a false-positive watchdog reset, but the chip is still
 * protected against a real Core 1 hang because the kicks themselves
 * stop if wd_task itself hangs.
 *
 * Atomic RELAXED is sufficient — wd_task only reads the value to
 * decide whether to skip silence-detect; it does not order any other
 * memory access against it. Both cores can pause/resume freely.
 */
static inline void mokya_watchdog_pause(void)
{
    __atomic_fetch_add(&g_ipc_shared.wd_pause, 1u, __ATOMIC_RELAXED);
}

static inline void mokya_watchdog_resume(void)
{
    __atomic_fetch_sub(&g_ipc_shared.wd_pause, 1u, __ATOMIC_RELAXED);
}

#ifdef __cplusplus
}
#endif
