/* nodes_db.c — see nodes_db.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nodes_db.h"

#include <string.h>

static nodes_db_entry_t s_table[NODES_DB_CAPACITY];
static uint32_t         s_change_seq;
static uint32_t         s_touch_clock;   /* monotonic, bumped per upsert */

static int find_by_id(uint32_t node_id)
{
    for (uint32_t i = 0; i < NODES_DB_CAPACITY; ++i) {
        if (s_table[i].in_use && s_table[i].node_id == node_id) {
            return (int)i;
        }
    }
    return -1;
}

static int find_free_or_oldest(void)
{
    int oldest_idx = 0;
    uint32_t oldest_touch = UINT32_MAX;
    for (uint32_t i = 0; i < NODES_DB_CAPACITY; ++i) {
        if (!s_table[i].in_use) return (int)i;
        if (s_table[i].last_touch_seq < oldest_touch) {
            oldest_touch = s_table[i].last_touch_seq;
            oldest_idx = (int)i;
        }
    }
    return oldest_idx;
}

void nodes_db_upsert(uint32_t node_id,
                     int16_t  rssi,
                     int8_t   snr_x4,
                     uint8_t  hops_away,
                     int32_t  lat_e7,
                     int32_t  lon_e7,
                     uint16_t battery_mv,
                     const uint8_t *alias,
                     uint8_t alias_len)
{
    if (node_id == 0u) return;       /* sentinel for "empty slot" */
    if (alias_len > NODES_DB_ALIAS_MAX) alias_len = NODES_DB_ALIAS_MAX;

    int idx = find_by_id(node_id);
    if (idx < 0) idx = find_free_or_oldest();

    nodes_db_entry_t *e = &s_table[idx];
    e->in_use         = true;
    e->node_id        = node_id;
    e->rssi           = rssi;
    e->snr_x4         = snr_x4;
    e->hops_away      = hops_away;
    e->lat_e7         = lat_e7;
    e->lon_e7         = lon_e7;
    e->battery_mv     = battery_mv;
    e->alias_len      = alias_len;
    if (alias_len > 0 && alias != NULL) {
        memcpy(e->alias, alias, alias_len);
    }
    e->last_touch_seq = ++s_touch_clock;

    __atomic_store_n(&s_change_seq, s_change_seq + 1u, __ATOMIC_RELEASE);
}

uint32_t nodes_db_change_seq(void)
{
    return __atomic_load_n(&s_change_seq, __ATOMIC_ACQUIRE);
}

uint32_t nodes_db_count(void)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < NODES_DB_CAPACITY; ++i) {
        if (s_table[i].in_use) c++;
    }
    return c;
}

bool nodes_db_take_at(uint32_t index, nodes_db_entry_t *out)
{
    if (!out) return false;

    /* Order by most-recently-touched first. Find the entry whose rank
     * (from the top) equals `index`. */
    uint32_t want_rank = index;
    uint32_t selected_touch = 0;
    int      selected_idx   = -1;

    /* Iteratively find the (index+1)-th highest touch seq. Capacity is
     * 16 so this is O(N²) in the absolute worst case but trivially
     * fast — keeps the implementation transparent without sorting. */
    uint32_t prev_high = UINT32_MAX;
    for (uint32_t pass = 0; pass <= want_rank; ++pass) {
        selected_idx = -1;
        selected_touch = 0;
        for (uint32_t i = 0; i < NODES_DB_CAPACITY; ++i) {
            if (!s_table[i].in_use) continue;
            uint32_t t = s_table[i].last_touch_seq;
            if (t < prev_high && t >= selected_touch) {
                selected_touch = t;
                selected_idx   = (int)i;
            }
        }
        if (selected_idx < 0) return false;   /* fewer than index+1 entries */
        prev_high = selected_touch;
    }

    *out = s_table[selected_idx];
    return true;
}
