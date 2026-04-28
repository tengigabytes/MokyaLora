/* draft_store.c — see draft_store.h.
 *
 * Implementation notes
 *
 *   - Flash erase granularity is 4 KB (FLASH_SECTOR_SIZE on RP2350); each
 *     slot maps 1:1 to one sector so save/clear are a single erase + program.
 *   - Reads go through the XIP-cached window. Writes require XIP cache
 *     invalidation for the affected range so the next read sees the new
 *     bytes (mirrors lru_persist.cpp).
 *   - Header serialisation is plain little-endian — the RP2350 is LE so
 *     struct copy works, but we use explicit field writes to make the
 *     wire format independent of struct packing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "draft_store.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "hardware/flash.h"
#include "hardware/xip_cache.h"

#include "mokya_trace.h"

/* ── Partition map ───────────────────────────────────────────────────── */

#define PARTITION_ADDR    0x10C10000u                 /* XIP read base    */
#define PARTITION_OFFSET  0x00C10000u                 /* flash byte off   */
#define SLOT_SIZE         FLASH_SECTOR_SIZE           /* 4096             */
#define SLOT_HEADER_BYTES 16u
#define MAGIC             0x54465244u                 /* 'DRFT' LE        */

_Static_assert((PARTITION_OFFSET % SLOT_SIZE) == 0,   "slot align");
_Static_assert(DRAFT_STORE_TEXT_MAX + SLOT_HEADER_BYTES == SLOT_SIZE,
               "slot layout");
_Static_assert(DRAFT_STORE_SLOT_COUNT * SLOT_SIZE == 0x10000u,
               "partition fits 64 KB");

/* ── Header read helpers ─────────────────────────────────────────────── */

typedef struct {
    uint32_t magic;
    uint32_t draft_id;
    uint16_t text_len;
    uint16_t reserved;
    uint32_t crc32;
} slot_header_t;

static inline const uint8_t *slot_xip(uint32_t idx)
{
    return (const uint8_t *)(uintptr_t)(PARTITION_ADDR + idx * SLOT_SIZE);
}

static inline uint32_t slot_offset(uint32_t idx)
{
    return PARTITION_OFFSET + idx * SLOT_SIZE;
}

static void read_header(uint32_t idx, slot_header_t *h)
{
    const uint8_t *p = slot_xip(idx);
    h->magic    = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    h->draft_id = ((uint32_t)p[4]) | ((uint32_t)p[5] << 8) |
                  ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    h->text_len = (uint16_t)(p[8] | (p[9] << 8));
    h->reserved = (uint16_t)(p[10] | (p[11] << 8));
    h->crc32    = ((uint32_t)p[12]) | ((uint32_t)p[13] << 8) |
                  ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
}

/* Find the slot holding `draft_id`. Returns true on hit and writes the
 * slot index to `*idx_out`. Caller must own the partition mutex. */
static bool find_slot(uint32_t draft_id, uint32_t *idx_out)
{
    if (draft_id == 0u) return false;
    for (uint32_t i = 0; i < DRAFT_STORE_SLOT_COUNT; ++i) {
        slot_header_t h;
        read_header(i, &h);
        if (h.magic == MAGIC && h.draft_id == draft_id) {
            *idx_out = i;
            return true;
        }
    }
    return false;
}

/* Find an erased slot suitable for a new draft. Returns true on success;
 * if the partition is full, returns the first occupied slot (Phase 2
 * MVP eviction policy: just stomp on the first existing draft). */
static bool find_free_or_evict(uint32_t *idx_out)
{
    /* First pass — erased slots. */
    for (uint32_t i = 0; i < DRAFT_STORE_SLOT_COUNT; ++i) {
        slot_header_t h;
        read_header(i, &h);
        if (h.magic == 0xFFFFFFFFu) {
            *idx_out = i;
            return true;
        }
    }
    /* Second pass — first occupied slot. */
    for (uint32_t i = 0; i < DRAFT_STORE_SLOT_COUNT; ++i) {
        slot_header_t h;
        read_header(i, &h);
        if (h.magic == MAGIC) {
            *idx_out = i;
            return true;
        }
    }
    return false;
}

/* ── Public API ──────────────────────────────────────────────────────── */

