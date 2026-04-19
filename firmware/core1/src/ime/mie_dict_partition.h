/* mie_dict_partition.h -- Location of the MDBL dict blob in flash.
 *
 * The blob is flashed as an independent partition by
 * scripts/build_and_flash.sh. Core 1 reads the TOC at boot and copies
 * the individual sections to PSRAM, then hands those PSRAM pointers to
 * mie_dict_open_memory(). See firmware/mie/tools/pack_dict_blob.py for
 * the layout.
 *
 * Flash map (W25Q128JW, 16 MB):
 *   0x10000000  Core 0 Meshtastic image  (2 MB slot)
 *   0x10200000  Core 1 bridge image      (2 MB slot)
 *   0x10400000  MIE dict blob (this)     (6 MB reserved)
 *   0x10A00000  MIE font blob (MIEF)     (2 MB reserved)
 *   0x10C00000  LittleFS / free          (4 MB)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flash XIP address where the MDBL dict blob begins. Must match
 * DICT_ADDR in scripts/build_and_flash.sh. */
#define MIE_DICT_PARTITION_ADDR  0x10400000u

/* Reserved partition size. Current blob is ~5 MB; 6 MB leaves headroom
 * for English expansion without encroaching on the font partition at
 * 0x10A00000 (see mie_font_partition.h). */
#define MIE_DICT_PARTITION_SIZE  (6u * 1024u * 1024u)

/* MDBL header layout (little-endian). See pack_dict_blob.py. */
#define MIE_MDBL_MAGIC           0x4C42444Du  /* "MDBL" as u32 LE */
#define MIE_MDBL_VERSION         1u
#define MIE_MDBL_HEADER_SIZE     0x28u

typedef struct {
    uint32_t magic;        /* MIE_MDBL_MAGIC */
    uint16_t version;      /* MIE_MDBL_VERSION */
    uint16_t reserved;
    uint32_t zh_dat_off;   /* offset from blob start; 0 if section absent */
    uint32_t zh_dat_size;
    uint32_t zh_val_off;
    uint32_t zh_val_size;
    uint32_t en_dat_off;
    uint32_t en_dat_size;
    uint32_t en_val_off;
    uint32_t en_val_size;
} mie_mdbl_header_t;

#ifdef __cplusplus
} /* extern "C" */
#endif
