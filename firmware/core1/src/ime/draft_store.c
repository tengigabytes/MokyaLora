/* draft_store.c — see draft_store.h.
 *
 * Phase 6 (Task #70): migrated from a raw 64 KB flash partition at
 * 0x10C10000 to LFS-backed files under /drafts/. Public API unchanged
 * — callers (ime_task) see the same load/save/clear/self_test surface.
 *
 * On-disk layout:
 *   /.draft_XXXXXXXX           where XXXXXXXX is draft_id in lowercase
 *                              hex (8 chars, zero-padded). File body
 *                              is a 16 B header + UTF-8 text bytes.
 *                              Flat top-level path mirrors dm_persist
 *                              to dodge the lfs_mkdir-on-first-write
 *                              race observed in Phase 2.4 selftest.
 *   header  uint32_t magic     'DRFT' = 0x54465244 (little-endian)
 *           uint32_t draft_id  redundant — caught path↔body mismatch
 *           uint16_t text_len  UTF-8 byte length
 *           uint16_t reserved
 *           uint32_t crc32     reserved (0)
 *
 * The eviction policy from the raw-flash version (16-slot fixed cap)
 * is gone — LFS lets each draft live as long as the partition has free
 * space. Capacity is now bounded by the LFS partition (1 MB) instead
 * of 64 KB, comfortably above any realistic draft volume (one draft
 * per peer + one per text setting field).
 *
 * Concurrency: c1_storage uses a recursive FreeRTOS mutex internally,
 * so multiple callers are safe; draft_store remains single-caller in
 * practice (only the IME task touches it).
 *
 * SPDX-License-Identifier: MIT
 */

#include "draft_store.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "c1_storage.h"
#include "../../lfs/lfs.h"
#include "mokya_trace.h"

#define DRAFT_PATH_PREFIX  "/.draft_"
#define DRAFT_PATH_MAX     32u
#define DRAFT_MAGIC        0x54465244u   /* 'DRFT' LE */

typedef struct {
    uint32_t magic;
    uint32_t draft_id;
    uint16_t text_len;
    uint16_t reserved;
    uint32_t crc32;             /* reserved, 0 */
} draft_header_t;

_Static_assert(sizeof(draft_header_t) == 16u, "header size");

/* SWD-readable diag. */
volatile uint32_t g_draft_saves   __attribute__((used)) = 0u;
volatile uint32_t g_draft_loads   __attribute__((used)) = 0u;
volatile uint32_t g_draft_clears  __attribute__((used)) = 0u;
volatile uint32_t g_draft_failures __attribute__((used)) = 0u;
volatile int32_t  g_draft_last_err __attribute__((used)) = 0;

/* SWD-driven self-test trigger (drained by bridge_task). */
volatile uint32_t g_draft_self_test_request __attribute__((used)) = 0u;
volatile uint32_t g_draft_self_test_done    __attribute__((used)) = 0u;
volatile uint8_t  g_draft_self_test_ok      __attribute__((used)) = 0u;

/* ── Path helpers ─────────────────────────────────────────────────── */

