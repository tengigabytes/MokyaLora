/* range_test_log.c — see range_test_log.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "range_test_log.h"

#include <string.h>

static range_test_entry_t s_entries[RANGE_TEST_PEERS_MAX];
static uint32_t           s_count;          ///< 0..RANGE_TEST_PEERS_MAX
static uint32_t           s_change_seq;
static uint32_t           s_total_hits;

void range_test_log_record(uint32_t node_num, uint32_t seq,
                            int8_t   snr_x4, int16_t rssi,
                            uint32_t epoch)
{
    if (node_num == 0u) return;

    /* Existing peer? Update in place. */
    for (uint32_t i = 0; i < s_count; ++i) {
        if (s_entries[i].node_num == node_num) {
            s_entries[i].hits++;
            s_entries[i].last_seq     = seq;
            s_entries[i].last_snr_x4  = snr_x4;
            s_entries[i].last_rssi    = rssi;
            s_entries[i].last_epoch   = epoch;
            s_total_hits++;
            s_change_seq++;
            return;
        }
    }

    /* New peer — append if room. v1 caps at 7; the 8th unique peer is
     * dropped (T-2 row count is the limiting factor, not ring memory). */
    if (s_count < RANGE_TEST_PEERS_MAX) {
        s_entries[s_count].node_num    = node_num;
        s_entries[s_count].hits        = 1u;
        s_entries[s_count].last_seq    = seq;
        s_entries[s_count].last_snr_x4 = snr_x4;
        s_entries[s_count].last_rssi   = rssi;
        s_entries[s_count].last_epoch  = epoch;
        s_count++;
        s_total_hits++;
        s_change_seq++;
    }
}

uint32_t range_test_log_count(void)        { return s_count; }
uint32_t range_test_log_change_seq(void)   { return s_change_seq; }
uint32_t range_test_log_total_hits(void)   { return s_total_hits; }

bool range_test_log_get(uint32_t idx, range_test_entry_t *out)
{
    if (idx >= s_count || out == NULL) return false;
    *out = s_entries[idx];
    return true;
}
