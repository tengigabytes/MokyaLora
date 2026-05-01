/* c1_storage.c — see c1_storage.h.
 *
 * Mount-or-format flow:
 *   1. lfs_mount  → on LFS_ERR_CORRUPT proceed to step 2
 *   2. lfs_format → re-attempt lfs_mount
 *   3. write /.format_version with C1_STORAGE_SCHEMA_VERSION
 *
 * Schema check on existing FS:
 *   1. lfs_mount succeeds
 *   2. open /.format_version, read 4 B uint32, compare
 *   3. If mismatch: log + format (Phase 2 policy — no migration code yet)
 *
 * Single-open enforcement: g_c1_lfs_file_buffer is the only file cache
 * available, and it backs ALL writes. Allowing two concurrent opens
 * would race on this buffer. The file API checks s_file_in_use.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "c1_storage.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "mokya_trace.h"

/* ── State ─────────────────────────────────────────────────────────── */

/* lfs_t lives in PSRAM — only touched in non-flash-op windows. ~80 B. */
static lfs_t s_lfs __attribute__((section(".psram_bss")));

/* Top-level stats — bumped under the LFS lock so reads are coherent. */
static c1_storage_stats_t s_stats __attribute__((section(".psram_bss")));

/* Single-open flag (file cache shared). */
static volatile bool s_file_in_use __attribute__((section(".psram_bss")));

/* ── Helpers ───────────────────────────────────────────────────────── */

static int write_schema_version(void)
{
    lfs_file_t f;
    struct lfs_file_config cfg = {
        .buffer = g_c1_lfs_file_buffer,
        .attrs = NULL,
        .attr_count = 0,
    };
    int rc = lfs_file_opencfg(&s_lfs, &f, C1_STORAGE_VERSION_PATH,
                              LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &cfg);
    if (rc < 0) return rc;
    uint32_t v = C1_STORAGE_SCHEMA_VERSION;
    rc = lfs_file_write(&s_lfs, &f, &v, sizeof(v));
    int closerc = lfs_file_close(&s_lfs, &f);
    if (rc < 0) return rc;
    return closerc;
}

static int read_schema_version(uint32_t *out)
{
    lfs_file_t f;
    struct lfs_file_config cfg = {
        .buffer = g_c1_lfs_file_buffer,
        .attrs = NULL,
        .attr_count = 0,
    };
    int rc = lfs_file_opencfg(&s_lfs, &f, C1_STORAGE_VERSION_PATH,
                              LFS_O_RDONLY, &cfg);
    if (rc < 0) return rc;
    uint32_t v = 0;
    int n = lfs_file_read(&s_lfs, &f, &v, sizeof(v));
    int closerc = lfs_file_close(&s_lfs, &f);
    if (n != (int)sizeof(v)) return LFS_ERR_CORRUPT;
    if (closerc < 0) return closerc;
    *out = v;
    return LFS_ERR_OK;
}

