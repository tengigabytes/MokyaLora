/* key_inject.h — SWD-driven virtual key injection for Core 1.
 *
 * Provides a memory-mapped ring buffer that a host (J-Link Commander
 * `mem8`) can write keypress events into. A dedicated FreeRTOS task on
 * Core 1 polls the buffer and forwards events to key_event_push_inject(),
 * which feeds them through the same arbitration / queue path as physical
 * keys (the user's hand still wins via key_event_mark_hw arbitration).
 *
 * Wire format (events[] is now an array of 2-byte records as of Phase
 * 1.4; bumped magic to KEY_INJECT_MAGIC_V2 = "KEYJ"):
 *   byte[0]:
 *     bit 7      : pressed (1) or released (0)
 *     bits 6..0  : keycode (0x01..0x3F per mie/keycode.h)
 *   byte[1]:
 *     KEY_FLAG_* bitmask (mie/keycode.h MOKYA_KEY_FLAG_LONG_PRESS,
 *     MOKYA_KEY_FLAG_LONG_LONG, ...). 0 = short tap.
 *
 * Producer (host) protocol:
 *   1. Read consumer_idx (counted in EVENTS, not bytes).
 *   2. Wait until (producer_idx - consumer_idx) < KEY_INJECT_RING_EVENTS.
 *   3. Write events[(producer_idx % KEY_INJECT_RING_EVENTS) * 2 + 0] = key_byte
 *      and events[(producer_idx % KEY_INJECT_RING_EVENTS) * 2 + 1] = flags_byte.
 *   4. Bump producer_idx (with a release barrier on the host side).
 *
 * Consumer (Core 1) protocol:
 *   1. Read producer_idx (acquire).
 *   2. Drain events[consumer_idx % SIZE .. producer_idx % SIZE].
 *   3. Bump consumer_idx after each event.
 *
 * Address discovery: the buffer is a regular static — find via
 * `arm-none-eabi-nm core1_bridge.elf | grep g_key_inject_buf`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic bumped to "KEYJ" when the wire format moved from a 1-byte single
 * record per event to a 2-byte (key_byte, flags_byte) pair (Phase 1.4
 * long-press disambiguation). Hosts that observe magic == 0x4B454949 are
 * talking to a pre-Phase-1.4 firmware and must not bump producer_idx. */
#define KEY_INJECT_MAGIC_V1      0x4B454949u  /* legacy 1-byte format */
#define KEY_INJECT_MAGIC         0x4B45494Au  /* "KEYJ" — 2-byte format */
#define KEY_INJECT_RING_EVENTS   32u
#define KEY_INJECT_EVENT_SIZE    2u
#define KEY_INJECT_RING_BYTES    (KEY_INJECT_RING_EVENTS * KEY_INJECT_EVENT_SIZE)

typedef struct {
    volatile uint32_t magic;          /* set to KEY_INJECT_MAGIC by Core 1 init */
    volatile uint32_t producer_idx;   /* host writes; bumped per EVENT          */
    volatile uint32_t consumer_idx;   /* core1 writes after consuming           */
    volatile uint32_t pushed;         /* core1: total events forwarded ok       */
    volatile uint32_t rejected;       /* core1: forwards that returned non-OK   */
    volatile uint8_t  events[KEY_INJECT_RING_BYTES];
} key_inject_buf_t;

extern volatile key_inject_buf_t g_key_inject_buf;

/* Transport arbitration. Exactly one of {SWD ring, RTT down-channel}
 * actively polls at a time; the other task checks this byte at the
 * top of its loop and long-sleeps when it isn't selected. The host
 * switches mode via SWD memory_write before running a batch of key
 * inject operations and switches back when done, so the two
 * transports don't compete for the same priority band and starve
 * ime_task. Default 0 (SWD) preserves historical behaviour.          */
#define KEY_INJECT_MODE_SWD   0u
#define KEY_INJECT_MODE_RTT   1u
extern volatile uint8_t g_key_inject_mode;

/* Create and start the polling task. Must be called after FreeRTOS
 * scheduler is up. Idempotent — second call is a no-op. */
void key_inject_task_start(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
