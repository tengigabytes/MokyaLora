/* c1_storage.h — Core 1 unified persistence (LittleFS-backed).
 *
 * Public façade over the vendored LittleFS instance. Consumers
 * (dm_persist, waypoint_persist, history_persist, etc.) call this API
 * rather than touching `lfs_*` directly.
 *
 * Lifecycle:
 *   1. c1_storage_init() — at Core 1 task-startup. Mounts LFS; on
 *      LFS_ERR_CORRUPT formats and re-mounts. Writes /.format_version
 *      with the current schema id on fresh format. Idempotent — safe
 *      to call once.
 *   2. Files: open / read / write / close. Single concurrent open
 *      enforced (shared SRAM file cache).
 *   3. c1_storage_get_stats() — for periodic capacity reporting.
 *
 * Concurrency: thread-safe. The underlying lfs_t lock/unlock callbacks
 * (firmware/core1/src/storage/lfs_blockdev.c) take a FreeRTOS recursive
 * mutex, so multiple tasks can call into c1_storage_* concurrently.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "../../lfs/lfs.h"
#include "lfs_blockdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Schema version — bump when on-disk record formats change. */
#define C1_STORAGE_SCHEMA_VERSION  1u
#define C1_STORAGE_VERSION_PATH    "/.format_version"

typedef struct {
    lfs_file_t              file;
    struct lfs_file_config  cfg;          ///< Points at g_c1_lfs_file_buffer
    bool                    open;
} c1_storage_file_t;

typedef struct {
    bool     mounted;                     ///< Last init/mount succeeded
    uint32_t mount_attempts;
    uint32_t mount_failures;
    uint32_t format_count;                ///< How many times format ran
    uint32_t blocks_used;                 ///< Snapshot from last get_stats
    uint32_t blocks_total;                ///< Total provisioned in cfg
    uint32_t schema_version;              ///< Read from /.format_version
} c1_storage_stats_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Mount LFS; format + mount on LFS_ERR_CORRUPT. Returns true on a
 * usable filesystem (mounted, schema ok). Idempotent. */
bool c1_storage_init(void);

/* Force-format the device. Destroys all data. Used by debug tooling
 * and the build_and_flash.sh --wipe-fs option. Returns true on success. */
bool c1_storage_format_now(void);

/* True if init() succeeded and the FS is currently mounted. */
bool c1_storage_is_mounted(void);

/* Snapshot stats (cheap; uses cached counters + on-demand
 * lfs_fs_size). */
void c1_storage_get_stats(c1_storage_stats_t *out);

/* Self-test — write/read/verify/delete a few small files end-to-end.
 * Bumps SWD-readable diag globals for external scripts. Non-fatal:
 * returns false on first failure, but does NOT unmount or change state.
 * Runs in ~50 ms typical (one erase + a few programs). */
bool c1_storage_self_test(void);

/* Capacity stress test (SWD-triggered) — write `n_files` files of
 * `bytes_per_file` bytes each (file path "/.cs_NN"), verify reads,
 * delete all, verify free space recovers. Diagnostic only — wears
 * flash so default-OFF; bridge_task polls g_c1_storage_stress_request
 * vs g_c1_storage_stress_done and calls this when they differ.
 * Bumps SWD globals g_c1_storage_stress_{passes,failures,last_err,
 * dur_us,blocks_used_peak}. */
bool c1_storage_stress_test(uint32_t n_files, uint32_t bytes_per_file);

/* ── File API ──────────────────────────────────────────────────────── */

/* Path inspection. Returns true iff `path` exists in the FS. */
bool c1_storage_exists(const char *path);

/* Delete `path`. Returns true on success (or if path doesn't exist). */
bool c1_storage_unlink(const char *path);

/* Walk a directory, invoking `cb` for every regular file (skips
 * subdirs). Stops early if `cb` returns false. `ctx` is passed
 * through. Used by dm_persist to enumerate /.dm_* on boot. */
typedef bool (*c1_storage_dir_cb)(const char *name,
                                  uint32_t size,
                                  void *ctx);
bool c1_storage_walk(const char *dir_path,
                     c1_storage_dir_cb cb,
                     void *ctx);

/* Open `path` with LFS flags (LFS_O_RDONLY, LFS_O_WRONLY|LFS_O_CREAT,
 * etc — see lfs.h). Single concurrent open is enforced; second open
 * returns false. */
bool c1_storage_open(c1_storage_file_t *f,
                     const char *path,
                     int lfs_flags);

/* Read up to `size` bytes. Returns bytes actually read (>=0) or
 * negative LFS error. */
int  c1_storage_read(c1_storage_file_t *f, void *buf, size_t size);

/* Write `size` bytes. Returns bytes written (>=0) or negative LFS
 * error. Caller MUST close to flush. */
int  c1_storage_write(c1_storage_file_t *f, const void *buf, size_t size);

/* Close + flush. Returns true on success. Always nulls out f->open
 * even on error (so the next open() can proceed). */
bool c1_storage_close(c1_storage_file_t *f);

/* T1.C1/C2 — drain SWD-driven test triggers (file-corruption injection).
 * Cheap; bridge_task calls once per loop iter. */
void c1_storage_poll_swd_triggers(void);

#ifdef __cplusplus
}
#endif
