/* range_test_log.h — per-peer counters for inbound RANGE_TEST_APP
 * (PortNum 66) packets.
 *
 * Cascade FR_TAG_PACKET dispatch records each Range Test packet here;
 * range_test_view (T-2) renders the latest snapshot. v1 keeps a flat
 * 7-slot table — one row per source peer; a 8th unique peer is
 * dropped. Total hit count is also tracked at module scope.
 *
 * No mutex: producer is the cascade `bridge_task` (Core 1) and
 * consumer is the lvgl_task (also Core 1); they don't preempt each
 * other (single-core FreeRTOS, lvgl_task lower priority than bridge).
 * Reader uses a snapshot-by-copy via range_test_log_get to avoid
 * tearing on multi-byte fields.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RANGE_TEST_PEERS_MAX  7u

typedef struct {
    uint32_t node_num;        ///< MeshPacket.from
    uint32_t hits;            ///< total RANGE_TEST_APP packets observed
    uint32_t last_seq;        ///< parsed leading integer in payload, 0 if none
    int8_t   last_snr_x4;     ///< MeshPacket.rx_snr × 4 (saturated), INT8_MIN unknown
    int16_t  last_rssi;       ///< MeshPacket.rx_rssi (dBm), 0 if absent
    uint32_t last_epoch;      ///< MeshPacket.rx_time, 0 if absent
} range_test_entry_t;

void     range_test_log_record(uint32_t node_num, uint32_t seq,
                                int8_t   snr_x4, int16_t rssi,
                                uint32_t epoch);
uint32_t range_test_log_count(void);
bool     range_test_log_get(uint32_t idx, range_test_entry_t *out);
uint32_t range_test_log_change_seq(void);
uint32_t range_test_log_total_hits(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
