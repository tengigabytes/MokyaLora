/* waypoint_persist.h — Phase 4: persist phoneapi waypoint cache to LFS.
 *
 * Single-file bridge for the 8-slot phoneapi_waypoints[] table. Mirrors
 * the dm_persist / Phase 3 shape but simpler — only one file
 * ("/.waypoints"), single dirty bit (no per-slot tracking needed since
 * the whole table re-serialises as one record).
 *
 * Save trigger:
 *   phoneapi_waypoints_upsert / _remove sets the cache's dirty flag.
 *   30 s soft timer in this module polls phoneapi_waypoints_pop_dirty;
 *   on true it serialises the whole table → /.waypoints.
 *
 * Load trigger:
 *   waypoint_persist_load_all() at boot reads /.waypoints and calls
 *   phoneapi_waypoints_upsert() for each in-use entry. Called from
 *   bridge_task setup after c1_storage_init succeeds.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_WAYPOINT_PERSIST_H
#define MOKYA_CORE1_WAYPOINT_PERSIST_H

#include <stdbool.h>
#include <stdint.h>

#include "phoneapi_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WAYPOINT_PERSIST_MAGIC    0x31594157u   /* 'WAY1' little-endian */
#define WAYPOINT_PERSIST_VERSION  1u
#define WAYPOINT_PERSIST_PATH     "/.waypoints"

/* On-disk record. 16 B header + 8 × phoneapi_waypoint_t (~180 B each)
 * ≈ 1456 B. Fits comfortably in one 4 KB LFS sector. */
typedef struct waypoint_persist_record {
    uint32_t magic;             /* WAYPOINT_PERSIST_MAGIC */
    uint32_t version;           /* WAYPOINT_PERSIST_VERSION */
    uint32_t count;             /* Number of in_use slots in table[] */
    uint32_t reserved;          /* Pad / future use */
    phoneapi_waypoint_t table[PHONEAPI_WAYPOINTS_CAP];
} waypoint_persist_record_t;

/* Mount-time load: read /.waypoints, restore each entry into the cache.
 * Returns the number of waypoints restored (0..PHONEAPI_WAYPOINTS_CAP).
 * 0 is a normal first-boot result (no file yet). */
uint32_t waypoint_persist_load_all(void);

/* Initialise + start the 30 s flush timer. Idempotent; safe even if
 * c1_storage isn't mounted (timer skipped). */
void waypoint_persist_init(void);

/* Force-flush now. Returns true on success (file written) or if no
 * dirty state existed (no-op success). */
bool waypoint_persist_flush_now(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_WAYPOINT_PERSIST_H */
