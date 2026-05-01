/* dm_persist.h — DM conversation history persistence on c1_storage (LFS).
 *
 * Bridges dm_store (RAM-only ring) to a per-peer file in the unified
 * Core 1 LittleFS partition. Files live at top-level paths like
 * "/.dm_XXXXXXXX" where XXXXXXXX is the peer's 32-bit node_id in
 * lowercase hex. Top-level avoids the lfs_mkdir-on-first-write race
 * encountered during Phase 2.4 selftest debugging.
 *
 * Save trigger: dm_store mutation paths (ingest_inbound / _outbound /
 * update_ack / mark_read) set a dirty bit. dm_persist owns a 30 s
 * FreeRTOS soft timer that drains the dirty bitmap on each tick and
 * writes one file per dirty peer. 30 s smooths short bursts (e.g. a
 * conversation flurry) into one flash write per peer.
 *
 * Load trigger: dm_persist_load_all() called from dm_store_init() ->
 * c1_storage_init() success path. Iterates "/" via lfs_dir_open, reads
 * each "/.dm_*" file, parses the hex suffix → peer_node_id, restores
 * the slot into dm_store via dm_store_restore_peer().
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MOKYA_CORE1_DM_PERSIST_H
#define MOKYA_CORE1_DM_PERSIST_H

#include <stdbool.h>
#include <stdint.h>

#include "dm_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DM_PERSIST_MAGIC    0x31534D44u   /* 'DMS1' little-endian */
#define DM_PERSIST_VERSION  1u
#define DM_PERSIST_PATH_MAX 16             /* "/.dm_XXXXXXXX" + NUL */

/* On-disk record. Mirrors peer_slot_t fields one-to-one (modulo the
 * `in_use` flag which is implied by file existence). dm_msg_t is
 * defined in dm_store.h and is 232 B; ring is 8 messages so the body
 * is 1856 B. Total file size = 16 B header + 4 B counters + 1856 B
 * ring = 1876 B. Fits in one LFS sector (4 KB) with room to grow. */
typedef struct dm_persist_record {
    uint32_t magic;             /* DM_PERSIST_MAGIC */
    uint32_t version;           /* DM_PERSIST_VERSION */
    uint32_t peer_node_id;
    uint32_t last_activity_ms;
    uint8_t  unread;
    uint8_t  count;
    uint8_t  head;
    uint8_t  pad;
    dm_msg_t ring[DM_STORE_MSGS_PER];
} dm_persist_record_t;

/* Initialise the persistence layer and start the 30 s flush timer.
 * Must be called AFTER c1_storage_init() returns true and AFTER
 * dm_store_init(). Safe to call once; subsequent calls are no-ops. */
void dm_persist_init(void);

/* Force-flush all dirty peers immediately (synchronous). Returns the
 * number of files written. Used by graceful-shutdown paths, also
 * exercised by tests via SWD trigger. */
uint32_t dm_persist_flush_now(void);

/* Save one peer's slot to flash. Reads the latest snapshot from
 * dm_store via dm_store_snapshot_peer, serialises into the on-disk
 * record, writes /.dm_XXXXXXXX. Clears the peer's dirty bit on
 * success. Returns true on success. */
bool dm_persist_save_peer(uint32_t peer_node_id);

/* Walk "/" via lfs_dir_open, read each /.dm_* file, restore into
 * dm_store. Returns the number of peers loaded. Called by
 * dm_store_init() on the c1_storage-mounted path. */
uint32_t dm_persist_load_all(void);

/* T1 — drain SWD-driven test triggers (outbound DM inject, ack
 * update, indexed message read). Cheap; polls a few volatile uint32s
 * per call. bridge_task calls this once per loop iteration. */
void dm_persist_poll_swd_triggers(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOKYA_CORE1_DM_PERSIST_H */
