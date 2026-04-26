/* nodes_db.h — Core 1 mirror of mesh peers reported by Core 0.
 *
 * Holds up to NODES_DB_CAPACITY entries. Lookups by node_id (linear
 * scan — N is small enough that a hash isn't worth its weight). On
 * upsert, an existing entry is updated in place; new entries take the
 * least-recently-touched slot when the table is full.
 *
 * Concurrency model:
 *   Producer = bridge_task IPC dispatcher (single).
 *   Consumer = lvgl_task via nodes_view_refresh (single).
 *   change_seq is bumped with __ATOMIC_RELEASE on every upsert; the
 *   view polls it with __ATOMIC_ACQUIRE and skips work otherwise.
 *
 * Concurrency caveat: under heavy update bursts a consumer reading an
 * entry could observe a torn state mid-upsert. Acceptable for a UI
 * path — worst case the user sees one out-of-date frame, the next
 * tick paints the consistent value.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_NODES_DB_H
#define MOKYA_CORE1_NODES_DB_H

#include <stdbool.h>
#include <stdint.h>

#define NODES_DB_CAPACITY   8u
#define NODES_DB_ALIAS_MAX 16u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     in_use;
    uint32_t node_id;          ///< Meshtastic node id; 0 means slot empty
    int16_t  rssi;             ///< INT16_MIN if unknown
    int8_t   snr_x4;           ///< INT8_MIN if unknown
    uint8_t  hops_away;        ///< 0xFF if unknown
    int32_t  lat_e7;           ///< INT32_MIN if unknown
    int32_t  lon_e7;           ///< INT32_MIN if unknown
    uint16_t battery_mv;       ///< 0 if unknown
    uint8_t  alias_len;        ///< 0..NODES_DB_ALIAS_MAX
    uint8_t  alias[NODES_DB_ALIAS_MAX]; ///< short_name / long_name prefix; no NUL
    uint32_t last_touch_seq;   ///< Bumped each upsert; LRU eviction key
} nodes_db_entry_t;

void nodes_db_upsert(uint32_t node_id,
                     int16_t  rssi,
                     int8_t   snr_x4,
                     uint8_t  hops_away,
                     int32_t  lat_e7,
                     int32_t  lon_e7,
                     uint16_t battery_mv,
                     const uint8_t *alias,
                     uint8_t alias_len);

uint32_t nodes_db_change_seq(void);
uint32_t nodes_db_count(void);

/* Copy entry at relative index (0..count-1, ordered by most recently
 * touched first). Returns true on success; false if index >= count(). */
bool nodes_db_take_at(uint32_t index, nodes_db_entry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOKYA_CORE1_NODES_DB_H */