bool draft_store_init(void)
{
    /* Phase 2 MVP — nothing to set up. Erased state is the same as a
     * never-touched partition because we treat magic 0xFFFFFFFF as empty. */
    return true;
}

bool draft_store_load(uint32_t draft_id, char *buf, size_t cap, size_t *out_len)
{
    if (buf == NULL || draft_id == 0u) return false;

    uint32_t idx;
    if (!find_slot(draft_id, &idx)) return false;

    slot_header_t h;
    read_header(idx, &h);
    if (h.text_len > DRAFT_STORE_TEXT_MAX) return false;
    if ((size_t)h.text_len > cap)          return false;

    const uint8_t *src = slot_xip(idx) + SLOT_HEADER_BYTES;
    if (h.text_len > 0) memcpy(buf, src, h.text_len);
    if (out_len) *out_len = h.text_len;
    return true;
}

bool draft_store_clear(uint32_t draft_id)
{
    if (draft_id == 0u) return false;

    uint32_t idx;
    if (!find_slot(draft_id, &idx)) return true;   /* nothing to clear */

    flash_range_erase(slot_offset(idx), SLOT_SIZE);
    xip_cache_invalidate_range(slot_offset(idx), SLOT_SIZE);
    return true;
}

bool draft_store_save(uint32_t draft_id, const char *text, size_t text_len)
{
    if (draft_id == 0u) return false;
    if (text_len > DRAFT_STORE_TEXT_MAX) return false;

    if (text_len == 0) {
        return draft_store_clear(draft_id);
    }

    uint32_t idx;
    bool reused = find_slot(draft_id, &idx);
    if (!reused) {
        if (!find_free_or_evict(&idx)) return false;
    }

    /* Build the full slot in a heap scratch — flash_range_program
     * requires aligned writes and we want the unused tail to be 0xFF
     * (post-erase state) so future searches don't trip on stale bytes. */
    uint8_t *scratch = (uint8_t *)pvPortMalloc(SLOT_SIZE);
    if (scratch == NULL) return false;
    memset(scratch, 0xFF, SLOT_SIZE);

    /* Header (little-endian, see read_header). */
    scratch[0]  = (uint8_t)(MAGIC      );
    scratch[1]  = (uint8_t)(MAGIC >> 8 );
    scratch[2]  = (uint8_t)(MAGIC >> 16);
    scratch[3]  = (uint8_t)(MAGIC >> 24);
    scratch[4]  = (uint8_t)(draft_id      );
    scratch[5]  = (uint8_t)(draft_id >> 8 );
    scratch[6]  = (uint8_t)(draft_id >> 16);
    scratch[7]  = (uint8_t)(draft_id >> 24);
    scratch[8]  = (uint8_t)(text_len      );
    scratch[9]  = (uint8_t)(text_len >> 8 );
    scratch[10] = 0;
    scratch[11] = 0;
    /* crc32 left as 0 — Phase 2 MVP, room to add later */
    scratch[12] = 0; scratch[13] = 0; scratch[14] = 0; scratch[15] = 0;

    if (text != NULL && text_len > 0) {
        memcpy(scratch + SLOT_HEADER_BYTES, text, text_len);
    }

    flash_range_erase(slot_offset(idx), SLOT_SIZE);
    flash_range_program(slot_offset(idx), scratch, SLOT_SIZE);
    xip_cache_invalidate_range(slot_offset(idx), SLOT_SIZE);

    vPortFree(scratch);
    return true;
}

bool draft_store_self_test(void)
{
    static const uint32_t kTestId = 0xC0FFEEDBu;
    static const char    kPattern[] = "draft_store_self_test_v1_2026";  /* 30 chars */
    const size_t         pat_len    = sizeof(kPattern) - 1u;

    TRACE_BARE("drft", "test_begin");

    /* Phase 1: clean slate. */
    (void)draft_store_clear(kTestId);

    /* Phase 2: save. */
    if (!draft_store_save(kTestId, kPattern, pat_len)) {
        TRACE_BARE("drft", "test_save_fail");
        return false;
    }

    /* Phase 3: load. */
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

    /* Cleanup so production doesn't carry the test slot. */
    (void)draft_store_clear(kTestId);
    TRACE("drft", "test_ok", "len=%u", (unsigned)pat_len);
    return true;
}
