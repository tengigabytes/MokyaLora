/* metrics/history.h — F-4 trend ring buffer (T2.6).
 *
 * 256-entry ring sampled every 30 s. Captures battery SoC, the most
 * recently observed cascade RX SNR, and self's air_util_tx_pct as
 * surfaced by the cascade FR_TAG_NODE_INFO DeviceMetrics decoder
 * (phoneapi_node_t.air_util_tx_pct, populated by self's own NodeInfo
 * broadcasts).
 *
 * Storage: ~1.5 KB SRAM .bss. Not persisted across reboot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define METRICS_HISTORY_LEN          256u   /* points kept             */
#define METRICS_HISTORY_PERIOD_SECS   30u   /* seconds per sample      */

/* Sentinel for "no data captured for this slot/field". */
#define METRICS_HISTORY_NONE         INT16_MIN

typedef struct {
    int16_t soc_pct;          /* 0..100, METRICS_HISTORY_NONE = no data */
    int16_t last_rx_snr_x10;  /* signed dB×10; NONE if no RX since boot */
    int16_t air_tx_pct_x10;   /* self's air_util_tx_pct ×10 (0..1000);
                               * NONE if self's NodeInfo with metrics
                               * not yet observed via cascade. */
} metrics_sample_t;

/* Start the ring + soft timer. Safe to call once after FreeRTOS scheduler
 * is running; no-op on subsequent calls. */
void metrics_history_init(void);

/* Number of valid samples currently in the ring (0..METRICS_HISTORY_LEN). */
uint16_t metrics_history_count(void);

/* Read one sample, indexed from the newest (idx_from_newest=0) backwards.
 * Returns false if the slot is beyond the populated range. */
bool metrics_history_get(uint16_t idx_from_newest, metrics_sample_t *out);

/* Cascade RX SNR notification — call from phoneapi_session FR_TAG_PACKET
 * dispatch with the envelope rx_snr_x4 value (int16, ×4 dB). Cheap; just
 * stores into a global the next sampler tick will pick up. */
void metrics_history_note_rx_snr_x4(int16_t snr_x4);

/* Bump the change-seq monotonically when a new sample is appended. UI
 * polls this to gate chart redraws (matches phoneapi_cache_change_seq()). */
uint32_t metrics_history_change_seq(void);

/* ── Phase 5 persistence bridge (history_persist.c) ──────────────── */

/* Snapshot the entire ring + counters into caller-provided buffers.
 * Caller must size buf for at least METRICS_HISTORY_LEN samples.
 * Used by history_persist for its on-disk save path. */
void metrics_history_snapshot(metrics_sample_t *buf,
                              uint16_t *out_head,
                              uint16_t *out_count);

/* Bulk restore — used at boot after history_persist loads /.history.
 * Replaces the in-memory ring with the supplied data. count is clamped
 * to METRICS_HISTORY_LEN; head is taken modulo. Resets change_seq. */
void metrics_history_restore(const metrics_sample_t *buf,
                             uint16_t head,
                             uint16_t count);

/* Pop the dirty bit (true if any new sample arrived since the last
 * call). Single-bit, since the file format is the whole ring — no
 * benefit from per-slot tracking. */
bool     metrics_history_pop_dirty(void);

/* T1 — drain SWD-driven test triggers (indexed read, ring-fill).
 * Cheap; bridge_task calls once per loop iter. */
void     metrics_history_poll_swd_triggers(void);
