/* messages_inbox.h — Last-received text message snapshot for LVGL.
 *
 * M5 Phase 1 minimal slice: store only the most recent IPC_MSG_RX_TEXT
 * payload from Core 0, plus a monotonic sequence number so the consumer
 * (messages_view) can detect a change without locking. A multi-message
 * scrollback is M5 Phase 2 work.
 *
 * Concurrency model:
 *   Producer = Core 1 bridge_task (single producer, IPC dispatcher).
 *   Consumer = lvgl_task (single consumer, view refresh). Both run on
 *   Core 1, never preempted while writing each individual atomic field
 *   (bridge_task is the higher-priority of the two when an IPC slot
 *   arrives), so a sequence-number gate is sufficient — no mutex.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_MESSAGES_INBOX_H
#define MOKYA_CORE1_MESSAGES_INBOX_H

#include <stdbool.h>
#include <stdint.h>

#define MESSAGES_INBOX_TEXT_MAX 200u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t seq;                                ///< Monotonic — bumped on each new message
    uint32_t from_node_id;
    uint32_t to_node_id;
    uint8_t  channel_index;
    uint16_t text_len;                           ///< Bytes used in `text`
    uint8_t  text[MESSAGES_INBOX_TEXT_MAX];      ///< UTF-8, no null terminator
} messages_inbox_snapshot_t;

/* Producer: copy a fresh RX_TEXT payload into the inbox. Truncates `text`
 * if it exceeds MESSAGES_INBOX_TEXT_MAX. Bumps `seq` last so a reader
 * polling `seq` sees the full body atomically (release ordering). */
void messages_inbox_publish(uint32_t from_node_id,
                            uint32_t to_node_id,
                            uint8_t  channel_index,
                            const uint8_t *text,
                            uint16_t text_len);

/* Consumer: copy the latest snapshot into `out` only if its seq differs
 * from `last_seen_seq`. Returns true when a new snapshot was copied; the
 * caller should update its `last_seen_seq = out->seq`. */
bool messages_inbox_take_if_new(uint32_t last_seen_seq,
                                messages_inbox_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_MESSAGES_INBOX_H */
