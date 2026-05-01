/* history_persist.h — Phase 5: F-4 telemetry ring snapshot persistence.
 *
 * Saves the metrics_history 256-sample ring to /.history. Survives
 * watchdog/cold reset so the F-4 chart shows continuous trend instead
 * of resetting every reboot.
 *
 * Save period: 5 min (lower than dm/waypoint 30 s — telemetry is
 * statistical, not user-critical, and writing every 30 s would burn
 * flash for marginal benefit). 5 min × 30 s/sample = 10 fresh samples
 * per save → reasonable trade.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_HISTORY_PERSIST_H
#define MOKYA_CORE1_HISTORY_PERSIST_H

#include <stdbool.h>
#include <stdint.h>

#include "history.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_PERSIST_MAGIC    0x31534948u   /* 'HIS1' little-endian */
#define HISTORY_PERSIST_VERSION  1u
#define HISTORY_PERSIST_PATH     "/.history"

/* On-disk record. 16 B header + 256 × 6 B samples = 1552 B. Fits in
 * one 4 KB LFS sector. */
typedef struct history_persist_record {
    uint32_t magic;             /* HISTORY_PERSIST_MAGIC */
    uint32_t version;           /* HISTORY_PERSIST_VERSION */
    uint16_t head;              /* metrics_history s_head at save time */
    uint16_t count;             /* metrics_history s_count at save time */
    uint32_t reserved;
    metrics_sample_t ring[METRICS_HISTORY_LEN];
} history_persist_record_t;

/* Mount-time load: read /.history, restore the ring. Returns count
 * loaded (0 = no file or load failed). */
uint16_t history_persist_load(void);

/* Initialise + start the 5 min flush timer. Idempotent. */
void history_persist_init(void);

/* Force-flush now. Returns true on success or no-op (no dirty). */
bool history_persist_flush_now(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_HISTORY_PERSIST_H */
