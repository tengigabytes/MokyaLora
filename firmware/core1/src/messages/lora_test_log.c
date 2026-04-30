/* lora_test_log.c — see lora_test_log.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lora_test_log.h"

#include <string.h>

/* state in PSRAM — single-task usage, no SWD-coherency concern (host
 * test reads the SWD diag globals exported by lora_test_view, not
 * this struct directly). */
static lora_test_state_t s_state __attribute__((section(".psram_bss")));
static bool      s_state_inited;
static uint32_t  s_change_seq;

static void ensure_inited(void)
{
    /* PSRAM is BSS-zeroed too, but we want INT8_MIN sentinel for
     * last_rx_snr_x4. Lazy-init on first use. */
    if (!s_state_inited) {
        s_state.last_rx_snr_x4 = INT8_MIN;
        s_state_inited = true;
    }
}

void lora_test_log_record_rx(int8_t snr_x4, int16_t rssi, uint32_t epoch)
{
    ensure_inited();
    s_state.rx_count++;
    s_state.last_rx_snr_x4 = snr_x4;
    s_state.last_rx_rssi   = rssi;
    s_state.last_rx_epoch  = epoch;
    s_change_seq++;
}

void lora_test_log_record_queue_status(uint32_t free, uint32_t maxlen,
                                        uint32_t mesh_packet_id, int32_t res)
{
    ensure_inited();
    s_state.tx_status_count++;
    s_state.queue_free      = free;
    s_state.queue_maxlen    = maxlen;
    s_state.last_queue_pid  = mesh_packet_id;
    s_state.last_queue_res  = res;
    s_change_seq++;
}

void lora_test_log_record_ack(uint32_t pid, uint8_t err)
{
    ensure_inited();
    if (err == 0u) s_state.ack_count++;
    else           s_state.nack_count++;
    s_state.last_ack_pid = pid;
    s_state.last_ack_err = err;
    s_change_seq++;
}

const lora_test_state_t *lora_test_log_get(void) {
    ensure_inited();
    return &s_state;
}
uint32_t                 lora_test_log_change_seq(void) { return s_change_seq; }
