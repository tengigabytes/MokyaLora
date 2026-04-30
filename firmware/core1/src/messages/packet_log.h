/* packet_log.h — newest-first ring of recently-seen FromRadio.packet
 * envelopes for T-4 sniffer view.
 *
 * Cascade FR_TAG_PACKET dispatcher records every packet here (regardless
 * of portnum) before running the per-portnum decoder chain, so the
 * sniffer captures TEXT, POSITION, ROUTING(ACK), TRACEROUTE,
 * NEIGHBORINFO, RANGE_TEST, ADMIN, and anything else Meshtastic emits
 * — including portnums we don't have specific decoders for.
 *
 * Ring is single-producer (cascade `bridge_task`) / single-consumer
 * (lvgl_task sniffer_view); no mutex.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACKET_LOG_CAP            16u
#define PACKET_LOG_PAYLOAD_MAX    16u   /* truncated hex preview */

typedef struct {
    uint32_t epoch;            ///< MeshPacket.rx_time, 0 if absent
    uint32_t from_node;        ///< MeshPacket.from
    uint32_t portnum;          ///< Data.portnum (0 if Data missing)
    int8_t   snr_x4;           ///< MeshPacket.rx_snr × 4 saturated, INT8_MIN unset
    int16_t  rssi;             ///< MeshPacket.rx_rssi, 0 if absent
    uint8_t  payload_len;      ///< Bytes copied into payload[] (≤ MAX)
    uint8_t  payload[PACKET_LOG_PAYLOAD_MAX];
} packet_log_entry_t;

void     packet_log_record(const packet_log_entry_t *entry);
uint32_t packet_log_count(void);          ///< 0..PACKET_LOG_CAP
uint32_t packet_log_total(void);          ///< monotonic since boot
uint32_t packet_log_change_seq(void);
/** `index` 0 = newest. Returns false if index out of range. */
bool     packet_log_get_newest(uint32_t index, packet_log_entry_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif
