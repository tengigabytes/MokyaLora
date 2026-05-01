/* lfs_blockdev.h — LittleFS block-device adapter for Core 1.
 *
 * Maps LFS read/prog/erase/sync onto Pico SDK flash_range_* + XIP
 * memcpy. The wrap path defined in firmware/core1/src/ime/flash_safety_wrap.c
 * intercepts flash_range_program / flash_range_erase and parks Core 0
 * via shared-SRAM handshake (flash_lock_c0 + IPC_FLASH_DOORBELL_C0),
 * so block-device callers don't need to worry about cross-core safety.
 *
 * Region: 0x10C20000 .. 0x11000000 (3.875 MB) in Phase 2; can be
 * reduced to 0x10C00000..0x11000000 (4 MB) in Phase 7 once raw LRU +
 * draft partitions are reclaimed. Compile-time macros below.
 *
 * Buffer placement:
 *   - prog_buffer + per-file cache: SRAM (flash_range_program reads
 *     source via QMI which is busy with the flash op; PSRAM source
 *     would stall on the busy bus)
 *   - read_buffer + lookahead_buffer: PSRAM (used only outside flash
 *     ops; cheap, frees SRAM budget)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "../../lfs/lfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Region (Phase 2 placement; Phase 7 will lower start) ──────────── */

#define C1_LFS_REGION_XIP_BASE     0x10C20000u
#define C1_LFS_REGION_FLASH_OFFS   0x00C20000u   /* = XIP - 0x10000000  */
#define C1_LFS_REGION_END          0x11000000u   /* exclusive end       */
#define C1_LFS_REGION_SIZE         (C1_LFS_REGION_END - C1_LFS_REGION_XIP_BASE)

#define C1_LFS_BLOCK_SIZE          4096u
#define C1_LFS_PROG_SIZE           256u
#define C1_LFS_READ_SIZE           64u
#define C1_LFS_CACHE_SIZE          256u
#define C1_LFS_LOOKAHEAD_SIZE      128u
#define C1_LFS_BLOCK_COUNT         (C1_LFS_REGION_SIZE / C1_LFS_BLOCK_SIZE)
#define C1_LFS_BLOCK_CYCLES        500   /* dynamic wear-leveling threshold */

/* Pre-baked LFS configuration; used by c1_storage_init() to mount.
 * Caller does NOT need to copy or modify — passed straight to
 * lfs_mount / lfs_format. */
extern const struct lfs_config g_c1_lfs_cfg;

/* File handles use NULL buffer in lfs_file_config — LFS allocates a
 * 256 B prog cache from malloc per open file (routed via Pico SDK
 * malloc → pvPortMalloc to the FreeRTOS heap). Saves 256 B static
 * BSS at the cost of one heap alloc per concurrent file open. With
 * c1_storage's single-open policy that's at most 256 B heap any
 * given moment. */

#ifdef __cplusplus
}
#endif
