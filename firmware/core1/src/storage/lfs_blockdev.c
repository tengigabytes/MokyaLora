/* lfs_blockdev.c — see lfs_blockdev.h.
 *
 * read   : memcpy from XIP base + block*block_size + off
 * prog   : flash_range_program (intercepted by flash_safety_wrap → parks Core 0)
 * erase  : flash_range_erase   (intercepted by flash_safety_wrap → parks Core 0)
 * sync   : noop (programs/erases return synchronously after wrap unparks)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lfs_blockdev.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/platform.h"

#include "FreeRTOS.h"
#include "semphr.h"

/* ── Buffer allocation (placement matters — see header) ────────────── */

/* prog_buffer: SRAM-resident. flash_range_program reads source via QMI
 * which is busy with the flash op for the duration; a PSRAM source
 * would deadlock on the same bus. */
static uint8_t s_prog_buf[C1_LFS_CACHE_SIZE]
    __attribute__((aligned(4)));

/* read_buffer + lookahead_buffer: PSRAM-resident. Touched only outside
 * the flash-op critical section. Saves ~384 B of tight SRAM budget. */
static uint8_t s_read_buf[C1_LFS_CACHE_SIZE]
    __attribute__((aligned(4)))
    __attribute__((section(".psram_bss")));

static uint8_t s_lookahead_buf[C1_LFS_LOOKAHEAD_SIZE]
    __attribute__((aligned(4)))
    __attribute__((section(".psram_bss")));

/* ── Cross-task lock (LittleFS may be called from multiple FreeRTOS
 *     tasks — bridge_task for cascade flushes, lvgl_task for direct
 *     waypoint / DM saves). LFS_THREADSAFE gating in vendored lfs.h
 *     turns the cb hooks on; lazy-create the recursive mutex on first
 *     entry to avoid an init-order dependency. ──────────────────── */

#ifdef LFS_THREADSAFE
static SemaphoreHandle_t s_lock_handle __attribute__((section(".psram_bss")));

static int c1_lfs_lock(const struct lfs_config *c)
{
    (void)c;
    if (s_lock_handle == NULL) {
        s_lock_handle = xSemaphoreCreateRecursiveMutex();
    }
    if (s_lock_handle == NULL) return LFS_ERR_IO;
    if (xSemaphoreTakeRecursive(s_lock_handle, portMAX_DELAY) != pdTRUE) {
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

static int c1_lfs_unlock(const struct lfs_config *c)
{
    (void)c;
    if (s_lock_handle == NULL) return LFS_ERR_OK;
    xSemaphoreGiveRecursive(s_lock_handle);
    return LFS_ERR_OK;
}
#endif /* LFS_THREADSAFE */

/* ── Block-device callbacks ────────────────────────────────────────── */

static int c1_lfs_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buffer, lfs_size_t size)
{
    if (block >= c->block_count) return LFS_ERR_IO;
    if (off + size > c->block_size) return LFS_ERR_IO;

    /* XIP-mapped read — direct memcpy from cached flash. No flash op
     * in flight here (LFS callbacks are serialised by the lock above). */
    const uint8_t *src = (const uint8_t *)
        (C1_LFS_REGION_XIP_BASE + (uintptr_t)block * c->block_size + off);
    memcpy(buffer, src, size);
    return LFS_ERR_OK;
}

static int c1_lfs_prog(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, const void *buffer, lfs_size_t size)
{
    if (block >= c->block_count) return LFS_ERR_IO;
    if (off + size > c->block_size) return LFS_ERR_IO;

    /* flash_range_program is wrapped by flash_safety_wrap.c — parks C0
     * via flash_lock_c0 + doorbell, runs the bootrom flash op, unparks. */
    uint32_t flash_offs = C1_LFS_REGION_FLASH_OFFS
        + (uint32_t)block * c->block_size + (uint32_t)off;
    flash_range_program(flash_offs, (const uint8_t *)buffer, size);

    /* Invalidate the XIP cache range so subsequent reads see the new
     * bytes (without this, a read after prog can return the old cached
     * line content). */
    xip_cache_invalidate_range(flash_offs, size);
    return LFS_ERR_OK;
}

static int c1_lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    if (block >= c->block_count) return LFS_ERR_IO;
    uint32_t flash_offs = C1_LFS_REGION_FLASH_OFFS
        + (uint32_t)block * c->block_size;
    flash_range_erase(flash_offs, c->block_size);
    xip_cache_invalidate_range(flash_offs, c->block_size);
    return LFS_ERR_OK;
}

static int c1_lfs_sync(const struct lfs_config *c)
{
    (void)c;
    /* Pico SDK flash_range_program / _erase return after the chip's
     * busy bit clears — already synchronous. Nothing to do. */
    return LFS_ERR_OK;
}

/* ── Public configuration ──────────────────────────────────────────── */

const struct lfs_config g_c1_lfs_cfg = {
    .context        = NULL,
    .read           = c1_lfs_read,
    .prog           = c1_lfs_prog,
    .erase          = c1_lfs_erase,
    .sync           = c1_lfs_sync,
#ifdef LFS_THREADSAFE
    .lock           = c1_lfs_lock,
    .unlock         = c1_lfs_unlock,
#endif
    .read_size      = C1_LFS_READ_SIZE,
    .prog_size      = C1_LFS_PROG_SIZE,
    .block_size     = C1_LFS_BLOCK_SIZE,
    .block_count    = C1_LFS_BLOCK_COUNT,
    .block_cycles   = C1_LFS_BLOCK_CYCLES,
    .cache_size     = C1_LFS_CACHE_SIZE,
    .lookahead_size = C1_LFS_LOOKAHEAD_SIZE,
    .read_buffer      = s_read_buf,
    .prog_buffer      = s_prog_buf,
    .lookahead_buffer = s_lookahead_buf,
};
