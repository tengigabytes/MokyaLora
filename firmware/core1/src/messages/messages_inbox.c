/* messages_inbox.c — see messages_inbox.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "messages_inbox.h"

#include <string.h>

static messages_inbox_snapshot_t s_snap;

void messages_inbox_publish(uint32_t from_node_id,
                            uint32_t to_node_id,
                            uint8_t  channel_index,
                            const uint8_t *text,
                            uint16_t text_len)
{
    if (text_len > MESSAGES_INBOX_TEXT_MAX) {
        text_len = MESSAGES_INBOX_TEXT_MAX;
    }

    /* Write body fields first; bump seq last so a reader that polls seq
     * either sees the whole previous snapshot or the whole new one, never
     * a partial body. */
    s_snap.from_node_id  = from_node_id;
    s_snap.to_node_id    = to_node_id;
    s_snap.channel_index = channel_index;
    s_snap.text_len      = text_len;
    if (text_len > 0 && text != NULL) {
        memcpy(s_snap.text, text, text_len);
    }

    uint32_t next = s_snap.seq + 1u;
    __atomic_store_n(&s_snap.seq, next, __ATOMIC_RELEASE);
}

bool messages_inbox_take_if_new(uint32_t last_seen_seq,
                                messages_inbox_snapshot_t *out)
{
    uint32_t cur = __atomic_load_n(&s_snap.seq, __ATOMIC_ACQUIRE);
    if (cur == last_seen_seq) {
        return false;
    }
    /* Copy under the assumption the producer is done — see header for
     * why no lock is needed in the current single-producer / single-
     * consumer setup. */
    *out = s_snap;
    out->seq = cur;
    return true;
}