static bool format_and_mount(void)
{
    s_stats.format_count++;
    int rc = lfs_format(&s_lfs, &g_c1_lfs_cfg);
    if (rc < 0) {
        TRACE("c1stor", "format_fail", "rc=%d", rc);
        return false;
    }
    rc = lfs_mount(&s_lfs, &g_c1_lfs_cfg);
    if (rc < 0) {
        TRACE("c1stor", "remount_fail", "rc=%d", rc);
        s_stats.mount_failures++;
        return false;
    }
    rc = write_schema_version();
    if (rc < 0) {
        TRACE("c1stor", "schema_write_fail", "rc=%d", rc);
        /* FS is mounted but schema marker missing. Best-effort — return
         * true so consumers can proceed; the next mount will treat this
         * as schema mismatch and re-format. Logged for diagnosis. */
    }
    s_stats.schema_version = C1_STORAGE_SCHEMA_VERSION;
    s_stats.mount_attempts++;
    s_stats.mounted = true;
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool c1_storage_init(void)
{
    if (s_stats.mounted) return true;

    s_stats.mount_attempts++;
    int rc = lfs_mount(&s_lfs, &g_c1_lfs_cfg);
    if (rc < 0) {
        TRACE("c1stor", "mount_fail", "rc=%d (corrupt or fresh — formatting)", rc);
        s_stats.mount_failures++;
        return format_and_mount();
    }

    /* Mounted — verify schema version. */
    uint32_t v = 0;
    int srrc = read_schema_version(&v);
    if (srrc == LFS_ERR_NOENT) {
        /* Existing FS without sentinel — first boot post-upgrade.
         * Just write the marker and continue. */
        TRACE_BARE("c1stor", "schema_missing");
        (void)write_schema_version();
        s_stats.schema_version = C1_STORAGE_SCHEMA_VERSION;
        s_stats.mounted = true;
        return true;
    }
    if (srrc < 0) {
        TRACE("c1stor", "schema_read_fail", "rc=%d (re-format)", srrc);
        (void)lfs_unmount(&s_lfs);
        return format_and_mount();
    }
    if (v != C1_STORAGE_SCHEMA_VERSION) {
        /* Phase 2 policy: incompatible schema → re-format. Add migration
         * here in future phases. */
        TRACE("c1stor", "schema_mismatch", "have=%u want=%u",
              (unsigned)v, (unsigned)C1_STORAGE_SCHEMA_VERSION);
        (void)lfs_unmount(&s_lfs);
        return format_and_mount();
    }

    s_stats.schema_version = v;
    s_stats.mounted = true;
    TRACE("c1stor", "mounted", "schema=%u blocks=%u",
          (unsigned)v, (unsigned)C1_LFS_BLOCK_COUNT);
    return true;
}

bool c1_storage_format_now(void)
{
    if (s_stats.mounted) {
        (void)lfs_unmount(&s_lfs);
        s_stats.mounted = false;
    }
    return format_and_mount();
}

bool c1_storage_is_mounted(void)
{
    return s_stats.mounted;
}

void c1_storage_get_stats(c1_storage_stats_t *out)
{
    if (out == NULL) return;
    *out = s_stats;
    if (s_stats.mounted) {
        lfs_ssize_t used = lfs_fs_size(&s_lfs);
        if (used >= 0) {
            out->blocks_used = (uint32_t)used;
            s_stats.blocks_used = (uint32_t)used;
        }
        out->blocks_total = C1_LFS_BLOCK_COUNT;
        s_stats.blocks_total = C1_LFS_BLOCK_COUNT;
    }
}

/* ── File API ──────────────────────────────────────────────────────── */

bool c1_storage_exists(const char *path)
{
    if (!s_stats.mounted || path == NULL) return false;
    struct lfs_info info;
    return lfs_stat(&s_lfs, path, &info) == LFS_ERR_OK;
}

bool c1_storage_unlink(const char *path)
{
    if (!s_stats.mounted || path == NULL) return false;
    int rc = lfs_remove(&s_lfs, path);
    return rc == LFS_ERR_OK || rc == LFS_ERR_NOENT;
}

bool c1_storage_open(c1_storage_file_t *f, const char *path, int lfs_flags)
{
    if (!s_stats.mounted || f == NULL || path == NULL) return false;
    if (s_file_in_use) {
        TRACE("c1stor", "open_busy", "path=%s", path);
        return false;
    }
    f->cfg.buffer = g_c1_lfs_file_buffer;
    f->cfg.attrs = NULL;
    f->cfg.attr_count = 0;
    int rc = lfs_file_opencfg(&s_lfs, &f->file, path, lfs_flags, &f->cfg);
    if (rc < 0) {
        TRACE("c1stor", "open_fail", "rc=%d path=%s", rc, path);
        f->open = false;
        return false;
    }
    f->open = true;
    s_file_in_use = true;
    return true;
}

int c1_storage_read(c1_storage_file_t *f, void *buf, size_t size)
{
    if (f == NULL || !f->open) return LFS_ERR_INVAL;
    return lfs_file_read(&s_lfs, &f->file, buf, size);
}

int c1_storage_write(c1_storage_file_t *f, const void *buf, size_t size)
{
    if (f == NULL || !f->open) return LFS_ERR_INVAL;
    return lfs_file_write(&s_lfs, &f->file, buf, size);
}

bool c1_storage_close(c1_storage_file_t *f)
{
    if (f == NULL) return false;
    bool ok = true;
    if (f->open) {
        int rc = lfs_file_close(&s_lfs, &f->file);
        if (rc < 0) {
            TRACE("c1stor", "close_fail", "rc=%d", rc);
            ok = false;
        }
        f->open = false;
        s_file_in_use = false;
    }
    return ok;
}
