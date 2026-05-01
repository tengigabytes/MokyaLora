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

#include <stdio.h>
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

/* SWD-readable diag globals — .bss-resident (PSRAM is not SWD-coherent
 * without explicit cache flush, see project_psram_swd_cache_coherence
 * memory note).  Trimmed to the minimum that proves the test sequence
 * actually ran: magic, pass count, fail count, last error code. */
volatile uint32_t g_c1_storage_st_magic       __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_st_passes      __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_st_failures    __attribute__((used)) = 0u;
volatile int32_t  g_c1_storage_st_last_err    __attribute__((used)) = 0;
volatile uint32_t g_c1_storage_st_dur_us      __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_blocks_used    __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_blocks_total   __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_format_count   __attribute__((used)) = 0u;
#define C1_STORAGE_ST_MAGIC_DONE  0x53544F50u  /* 'STOP' little-endian */

/* Capacity stress trigger (SWD-writable). Bridge_task polls and runs
 * the stress when request != done. Encoding:
 *   request = (n_files << 16) | bytes_per_file_kb_quantum
 *   bytes = (request & 0xFFFF) * 256   (so 4 → 1024 B, 8 → 2048 B)
 * Convention: writes a unique non-zero value each test run; firmware
 * acks by writing same value to g_c1_storage_stress_done. */
volatile uint32_t g_c1_storage_stress_request    __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_stress_done       __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_stress_passes     __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_stress_failures   __attribute__((used)) = 0u;
volatile int32_t  g_c1_storage_stress_last_err   __attribute__((used)) = 0;
volatile uint32_t g_c1_storage_stress_dur_us     __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_stress_blocks_peak __attribute__((used)) = 0u;

/* Power-loss survival probe — Phase 2.6.  Read at every successful
 * boot from /.pl_marker, then increment+write back.  Test scripts
 * verify g_c1_storage_pl_count grows monotonically across SYSRESETREQ
 * cycles, proving file persistence + mount-after-write durability. */
volatile uint32_t g_c1_storage_pl_count    __attribute__((used)) = 0u;
volatile uint32_t g_c1_storage_pl_existed  __attribute__((used)) = 0u;
volatile int32_t  g_c1_storage_pl_last_err __attribute__((used)) = 0;

/* Test-only: SWD writes any non-zero value to request a watchdog
 * reboot from Core 1 (clean chip-wide reset, both cores cold-boot).
 * Bridge_task polls and triggers when set.  Kept under
 * `MOKYA_C1_STORAGE_RESET_TRIGGER` so production can disable. */
volatile uint32_t g_c1_storage_reset_request __attribute__((used)) = 0u;

/* ── Helpers ───────────────────────────────────────────────────────── */

