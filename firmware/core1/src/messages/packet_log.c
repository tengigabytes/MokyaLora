/* packet_log.c — see packet_log.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "packet_log.h"

#include <string.h>

/* Ring lives in PSRAM (.psram_bss) — 16 × ~36 B ≈ 580 B which is too
 * much for the tight Core 1 SRAM budget. Single-task usage (cascade
 * write, lvgl_task read) → no cross-core coherency concern; both go
 * through the cached PSRAM alias. SWD verification reads the diag
 * globals exported by sniffer_view (regular BSS), not the ring
 * directly, so write-back caching doesn't affect tests either. */
static packet_log_entry_t s_ring[PACKET_LOG_CAP] __attribute__((section(".psram_bss")));
static uint32_t           s_count;        ///< 0..CAP (saturates)
static uint32_t           s_head;         ///< write position (mod CAP)
static uint32_t           s_total;        ///< monotonic packets seen
static uint32_t           s_change_seq;

void packet_log_record(const packet_log_entry_t *entry)
{
    if (entry == NULL) return;
    s_ring[s_head] = *entry;
    s_head = (s_head + 1u) % PACKET_LOG_CAP;
    if (s_count < PACKET_LOG_CAP) s_count++;
    s_total++;
    s_change_seq++;
}

uint32_t packet_log_count(void)      { return s_count; }
uint32_t packet_log_total(void)      { return s_total; }
uint32_t packet_log_change_seq(void) { return s_change_seq; }

bool packet_log_get_newest(uint32_t index, packet_log_entry_t *out)
{
    if (out == NULL || index >= s_count) return false;
    /* Newest is at (s_head - 1) mod CAP. Walk back `index` slots. */
    uint32_t slot = (s_head + PACKET_LOG_CAP - 1u - index) % PACKET_LOG_CAP;
    *out = s_ring[slot];
    return true;
}