static void format_path(char *buf, size_t cap, uint32_t draft_id)
{
    snprintf(buf, cap, DRAFT_PATH_PREFIX "%08lx",
             (unsigned long)draft_id);
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool draft_store_init(void)
{
    /* No-op: top-level /.draft_* path needs no mkdir. Kept for API
     * symmetry with lru_persist_init(). */
    return true;
}

bool draft_store_load(uint32_t draft_id, char *buf, size_t cap, size_t *out_len)
{
    if (buf == NULL || draft_id == 0u) return false;
    if (!c1_storage_is_mounted()) return false;

    char path[DRAFT_PATH_MAX];
    format_path(path, sizeof(path), draft_id);
    if (!c1_storage_exists(path)) return false;

    c1_storage_file_t f;
    if (!c1_storage_open(&f, path, LFS_O_RDONLY)) {
        g_draft_failures++;
        g_draft_last_err = LFS_ERR_IO;
        return false;
    }
    draft_header_t h;
    int rc = c1_storage_read(&f, &h, sizeof(h));
    if (rc != (int)sizeof(h) || h.magic != DRAFT_MAGIC ||
        h.draft_id != draft_id ||
        h.text_len > DRAFT_STORE_TEXT_MAX) {
        (void)c1_storage_close(&f);
        g_draft_failures++;
        g_draft_last_err = (rc < 0) ? rc : LFS_ERR_CORRUPT;
        return false;
    }
    if ((size_t)h.text_len > cap) {
        (void)c1_storage_close(&f);
        return false;
    }
    if (h.text_len > 0) {
        rc = c1_storage_read(&f, buf, h.text_len);
        if (rc != (int)h.text_len) {
            (void)c1_storage_close(&f);
            g_draft_failures++;
            g_draft_last_err = (rc < 0) ? rc : LFS_ERR_IO;
            return false;
        }
    }
    (void)c1_storage_close(&f);
    if (out_len) *out_len = h.text_len;
    g_draft_loads++;
    return true;
}

bool draft_store_clear(uint32_t draft_id)
{
    if (draft_id == 0u) return false;
    if (!c1_storage_is_mounted()) return false;

    char path[DRAFT_PATH_MAX];
    format_path(path, sizeof(path), draft_id);
    bool ok = c1_storage_unlink(path);
    if (ok) g_draft_clears++;
    return ok;
}

bool draft_store_save(uint32_t draft_id, const char *text, size_t text_len)
{
    if (draft_id == 0u) return false;
    if (text_len > DRAFT_STORE_TEXT_MAX) return false;
    if (!c1_storage_is_mounted()) return false;

    if (text_len == 0) {
        return draft_store_clear(draft_id);
    }

    char path[DRAFT_PATH_MAX];
    format_path(path, sizeof(path), draft_id);

    c1_storage_file_t f;
    if (!c1_storage_open(&f, path,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        g_draft_failures++;
        g_draft_last_err = LFS_ERR_IO;
        return false;
    }
    draft_header_t h = {
        .magic    = DRAFT_MAGIC,
        .draft_id = draft_id,
        .text_len = (uint16_t)text_len,
        .reserved = 0,
        .crc32    = 0,
    };
    int rc = c1_storage_write(&f, &h, sizeof(h));
    if (rc == (int)sizeof(h) && text != NULL) {
        rc = c1_storage_write(&f, text, text_len);
    }
    bool closed = c1_storage_close(&f);
    if (rc < 0 || !closed) {
        g_draft_failures++;
        g_draft_last_err = (rc < 0) ? rc : LFS_ERR_IO;
        return false;
    }
    g_draft_saves++;
    return true;
}

bool draft_store_self_test(void)
{
    static const uint32_t kTestId = 0xC0FFEEDBu;
    static const char     kPattern[] = "draft_store_self_test_v2_lfs";
    const size_t          pat_len    = sizeof(kPattern) - 1u;

    TRACE_BARE("drft", "test_begin");
    if (!c1_storage_is_mounted()) {
        TRACE_BARE("drft", "test_unmounted");
        return false;
    }
    (void)draft_store_clear(kTestId);
    if (!draft_store_save(kTestId, kPattern, pat_len)) {
        TRACE_BARE("drft", "test_save_fail");
        return false;
    }
    char   buf[64];
    size_t got = 0;
    if (!draft_store_load(kTestId, buf, sizeof(buf), &got)) {
        TRACE_BARE("drft", "test_load_fail");
        (void)draft_store_clear(kTestId);
        return false;
    }
    if (got != pat_len || memcmp(buf, kPattern, pat_len) != 0) {
        TRACE("drft", "test_mismatch", "got=%u exp=%u",
              (unsigned)got, (unsigned)pat_len);
        (void)draft_store_clear(kTestId);
        return false;
    }
    (void)draft_store_clear(kTestId);
    TRACE("drft", "test_ok", "len=%u", (unsigned)pat_len);
    return true;
}