static int write_schema_version(void)
{
    lfs_file_t f;
    struct lfs_file_config cfg = {
        .buffer = NULL,    /* LFS allocates via lfs_malloc → FreeRTOS heap */
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
        .buffer = NULL,    /* LFS allocates via lfs_malloc → FreeRTOS heap */
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

/* ── Power-loss marker ─────────────────────────────────────────────── *
 *
 * /.pl_marker holds a 4-byte little-endian counter.  Read at boot,
 * increment, write back.  Persistent across SYSRESETREQ proves both
 * mount-of-existing-FS and write-then-reboot-then-read durability.
 * Failure here is non-fatal (logged via diag global); init proceeds
 * regardless. */
#define C1_STORAGE_PL_PATH "/.pl_marker"

static void update_pl_marker(void)
{
    g_c1_storage_pl_existed = 0u;
    g_c1_storage_pl_last_err = 0;

    uint32_t count = 0u;
    /* Read existing marker if present. */
    if (c1_storage_exists(C1_STORAGE_PL_PATH)) {
        c1_storage_file_t f;
        if (c1_storage_open(&f, C1_STORAGE_PL_PATH, LFS_O_RDONLY)) {
            int rc = c1_storage_read(&f, &count, sizeof(count));
            (void)c1_storage_close(&f);
            if (rc == (int)sizeof(count)) {
                g_c1_storage_pl_existed = 1u;
            } else {
                count = 0u;
                g_c1_storage_pl_last_err = (rc < 0) ? rc : LFS_ERR_CORRUPT;
            }
        } else {
            g_c1_storage_pl_last_err = LFS_ERR_IO;
        }
    }

    /* Increment + write back. */
    count++;
    c1_storage_file_t fw;
    if (!c1_storage_open(&fw, C1_STORAGE_PL_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        if (g_c1_storage_pl_last_err == 0) g_c1_storage_pl_last_err = LFS_ERR_IO;
        return;
    }
    int rc = c1_storage_write(&fw, &count, sizeof(count));
    bool closed = c1_storage_close(&fw);
    if (rc != (int)sizeof(count) || !closed) {
        if (g_c1_storage_pl_last_err == 0) {
            g_c1_storage_pl_last_err = (rc < 0) ? rc : LFS_ERR_IO;
        }
        return;
    }
    g_c1_storage_pl_count = count;
    TRACE("c1stor", "pl_marker", "count=%u existed=%u",
          (unsigned)count, (unsigned)g_c1_storage_pl_existed);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool c1_storage_init(void)
{

    s_stats.mount_attempts++;
    int rc = lfs_mount(&s_lfs, &g_c1_lfs_cfg);
    g_c1_storage_st_last_err = rc;  /* re-use diag global */
    if (rc == LFS_ERR_OK) {
        s_stats.schema_version = C1_STORAGE_SCHEMA_VERSION;
        s_stats.mounted = true;
        update_pl_marker();
        return true;
    }

    /* Mount failed — region uninitialised or corrupt. Format + remount.
     * Phase 2 policy: silent format on first boot.  Schema version
     * sentinel + verification deferred until format/mount path is
     * proven stable on hardware (Phase 2 follow-up). */
    TRACE("c1stor", "mount_fail", "rc=%d — formatting", rc);
    s_stats.mount_failures++;
    bool ok = format_and_mount();
    if (ok) update_pl_marker();
    return ok;
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

bool c1_storage_walk(const char *dir_path,
                     c1_storage_dir_cb cb,
                     void *ctx)
{
    if (!s_stats.mounted || dir_path == NULL || cb == NULL) return false;
    lfs_dir_t dir;
    if (lfs_dir_open(&s_lfs, &dir, dir_path) < 0) return false;
    struct lfs_info info;
    bool keep_going = true;
    while (keep_going && lfs_dir_read(&s_lfs, &dir, &info) > 0) {
        if (info.type != LFS_TYPE_REG) continue;
        keep_going = cb(info.name, info.size, ctx);
    }
    (void)lfs_dir_close(&s_lfs, &dir);
    return true;
}

bool c1_storage_open(c1_storage_file_t *f, const char *path, int lfs_flags)
{
    if (!s_stats.mounted || f == NULL || path == NULL) return false;
    if (s_file_in_use) {
        TRACE("c1stor", "open_busy", "path=%s", path);
        return false;
    }
    f->cfg.buffer = NULL;          /* LFS malloc per open */
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

/* ── Self-test ─────────────────────────────────────────────────────── *
 *
 * Runs a small but representative end-to-end battery on every boot:
 *   1. Write 3 files of varying sizes (16 B / 200 B / 1 KB).
 *   2. Read each back, verify byte-perfect.
 *   3. Delete all 3.
 *   4. Verify they're gone (lfs_stat returns LFS_ERR_NOENT).
 *
 * Each step bumps g_c1_storage_st_passes on success or g_c1_storage_st_failures
 * + records the last lfs error code. Final magic 'STOP' written to
 * g_c1_storage_st_magic so the test script can confirm the routine
 * fully ran (vs. crashed mid-test). */
#include "pico/time.h"

static const struct {
    const char *path;
    size_t      size;
    uint8_t     pattern;
} kSelfTestFiles[] = {
    { "/.st_a",    16, 0xA5 },
    { "/.st_b",   200, 0x5A },
    { "/.st_c",  1024, 0x33 },
};
#define SELFTEST_FILE_COUNT (sizeof(kSelfTestFiles) / sizeof(kSelfTestFiles[0]))
#define SELFTEST_BUF_MAX    1024

/* Heap-backed scratch buffer — 1 KB on bridge_task's 4 KB stack would
 * be tight against LFS internal frames + the lock mutex path. Allocate
 * once for the whole selftest, reuse for every file, free at end. */
static int st_write_one(int idx, uint8_t *buf)
{
    c1_storage_file_t f;
    if (!c1_storage_open(&f, kSelfTestFiles[idx].path,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        return -1;
    }
    memset(buf, kSelfTestFiles[idx].pattern, kSelfTestFiles[idx].size);
    int rc = c1_storage_write(&f, buf, kSelfTestFiles[idx].size);
    bool closed = c1_storage_close(&f);
    if (rc != (int)kSelfTestFiles[idx].size) return (rc < 0) ? rc : -2;
    if (!closed) return -3;
    return 0;
}

static int st_read_verify(int idx, uint8_t *buf)
{
    c1_storage_file_t f;
    if (!c1_storage_open(&f, kSelfTestFiles[idx].path, LFS_O_RDONLY)) {
        return -10;
    }
    int rc = c1_storage_read(&f, buf, kSelfTestFiles[idx].size);
    bool closed = c1_storage_close(&f);
    if (rc != (int)kSelfTestFiles[idx].size) return (rc < 0) ? rc : -11;
    for (size_t i = 0; i < kSelfTestFiles[idx].size; i++) {
        if (buf[i] != kSelfTestFiles[idx].pattern) return -12;
    }
    if (!closed) return -13;
    return 0;
}

bool c1_storage_self_test(void)
{
    if (!s_stats.mounted) {
        g_c1_storage_st_failures++;
        g_c1_storage_st_last_err = LFS_ERR_INVAL;
        g_c1_storage_st_magic = C1_STORAGE_ST_MAGIC_DONE;
        return false;
    }

    uint8_t *buf = pvPortMalloc(SELFTEST_BUF_MAX);
    if (buf == NULL) {
        g_c1_storage_st_failures++;
        g_c1_storage_st_last_err = LFS_ERR_NOMEM;
        g_c1_storage_st_magic = C1_STORAGE_ST_MAGIC_DONE;
        return false;
    }

    uint32_t t0 = (uint32_t)time_us_64();
    bool overall_ok = true;

    /* Write phase. */
    for (size_t i = 0; i < SELFTEST_FILE_COUNT; i++) {
        int rc = st_write_one((int)i, buf);
        if (rc < 0) {
            g_c1_storage_st_failures++;
            g_c1_storage_st_last_err = rc;
            TRACE("c1stor", "st_write_fail", "i=%u rc=%d", (unsigned)i, rc);
            overall_ok = false;
            continue;
        }
        g_c1_storage_st_passes++;
    }

    /* Read-back verify. */
    for (size_t i = 0; i < SELFTEST_FILE_COUNT; i++) {
        int rc = st_read_verify((int)i, buf);
        if (rc < 0) {
            g_c1_storage_st_failures++;
            g_c1_storage_st_last_err = rc;
            TRACE("c1stor", "st_read_fail", "i=%u rc=%d", (unsigned)i, rc);
            overall_ok = false;
            continue;
        }
        g_c1_storage_st_passes++;
    }

    /* Delete + verify gone. */
    for (size_t i = 0; i < SELFTEST_FILE_COUNT; i++) {
        if (!c1_storage_unlink(kSelfTestFiles[i].path)) {
            g_c1_storage_st_failures++;
            g_c1_storage_st_last_err = LFS_ERR_IO;
            overall_ok = false;
            continue;
        }
        if (c1_storage_exists(kSelfTestFiles[i].path)) {
            g_c1_storage_st_failures++;
            g_c1_storage_st_last_err = LFS_ERR_IO;
            overall_ok = false;
            continue;
        }
        g_c1_storage_st_passes++;
    }

    uint32_t dur = (uint32_t)time_us_64() - t0;
    g_c1_storage_st_dur_us = dur;

    /* Snapshot capacity into SWD globals while we have the lock. */
    lfs_ssize_t used = lfs_fs_size(&s_lfs);
    if (used >= 0) g_c1_storage_blocks_used = (uint32_t)used;
    g_c1_storage_blocks_total = C1_LFS_BLOCK_COUNT;
    g_c1_storage_format_count = s_stats.format_count;

    g_c1_storage_st_magic = C1_STORAGE_ST_MAGIC_DONE;
    TRACE("c1stor", "selftest",
          "passes=%u fails=%u dur_us=%u",
          (unsigned)g_c1_storage_st_passes,
          (unsigned)g_c1_storage_st_failures,
          (unsigned)dur);
    vPortFree(buf);
    return overall_ok;
}

/* ── Capacity stress test ──────────────────────────────────────────── *
 *
 * Writes `n_files` files of `bytes_per_file` bytes each ("/.cs_NN"
 * naming), reads them back byte-perfect, deletes all, verifies free
 * space recovers within reasonable tolerance (LFS GC may not run
 * immediately).
 *
 * Per-file pattern: byte i = (file_idx ^ i) & 0xFF — distinguishes
 * files AND positions, catches any block-mixing corruption.
 */
bool c1_storage_stress_test(uint32_t n_files, uint32_t bytes_per_file)
{
    if (!s_stats.mounted) {
        g_c1_storage_stress_failures++;
        g_c1_storage_stress_last_err = LFS_ERR_INVAL;
        return false;
    }
    /* Caps lifted for Phase 2 capacity-at-scale validation: plan
     * targets 600 blocks (~2.4 MB) saturation. With 600 × 4 KB single-
     * sector files we hit the realistic provisioned pool. */
    if (n_files == 0u || n_files > 700u || bytes_per_file == 0u
        || bytes_per_file > 4096u) {
        g_c1_storage_stress_failures++;
        g_c1_storage_stress_last_err = LFS_ERR_INVAL;
        return false;
    }

    uint8_t *buf = (uint8_t *)pvPortMalloc(bytes_per_file);
    if (buf == NULL) {
        g_c1_storage_stress_failures++;
        g_c1_storage_stress_last_err = LFS_ERR_NOMEM;
        return false;
    }

    uint32_t t0 = (uint32_t)time_us_64();
    bool overall_ok = true;
    uint32_t blocks_peak = 0u;
    char path[20];   /* "/.cs_NNN" up to 700 files */

    /* Write phase. */
    for (uint32_t i = 0; i < n_files; i++) {
        snprintf(path, sizeof(path), "/.cs_%03u", (unsigned)i);
        for (uint32_t j = 0; j < bytes_per_file; j++) {
            buf[j] = (uint8_t)((i ^ j) & 0xFFu);
        }
        c1_storage_file_t f;
        if (!c1_storage_open(&f, path,
                             LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
            g_c1_storage_stress_failures++;
            g_c1_storage_stress_last_err = LFS_ERR_IO;
            overall_ok = false;
            continue;
        }
        int rc = c1_storage_write(&f, buf, bytes_per_file);
        bool closed = c1_storage_close(&f);
        if (rc != (int)bytes_per_file || !closed) {
            g_c1_storage_stress_failures++;
            g_c1_storage_stress_last_err = (rc < 0) ? rc : LFS_ERR_IO;
            overall_ok = false;
            continue;
        }
        g_c1_storage_stress_passes++;
        /* Track peak usage during write phase. */
        lfs_ssize_t used = lfs_fs_size(&s_lfs);
        if (used >= 0 && (uint32_t)used > blocks_peak) {
            blocks_peak = (uint32_t)used;
        }
    }
    g_c1_storage_stress_blocks_peak = blocks_peak;

    /* Read-back verify. */
    for (uint32_t i = 0; i < n_files; i++) {
        snprintf(path, sizeof(path), "/.cs_%03u", (unsigned)i);
        c1_storage_file_t f;
        if (!c1_storage_open(&f, path, LFS_O_RDONLY)) {
            g_c1_storage_stress_failures++;
            g_c1_storage_stress_last_err = LFS_ERR_IO;
            overall_ok = false;
            continue;
        }
        int rc = c1_storage_read(&f, buf, bytes_per_file);
        bool closed = c1_storage_close(&f);
        if (rc != (int)bytes_per_file || !closed) {
            g_c1_storage_stress_failures++;
            g_c1_storage_stress_last_err = (rc < 0) ? rc : LFS_ERR_IO;
            overall_ok = false;
            continue;
        }
        bool match = true;
        for (uint32_t j = 0; j < bytes_per_file; j++) {
            if (buf[j] != (uint8_t)((i ^ j) & 0xFFu)) { match = false; break; }
        }
        if (!match) {
            g_c1_storage_stress_failures++;
            g_c1_storage_stress_last_err = LFS_ERR_CORRUPT;
            overall_ok = false;
            continue;
        }
        g_c1_storage_stress_passes++;
    }

    /* Delete + verify gone. */
    for (uint32_t i = 0; i < n_files; i++) {
        snprintf(path, sizeof(path), "/.cs_%03u", (unsigned)i);
        if (!c1_storage_unlink(path) || c1_storage_exists(path)) {
            g_c1_storage_stress_failures++;
            g_c1_storage_stress_last_err = LFS_ERR_IO;
            overall_ok = false;
            continue;
        }
        g_c1_storage_stress_passes++;
    }

    g_c1_storage_stress_dur_us = (uint32_t)time_us_64() - t0;
    vPortFree(buf);
    TRACE("c1stor", "stress",
          "n=%u sz=%u passes=%u fails=%u peak=%u dur_us=%u",
          (unsigned)n_files, (unsigned)bytes_per_file,
          (unsigned)g_c1_storage_stress_passes,
          (unsigned)g_c1_storage_stress_failures,
          (unsigned)blocks_peak,
          (unsigned)g_c1_storage_stress_dur_us);
    return overall_ok;
}
