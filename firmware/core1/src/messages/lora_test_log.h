/* lora_test_log.h — passive LoRa health metrics.
 *
 * Counters fed by cascade FR_TAG_PACKET / FR_TAG_QUEUE_STATUS / Routing
 * (ACK) hooks; rendered by T-5 lora_test_view as a diagnostic dashboard.
 *
 * v1 is passive — surfaces what cascade already produces. Active
 * loopback (TX self → RX self) + SX1262 register-level read are v2
 * (need new IPC commands + Meshtastic submodule patch).
 *
 * Single-task usage (cascade write, lvgl_task read on Core 1) so no
 * mutex needed. State struct ~40 B in regular .bss.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rx_count;        ///< total inbound MeshPackets observed
    uint32_t tx_status_count; ///< QueueStatus events received
    uint32_t ack_count;       ///< Routing(ACK) with err=0 (delivered)
    uint32_t nack_count;      ///< Routing(ACK) with err!=0 (failed)
    uint32_t queue_free;      ///< latest QueueStatus.free
    uint32_t queue_maxlen;    ///< latest QueueStatus.maxlen
    uint32_t last_ack_pid;
    uint8_t  last_ack_err;
    int8_t   last_rx_snr_x4;  ///< INT8_MIN if no RX yet
    int16_t  last_rx_rssi;
    uint32_t last_rx_epoch;
    uint32_t last_queue_pid;
    int32_t  last_queue_res;
} lora_test_state_t;

void lora_test_log_record_rx(int8_t snr_x4, int16_t rssi, uint32_t epoch);
void lora_test_log_record_queue_status(uint32_t free, uint32_t maxlen,
                                        uint32_t mesh_packet_id, int32_t res);
void lora_test_log_record_ack(uint32_t pid, uint8_t err);

const lora_test_state_t *lora_test_log_get(void);
uint32_t                 lora_test_log_change_seq(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
