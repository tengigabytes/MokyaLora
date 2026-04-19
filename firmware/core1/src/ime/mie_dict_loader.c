/* mie_dict_loader.c -- see mie_dict_loader.h for contract.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mie_dict_loader.h"

#include <string.h>

#include "mie_dict_partition.h"
#include "psram.h"

/* Per-section PSRAM destinations (cached base). Chosen so each section
 * starts on a 1 MB boundary — arbitrary, just keeps the layout easy to
 * eyeball in SWD and leaves headroom if individual sections grow. */
#define PSRAM_ZH_DAT_ADDR   (MOKYA_PSRAM_CACHED_BASE + 0x00000000u)  /* 0x11000000 */
#define PSRAM_ZH_VAL_ADDR   (MOKYA_PSRAM_CACHED_BASE + 0x00200000u)  /* 0x11200000 */
#define PSRAM_EN_DAT_ADDR   (MOKYA_PSRAM_CACHED_BASE + 0x00500000u)  /* 0x11500000 */
#define PSRAM_EN_VAL_ADDR   (MOKYA_PSRAM_CACHED_BASE + 0x00510000u)  /* 0x11510000 */

#define PSRAM_ZH_DAT_BUDGET 0x00200000u  /*  2 MB */
#define PSRAM_ZH_VAL_BUDGET 0x00300000u  /*  3 MB */
#define PSRAM_EN_DAT_BUDGET 0x00010000u  /* 64 KB — current 44 KB + growth */
#define PSRAM_EN_VAL_BUDGET 0x00040000u  /* 256 KB — current 36 KB + growth */

volatile uint32_t g_mie_dict_load_status = MIE_DICT_LOAD_NONE;

bool mie_dict_load_to_psram(mie_dict_pointers_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (g_psram_read_id[0] != MOKYA_PSRAM_MFID || g_psram_read_id[1] != MOKYA_PSRAM_KGD) {
        g_mie_dict_load_status = MIE_DICT_LOAD_ERR_NO_PSRAM;
        return false;
    }

    const uint8_t *blob_base = (const uint8_t *)MIE_DICT_PARTITION_ADDR;
    const mie_mdbl_header_t *hdr = (const mie_mdbl_header_t *)blob_base;

    if (hdr->magic != MIE_MDBL_MAGIC) {
        g_mie_dict_load_status = MIE_DICT_LOAD_ERR_MAGIC;
        return false;
    }
    if (hdr->version != MIE_MDBL_VERSION) {
        g_mie_dict_load_status = MIE_DICT_LOAD_ERR_VERSION;
        return false;
    }

    /* Geometry check: every section's [off, off+size) must stay inside
     * the reserved partition window. Guards against a garbage flash
     * partition triggering a multi-megabyte memcpy off the end of XIP. */
    const uint32_t part_end = MIE_DICT_PARTITION_SIZE;
    #define CHECK_RANGE(off, size) \
        do { if ((size) && ((off) < MIE_MDBL_HEADER_SIZE || \
                            (uint64_t)(off) + (size) > part_end)) { \
            g_mie_dict_load_status = MIE_DICT_LOAD_ERR_GEOMETRY; \
            return false; \
        } } while (0)
    CHECK_RANGE(hdr->zh_dat_off, hdr->zh_dat_size);
    CHECK_RANGE(hdr->zh_val_off, hdr->zh_val_size);
    CHECK_RANGE(hdr->en_dat_off, hdr->en_dat_size);
    CHECK_RANGE(hdr->en_val_off, hdr->en_val_size);
    #undef CHECK_RANGE

    /* Per-section budget check — makes sure a grown dict doesn't
     * overflow into the next PSRAM block silently. */
    if (hdr->zh_dat_size > PSRAM_ZH_DAT_BUDGET ||
        hdr->zh_val_size > PSRAM_ZH_VAL_BUDGET ||
        hdr->en_dat_size > PSRAM_EN_DAT_BUDGET ||
        hdr->en_val_size > PSRAM_EN_VAL_BUDGET) {
        g_mie_dict_load_status = MIE_DICT_LOAD_ERR_GEOMETRY;
        return false;
    }

    /* Copy each section to its fixed PSRAM address. Using memcpy from
     * flash XIP to cached PSRAM; XIP_CTRL_WRITABLE_M1 was set by
     * psram_init so writes pass through. */
    if (hdr->zh_dat_size)
        memcpy((void *)PSRAM_ZH_DAT_ADDR,
               blob_base + hdr->zh_dat_off, hdr->zh_dat_size);
    if (hdr->zh_val_size)
        memcpy((void *)PSRAM_ZH_VAL_ADDR,
               blob_base + hdr->zh_val_off, hdr->zh_val_size);
    if (hdr->en_dat_size)
        memcpy((void *)PSRAM_EN_DAT_ADDR,
               blob_base + hdr->en_dat_off, hdr->en_dat_size);
    if (hdr->en_val_size)
        memcpy((void *)PSRAM_EN_VAL_ADDR,
               blob_base + hdr->en_val_off, hdr->en_val_size);

    if (out) {
        out->zh_dat      = (const uint8_t *)PSRAM_ZH_DAT_ADDR;
        out->zh_dat_size = hdr->zh_dat_size;
        out->zh_val      = (const uint8_t *)PSRAM_ZH_VAL_ADDR;
        out->zh_val_size = hdr->zh_val_size;
        out->en_dat      = (const uint8_t *)PSRAM_EN_DAT_ADDR;
        out->en_dat_size = hdr->en_dat_size;
        out->en_val      = (const uint8_t *)PSRAM_EN_VAL_ADDR;
        out->en_val_size = hdr->en_val_size;
    }

    g_mie_dict_load_status = MIE_DICT_LOAD_OK;
    return true;
}
