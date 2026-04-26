/* messages_inbox.h — Bounded FIFO of recent received text messages.
 *
 * M5 Phase 2: stores the last `MESSAGES_INBOX_CAPACITY` entries in a
 * ring buffer keyed by a monotonic 32-bit sequence. Producer
 * (`bridge_task` IPC dispatcher) appends; consumer (`lvgl_task` via
 * messages_view) reads any entry by relative offset (0 = newest).
 *
 * Concurrency model:
 *   Single producer, single consumer (both on Core 1, on different
 *   FreeRTOS tasks). Producer writes the body fields of the next slot,
 *   then bumps the global `seq` with __ATOMIC_RELEASE; consumer reads
 *   `seq` with __ATOMIC_ACQUIRE before touching slot data. As long as
 *   the consumer reads each newly-published message reasonably quickly
 *   relative to the producer, no torn reads — see take_at_offset()
 *   notes below for the (acceptable) edge case where a slot gets
 *   overwritten while being read.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_MESSAGES_INBOX_H
#define MOKYA_CORE1_MESSAGES_INBOX_H

#include <stdbool.h>
#include <stdint.h>

#define MESSAGES_INBOX_TEXT_MAX  200u
#define MESSAGES_INBOX_CAPACITY    4u   /* FIFO depth — Phase 2 minimum */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t seq;                                ///< Monotonic id assigned at publish
    uint32_t from_node_id;
    uint32_t to_node_id;
    uint8_t  channel_index;
    uint16_t text_len;                           ///< Bytes used in `text`
    uint8_t  text[MESSAGES_INBOX_TEXT_MAX];      ///< UTF-8, no null terminator
} messages_inbox_entry_t;

/* Append a fresh RX_TEXT into the inbox, evicting the oldest entry if
 * the ring is full. Truncates `text` if it exceeds
 * MESSAGES_INBOX_TEXT_MAX. Bumps `seq` last so consumers polling seq
 * see the slot in fully-published state. */
void messages_inbox_publish(uint32_t from_node_id,
                            uint32_t to_node_id,
                            uint8_t  channel_index,
                            const uint8_t *text,
                            uint16_t text_len);

/* Latest sequence number ever published (= 0 before the first publish).
 * Use this in a refresh tick to detect new arrivals. */
uint32_t messages_inbox_latest_seq(void);

/* Number of entries currently held (0..MESSAGES_INBOX_CAPACITY). */
uint32_t messages_inbox_count(void);

/* Copy the entry at `offset` (0 = newest, 1 = next-newest, ...) into
 * `out`. Returns true on success; false if `offset >= count()`.
 *
 * Concurrency caveat: under heavy load the producer can overwrite a
 * slot while the consumer is mid-copy. Since this is a UI path and a
 * torn message is no worse than the user pressing UP again, we don't
 * spend cycles on a snapshot lock. The seq field is always written
 * last, so a consumer that re-checks `entry.seq` against the value it
 * expected can detect a torn read.
 */
bool messages_inbox_take_at_offset(uint32_t offset,
                                   messages_inbox_entry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_MESSAGES_INBOX_H */
