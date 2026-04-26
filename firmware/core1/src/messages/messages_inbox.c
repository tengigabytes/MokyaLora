/* messages_inbox.c — see messages_inbox.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "messages_inbox.h"

#include <string.h>

static messages_inbox_entry_t s_ring[MESSAGES_INBOX_CAPACITY];

/* Index of the slot the *next* publish will occupy (write head). */
static uint32_t s_write_idx;

/* Total number of valid entries in the ring (saturates at CAPACITY). */
static uint32_t s_count;

/* Most recently assigned `seq` (0 means "no message yet" — see
 * messages_inbox_publish for why we start counting at 1). */
static uint32_t s_latest_seq;

void messages_inbox_publish(uint32_t from_node_id,
                            uint32_t to_node_id,
                            uint8_t  channel_index,
                            const uint8_t *text,
                            uint16_t text_len)
{
    if (text_len > MESSAGES_INBOX_TEXT_MAX) {
        text_len = MESSAGES_INBOX_TEXT_MAX;
    }

    messages_inbox_entry_t *slot = &s_ring[s_write_idx];

    /* Body fields first. The seq field is written last with a release
     * fence so a consumer that has already loaded `s_latest_seq` and
     * starts reading this slot only sees a fully-populated entry. */
    slot->from_node_id  = from_node_id;
    slot->to_node_id    = to_node_id;
    slot->channel_index = channel_index;
    slot->text_len      = text_len;
    if (text_len > 0 && text != NULL) {
        memcpy(slot->text, text, text_len);
    }

    uint32_t next_seq = s_latest_seq + 1u;
    __atomic_store_n(&slot->seq, next_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s_latest_seq, next_seq, __ATOMIC_RELEASE);

    s_write_idx = (s_write_idx + 1u) % MESSAGES_INBOX_CAPACITY;
    if (s_count < MESSAGES_INBOX_CAPACITY) {
        s_count++;
    }
}

uint32_t messages_inbox_latest_seq(void)
{
    return __atomic_load_n(&s_latest_seq, __ATOMIC_ACQUIRE);
}

uint32_t messages_inbox_count(void)
{
    return s_count;
}

bool messages_inbox_take_at_offset(uint32_t offset,
                                   messages_inbox_entry_t *out)
{
    if (offset >= s_count) {
        return false;
    }

    /* `s_write_idx` points at the *next* slot to write. The newest
     * entry therefore lives at (write_idx - 1) mod CAPACITY; offset N
     * older lives at (write_idx - 1 - N). */
    uint32_t newest = (s_write_idx + MESSAGES_INBOX_CAPACITY - 1u)
                      % MESSAGES_INBOX_CAPACITY;
    uint32_t idx = (newest + MESSAGES_INBOX_CAPACITY - offset)
                   % MESSAGES_INBOX_CAPACITY;
    *out = s_ring[idx];
    return true;
}
