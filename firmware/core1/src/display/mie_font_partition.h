/* mie_font_partition.h -- Location of the MIEF font blob in flash.
 *
 * The blob is flashed as an independent partition by
 * scripts/build_and_flash.sh, parallel to the MDBL dict partition
 * at 0x10400000. Core 1's font driver reads glyphs directly from
 * flash (XIP) without copying to PSRAM — see docs/design-notes/
 * firmware-architecture.md §2.1 "font glyphs stay XIP".
 *
 * Flash map (W25Q128JW, 16 MB):
 *   0x10000000  Core 0 Meshtastic image  (2 MB slot)
 *   0x10200000  Core 1 bridge image      (2 MB slot)
 *   0x10400000  MIE dict blob (MDBL)     (6 MB reserved)
 *   0x10A00000  MIE font blob (MIEF)     (2 MB reserved, this)
 *   0x10C00000  MIE LRU persist          (64 KB reserved, Phase 1.6)
 *   0x10C10000  Free / future LittleFS   (~3.94 MB)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flash XIP address where the MIEF font blob begins. Must match
 * FONT_ADDR in scripts/build_and_flash.sh. */
#define MIE_FONT_PARTITION_ADDR  0x10A00000u

/* Reserved partition size. Current blob is ~250 KB (5.1k-char subset);
 * 2 MB leaves room for full-range CJK (~900 KB at 16 px 1 bpp) plus
 * future growth (32 px LVGL labels, icon fonts, language packs) without
 * touching the flash map. */
#define MIE_FONT_PARTITION_SIZE  (2u * 1024u * 1024u)

#ifdef __cplusplus
} /* extern "C" */
#endif
