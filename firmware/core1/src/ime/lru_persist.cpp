/* lru_persist.cpp — see lru_persist.h.
 *
 * Phase 7 (Task #71): migrated from a 64 KB raw flash partition at
 * 0x10C00000 to a single LFS file /.mie_lru.bin. Public API is
 * unchanged — ime_task.cpp keeps calling lru_persist_init / _load /
 * _save with the same signatures.
 *
 * Save still allocates a transient scratch on the FreeRTOS heap to
 * hold the serialised blob (up to 6152 B for kCap=128). The blob is
 * pushed straight to LFS — no flash padding, no Core 0 park dance
 * (LFS writes go through the storage layer's own flash_range_program
 * wrapping). The 64 KB partition at 0x10C00000 becomes unused; the
 * address comment in mie_lru_partition.h documents the historical
 * footprint and is kept until a future flash-map cleanup.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lru_persist.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include <mie/ime_logic.h>

extern "C" {
#include "c1_storage.h"
#include "../../lfs/lfs.h"
#include "mokya_trace.h"
}

#define LRU_PERSIST_PATH    "/.mie_lru.bin"
#define LRU_BLOB_MAX_BYTES  (8u + (size_t)mie::LruCache::kCap * 48u)

extern "C" {
volatile uint32_t g_lru_persist_saves    __attribute__((used)) = 0u;
volatile uint32_t g_lru_persist_loads    __attribute__((used)) = 0u;
volatile uint32_t g_lru_persist_failures __attribute__((used)) = 0u;
volatile int32_t  g_lru_persist_last_err __attribute__((used)) = 0;
volatile uint32_t g_lru_persist_bytes    __attribute__((used)) = 0u;

/* SWD-driven save trigger drained inside the IME task tripwire. */
volatile uint32_t g_lru_save_force_request __attribute__((used)) = 0u;
volatile uint32_t g_lru_save_force_done    __attribute__((used)) = 0u;

/* Bridge-side request/done mirror — bridge_task forwards a SWD save
 * request into g_lru_save_force_request, then waits for the IME task
 * tripwire to fire and increment saves before mirroring back. */
volatile uint32_t g_lru_persist_save_request __attribute__((used)) = 0u;
volatile uint32_t g_lru_persist_save_done    __attribute__((used)) = 0u;
volatile uint8_t  g_lru_persist_save_ok      __attribute__((used)) = 0u;
}

extern "C" bool lru_persist_init(void)
{
    /* Nothing to set up — c1_storage is mounted by bridge_task before
     * the IME task starts, and the path is top-level (no mkdir). The
     * symbol stays exported for ime_task.cpp call-site symmetry. */
    return true;
}

extern "C" bool lru_persist_load(mie::ImeLogic *ime)
{
    if (!ime) return false;
    if (!c1_storage_is_mounted()) return false;
    if (!c1_storage_exists(LRU_PERSIST_PATH)) return false;

    /* Two-step read: first pull the 8-byte header to discover the
     * actual entry count, then allocate exactly the bytes needed.
     * Avoids a 6152 B malloc on the boot-time load path where heap
     * is most fragmented. Empty-cache files round-trip with just an
     * 8 B alloc. */
    c1_storage_file_t f;
    if (!c1_storage_open(&f, LRU_PERSIST_PATH, LFS_O_RDONLY)) {
        g_lru_persist_failures++;
        g_lru_persist_last_err = LFS_ERR_IO;
        return false;
    }
    uint8_t header[8];
    int hrc = c1_storage_read(&f, header, sizeof(header));
    if (hrc != (int)sizeof(header)) {
        (void)c1_storage_close(&f);
        g_lru_persist_failures++;
        g_lru_persist_last_err = (hrc < 0) ? hrc : LFS_ERR_CORRUPT;
        return false;
    }
    /* Parse `count_` from header bytes 6..7 (LruCache layout). */
    uint16_t count = (uint16_t)(header[6] | ((uint16_t)header[7] << 8));
    if ((size_t)count > (size_t)mie::LruCache::kCap) {
        (void)c1_storage_close(&f);
        g_lru_persist_failures++;
        g_lru_persist_last_err = LFS_ERR_CORRUPT;
        return false;
    }
    size_t need = sizeof(header) + (size_t)count * 48u;

    uint8_t *buf = (uint8_t *)pvPortMalloc(need);
    if (!buf) {
        (void)c1_storage_close(&f);
        g_lru_persist_failures++;
        g_lru_persist_last_err = LFS_ERR_NOMEM;
        return false;
    }
    /* Replay the header into the front of buf so deserialize() sees a
     * complete blob; read the entry tail directly into buf+8. */
    memcpy(buf, header, sizeof(header));
    int rc = (int)sizeof(header);
    if (count > 0) {
        int tail = c1_storage_read(&f, buf + sizeof(header), need - sizeof(header));
        (void)c1_storage_close(&f);
        if (tail != (int)(need - sizeof(header))) {
            vPortFree(buf);
            g_lru_persist_failures++;
            g_lru_persist_last_err = (tail < 0) ? tail : LFS_ERR_CORRUPT;
            return false;
        }
        rc = (int)need;
    } else {
        (void)c1_storage_close(&f);
    }

    bool ok = ime->load_lru(buf, rc);
    vPortFree(buf);
    if (ok) {
        g_lru_persist_loads++;
        g_lru_persist_bytes = (uint32_t)rc;
    } else {
        g_lru_persist_failures++;
        g_lru_persist_last_err = LFS_ERR_CORRUPT;
    }
    return ok;
}

extern "C" bool lru_persist_save(mie::ImeLogic *ime)
{
    if (!ime) return false;
    if (!c1_storage_is_mounted()) return false;

    const int need = ime->lru_serialized_size();
    if (need <= 0 || (size_t)need > LRU_BLOB_MAX_BYTES) return false;

    uint8_t *scratch = (uint8_t *)pvPortMalloc((size_t)need);
    if (!scratch) {
        g_lru_persist_failures++;
        g_lru_persist_last_err = LFS_ERR_NOMEM;
        return false;
    }
    const int wrote = ime->serialize_lru(scratch, need);
    if (wrote != need) {
        vPortFree(scratch);
        g_lru_persist_failures++;
        g_lru_persist_last_err = LFS_ERR_INVAL;
        return false;
    }

    c1_storage_file_t f;
    if (!c1_storage_open(&f, LRU_PERSIST_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) {
        vPortFree(scratch);
        g_lru_persist_failures++;
        g_lru_persist_last_err = LFS_ERR_IO;
        return false;
    }
    int rc = c1_storage_write(&f, scratch, (size_t)need);
    bool closed = c1_storage_close(&f);
    vPortFree(scratch);
    if (rc != need || !closed) {
        g_lru_persist_failures++;
        g_lru_persist_last_err = (rc < 0) ? rc : LFS_ERR_IO;
        return false;
    }
    g_lru_persist_saves++;
    g_lru_persist_bytes = (uint32_t)need;
    return true;
}
