/* key_inject.h — SWD-driven virtual key injection for Core 1.
 *
 * Provides a memory-mapped ring buffer that a host (J-Link Commander
 * `mem8`) can write keypress events into. A dedicated FreeRTOS task on
 * Core 1 polls the buffer and forwards events to key_event_push_inject(),
 * which feeds them through the same arbitration / queue path as physical
 * keys (the user's hand still wins via key_event_mark_hw arbitration).
 *
 * Wire format (events[] byte):
 *   bit 7 : pressed (1) or released (0)
 *   bits 6..0 : keycode (0x01..0x3F per mie/keycode.h)
 *
 * Producer (host) protocol:
 *   1. Read consumer_idx.
 *   2. Wait until (producer_idx - consumer_idx) < KEY_INJECT_RING_BYTES.
 *   3. Write events[producer_idx % KEY_INJECT_RING_BYTES] = byte.
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

#define KEY_INJECT_MAGIC         0x4B454949u  /* "KEYI" little-endian */
#define KEY_INJECT_RING_BYTES    64u

typedef struct {
    volatile uint32_t magic;          /* set to KEY_INJECT_MAGIC by Core 1 init */
    volatile uint32_t producer_idx;   /* host writes; bumped after writing      */
    volatile uint32_t consumer_idx;   /* core1 writes after consuming           */
    volatile uint32_t pushed;         /* core1: total events forwarded ok       */
    volatile uint32_t rejected;       /* core1: forwards that returned non-OK   */
    volatile uint8_t  events[KEY_INJECT_RING_BYTES];
} key_inject_buf_t;

extern volatile key_inject_buf_t g_key_inject_buf;

/* Create and start the polling task. Must be called after FreeRTOS
 * scheduler is up. Idempotent — second call is a no-op. */
void key_inject_task_start(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
