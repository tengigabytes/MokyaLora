/* mie_dict_loader.c -- see mie_dict_loader.h for contract.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mie_dict_loader.h"

#include <string.h>

#include "hardware/xip_cache.h"

#include "mie_dict_partition.h"
#include "psram.h"

/* Per-section PSRAM destinations — offsets from the PSRAM base. Each
 * section starts on a (arbitrary) 1 MB boundary so the layout is easy
 * to eyeball in SWD and grown sections don't collide.
 *
 * IMPORTANT (see psram.c §§comment at ctrl.WRITABLE_M1): writes go to
 * the UNCACHED alias (0x15xxxxxx), reads go through the CACHED alias
 * (0x11xxxxxx). Writing via the cached alias only populates the XIP
 * cache without write-through to PSRAM; once the cache evicts those
 * lines (any subsequent multi-MB traffic does so), subsequent reads
 * pick up garbage from PSRAM and search results come back with stray
 * 0x06 prefixes / embedded NULs in Candidate.word.
 *
 * P2-15 update: uncached reads also trigger ~47% word errors on
 * tight-loop CPU reads (trie prefix scan, glyph lookup), because
 * back-to-back single-beat accesses don't give APS6404L enough CS-HIGH
 * time for DRAM refresh even with MAX_SELECT=1. Cached burst reads
 * (32-byte line fill + CPU work between) pass 0%. Reads MUST go
 * through the CACHED alias — this was the original intent per the
 * comment above but the macro had regressed to uncached. */
#define PSRAM_ZH_DAT_OFF    0x00000000u
#define PSRAM_ZH_VAL_OFF    0x00200000u
#define PSRAM_EN_DAT_OFF    0x00500000u
#define PSRAM_EN_VAL_OFF    0x00510000u

#define PSRAM_WRITE_ADDR(off) (MOKYA_PSRAM_UNCACHED_BASE + (off))
#define PSRAM_READ_ADDR(off)  (MOKYA_PSRAM_CACHED_BASE   + (off))

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

    /* Copy each section to the UNCACHED PSRAM alias so writes land on
     * real PSRAM. Consumers get the CACHED alias so their reads go
     * through the XIP cache. */
    if (hdr->zh_dat_size)
        memcpy((void *)PSRAM_WRITE_ADDR(PSRAM_ZH_DAT_OFF),
               blob_base + hdr->zh_dat_off, hdr->zh_dat_size);
    if (hdr->zh_val_size)
        memcpy((void *)PSRAM_WRITE_ADDR(PSRAM_ZH_VAL_OFF),
               blob_base + hdr->zh_val_off, hdr->zh_val_size);
    if (hdr->en_dat_size)
        memcpy((void *)PSRAM_WRITE_ADDR(PSRAM_EN_DAT_OFF),
               blob_base + hdr->en_dat_off, hdr->en_dat_size);
    if (hdr->en_val_size)
        memcpy((void *)PSRAM_WRITE_ADDR(PSRAM_EN_VAL_OFF),
               blob_base + hdr->en_val_off, hdr->en_val_size);

    /* Invalidate the XIP cache lines that cover the dict we just wrote
     * to PSRAM. We must invalidate by address range rather than the
     * cheaper `xip_cache_invalidate_all()` — empirically the set/way
     * variant leaves stale lines on RP2350 (reads through the cached
     * alias still return pre-load bytes until explicit address-range
     * invalidation, which is how this bug manifested: Candidate.word
     * came back with a byte like 0x55 at positions where PSRAM held
     * 0x25, because the cache was seeded with uninitialised PSRAM
     * content during the pre-load bring-up). `xip_cache_invalidate_range`
     * takes an offset from XIP_BASE (0x10000000), so PSRAM sections
     * live at 0x01000000 + PSRAM_*_OFF.                                  */
    #define PSRAM_XIP_OFFSET   0x01000000u    /* 0x11000000 - 0x10000000  */
    #define CACHE_LINE_ALIGN(sz) (((sz) + 7u) & ~7u)
    if (hdr->zh_dat_size)
        xip_cache_invalidate_range(PSRAM_XIP_OFFSET + PSRAM_ZH_DAT_OFF,
                                   CACHE_LINE_ALIGN(hdr->zh_dat_size));
    if (hdr->zh_val_size)
        xip_cache_invalidate_range(PSRAM_XIP_OFFSET + PSRAM_ZH_VAL_OFF,
                                   CACHE_LINE_ALIGN(hdr->zh_val_size));
    if (hdr->en_dat_size)
        xip_cache_invalidate_range(PSRAM_XIP_OFFSET + PSRAM_EN_DAT_OFF,
                                   CACHE_LINE_ALIGN(hdr->en_dat_size));
    if (hdr->en_val_size)
        xip_cache_invalidate_range(PSRAM_XIP_OFFSET + PSRAM_EN_VAL_OFF,
                                   CACHE_LINE_ALIGN(hdr->en_val_size));
    #undef PSRAM_XIP_OFFSET
    #undef CACHE_LINE_ALIGN

    *(volatile uint32_t *)0x2007FCE0u = 0x494E5631u;  /* 'INV1' — done */

    if (out) {
        out->zh_dat      = (const uint8_t *)PSRAM_READ_ADDR(PSRAM_ZH_DAT_OFF);
        out->zh_dat_size = hdr->zh_dat_size;
        out->zh_val      = (const uint8_t *)PSRAM_READ_ADDR(PSRAM_ZH_VAL_OFF);
        out->zh_val_size = hdr->zh_val_size;
        out->en_dat      = (const uint8_t *)PSRAM_READ_ADDR(PSRAM_EN_DAT_OFF);
        out->en_dat_size = hdr->en_dat_size;
        out->en_val      = (const uint8_t *)PSRAM_READ_ADDR(PSRAM_EN_VAL_OFF);
        out->en_val_size = hdr->en_val_size;
    }

    g_mie_dict_load_status = MIE_DICT_LOAD_OK;
    return true;
}
