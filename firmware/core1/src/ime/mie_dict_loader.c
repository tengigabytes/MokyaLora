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
 * Alias rule: writes via UNCACHED (0x15xxxxxx), reads via CACHED
 * (0x11xxxxxx). Reasoning:
 *
 * 1. RP2350 XIP cache is write-BACK for PSRAM (empirically confirmed
 *    by the bringup psram_wthru test, 2026-04-22): writing via the
 *    cached alias leaves dirty lines sitting in the 4 KB cache.
 *    During an 8 MB sequential write most lines get evicted-and-
 *    written-through by later writes — but the tail ~1 MB worth
 *    remains dirty when the loop ends. Invalidating the cache at
 *    that point drops those dirty lines without flushing, leaving
 *    the tail of PSRAM holding pre-write content (~0.19 % silent
 *    word corruption — catastrophic for a dict blob).
 *
 *    Writing via uncached is simpler AND 2.5× faster (31 vs 12.6
 *    MB/s) than the write-cached + xip_cache_clean_range +
 *    xip_cache_invalidate_range alternative.
 *
 * 2. Reading via cached wins on random access patterns: the 32-byte
 *    cache-line burst amortises cmd+addr+dummy across 8 words, and
 *    hot trie nodes stay resident between lookups. Uncached reads
 *    are correct (with RXDELAY=CLKDIV applied — see psram.c) but
 *    pay full QMI overhead on every word, which hurts the MIE trie
 *    walk hot path disproportionately.
 */
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

/* v4 single-blob loader: copies the entire MIE4 binary to the start of
 * PSRAM and exposes a (ptr, size) pair via out->v4_blob.
 *
 * Geometry check: the blob's total_size (header[0x20]) must fit within
 * the partition AND within the PSRAM zh_dat budget (kept here for
 * symmetry with the MDBL path; v4 uses the same 2 MB slot at PSRAM
 * offset 0). */
static bool mie_dict_load_v4_to_psram(mie_dict_pointers_t *out,
                                       const uint8_t *blob_base)
{
    /* Read total_size from the header (LE u32 at offset 0x20). */
    uint32_t total_size = 0;
    memcpy(&total_size, blob_base + MIE_MIE4_TOTAL_SIZE_OFF, sizeof(total_size));

    if (total_size < MIE_MIE4_HEADER_SIZE ||
        total_size > MIE_DICT_PARTITION_SIZE ||
        total_size > PSRAM_ZH_DAT_BUDGET + PSRAM_ZH_VAL_BUDGET) {
        g_mie_dict_load_status = MIE_DICT_LOAD_ERR_GEOMETRY;
        return false;
    }

    /* Read embedded English section pointers from the (possibly-grown)
     * header. Sections at offsets 0x30..0x3F were added 2026-04-24 so
     * v4 blobs can carry their own SmartEn dict — old v4 builds still
     * have zeros here, which correctly degrades to "no English". */
    uint32_t en_dat_off = 0, en_dat_size = 0;
    uint32_t en_val_off = 0, en_val_size = 0;
    if (total_size >= MIE_MIE4_HEADER_SIZE) {
        memcpy(&en_dat_off,  blob_base + MIE_MIE4_EN_DAT_OFF_OFF,  4);
        memcpy(&en_dat_size, blob_base + MIE_MIE4_EN_DAT_SIZE_OFF, 4);
        memcpy(&en_val_off,  blob_base + MIE_MIE4_EN_VAL_OFF_OFF,  4);
        memcpy(&en_val_size, blob_base + MIE_MIE4_EN_VAL_SIZE_OFF, 4);
        /* Bounds check: every declared section must fit within total_size
         * and within the English-side PSRAM budget (same cap as the MDBL
         * path so fragmented-heap dispatch is not possible). */
        if (en_dat_size > PSRAM_EN_DAT_BUDGET ||
            en_val_size > PSRAM_EN_VAL_BUDGET ||
            (en_dat_size && (uint64_t)en_dat_off + en_dat_size > total_size) ||
            (en_val_size && (uint64_t)en_val_off + en_val_size > total_size)) {
            g_mie_dict_load_status = MIE_DICT_LOAD_ERR_GEOMETRY;
            return false;
        }
    }

    /* Copy via UNCACHED alias (same rationale as MDBL path). */
    memcpy((void *)PSRAM_WRITE_ADDR(PSRAM_ZH_DAT_OFF), blob_base, total_size);

    /* Invalidate cached alias for the freshly-written range. */
    #define PSRAM_XIP_OFFSET   0x01000000u
    #define CACHE_LINE_ALIGN(sz) (((sz) + 7u) & ~7u)
    xip_cache_invalidate_range(PSRAM_XIP_OFFSET + PSRAM_ZH_DAT_OFF,
                               CACHE_LINE_ALIGN(total_size));
    #undef PSRAM_XIP_OFFSET
    #undef CACHE_LINE_ALIGN

    if (out) {
        out->v4_blob      = (const uint8_t *)PSRAM_READ_ADDR(PSRAM_ZH_DAT_OFF);
        out->v4_blob_size = total_size;
        /* English pointers alias INTO the v4 blob region — no separate
         * PSRAM copy is needed. Size > 0 gates the en_searcher attach
         * in ime_task.cpp. */
        if (en_dat_size) {
            out->en_dat      = out->v4_blob + en_dat_off;
            out->en_dat_size = en_dat_size;
        }
        if (en_val_size) {
            out->en_val      = out->v4_blob + en_val_off;
            out->en_val_size = en_val_size;
        }
    }

    g_mie_dict_load_status = MIE_DICT_LOAD_OK;
    return true;
}

bool mie_dict_load_to_psram(mie_dict_pointers_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (g_psram_read_id[0] != MOKYA_PSRAM_MFID || g_psram_read_id[1] != MOKYA_PSRAM_KGD) {
        g_mie_dict_load_status = MIE_DICT_LOAD_ERR_NO_PSRAM;
        return false;
    }

    const uint8_t *blob_base = (const uint8_t *)MIE_DICT_PARTITION_ADDR;

    /* Magic dispatch: MIE4 -> v4 single-blob loader; MDBL -> legacy v2 path. */
    uint32_t magic = 0;
    memcpy(&magic, blob_base, sizeof(magic));
    if (magic == MIE_MIE4_MAGIC) {
        return mie_dict_load_v4_to_psram(out, blob_base);
    }

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

    /* Drop any cache lines that might alias what we just wrote via the
     * UNCACHED path. Before the dict load ran, the cache may have been
     * seeded with uninitialised PSRAM content from incidental reads
     * through the cached alias (e.g. SWD inspection, PSRAM probe,
     * prefetch); leaving those lines resident would mean subsequent
     * reads via the cached alias return pre-load bytes instead of the
     * fresh dict. We don't have dirty lines of our own to worry about
     * because all writes in this function went via UNCACHED.
     *
     * Must be invalidate-by-range — the cheaper xip_cache_invalidate_
     * all() set/way variant is broken on RP2350 and leaves stale lines
     * intact (bug originally manifested as Candidate.word returning
     * 0x55 at positions where PSRAM held 0x25). The range API takes an
     * offset from XIP_BASE (0x10000000), so PSRAM sections live at
     * 0x01000000 + PSRAM_*_OFF.                                         */
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
