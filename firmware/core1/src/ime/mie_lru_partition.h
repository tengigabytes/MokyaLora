/* mie_lru_partition.h -- Location of the personal LRU blob in flash.
 *
 * Phase 1.6 persistence partition. The blob is a single LruCache
 * serialisation (see firmware/mie/include/mie/lru_cache.h) written by
 * firmware/core1/src/ime/lru_persist.c via the wrapped flash API.
 *
 * Flash map (W25Q128JW, 16 MB):
 *   0x10000000  Core 0 Meshtastic image       2 MB slot
 *   0x10200000  Core 1 bridge image           2 MB slot
 *   0x10400000  MIE dict blob (MDBL)          6 MB reserved
 *   0x10A00000  MIE font blob (MIEF)          2 MB reserved
 *   0x10C00000  MIE LRU persist (this)       64 KB reserved, 8 KB used
 *   0x10C10000  Free / future LittleFS        ~3.94 MB
 *   0x11000000  (flash end)
 *
 * Size rationale: an LruCache blob is 8 B header + 128 × 48 B = 6 152 B,
 * rounded up to 2 × 4 KB erase sectors = 8 KB. 64 KB reserved provides
 * a 16-sector pool for later wear-leveling slot rotation without having
 * to repartition the flash map.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIE_LRU_PARTITION_ADDR   0x10C00000u
#define MIE_LRU_PARTITION_SIZE   (64u * 1024u)

/* Offset from XIP_BASE (0x10000000). flash_range_erase/program take this
 * form, not the XIP-relative pointer used for reads. */
#define MIE_LRU_PARTITION_OFFSET 0x00C00000u

/* Active slot: first 4 KB sector pair of the reserved region. A future
 * wear-leveled rotation will pick from a pool of slots; for now we pin
 * the single active slot at offset 0.                                   */
#define MIE_LRU_SLOT_OFFSET      MIE_LRU_PARTITION_OFFSET
#define MIE_LRU_SLOT_SIZE        (8u * 1024u)

#ifdef __cplusplus
} /* extern "C" */
#endif
