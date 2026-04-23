/* lru_persist.cpp -- See lru_persist.h for contract.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lru_persist.h"
#include "mie_lru_partition.h"

#include <string.h>

#include "FreeRTOS.h"

#include "hardware/flash.h"
#include "hardware/xip_cache.h"

#include <mie/ime_logic.h>

namespace {

/* Header + max entries. 64 × 48 B + 8 B header = 3080 B. 4-byte aligned
 * for flash_range_program. Rounded up to the flash page size (256 B) so
 * the entire program call hits whole pages — 13 pages = 3328 B. */
constexpr size_t kBlobMaxBytes = 8u + 64u * 48u;     /* 3080 B */
constexpr size_t kPageSize     = FLASH_PAGE_SIZE;    /* 256 B */
constexpr size_t kPaddedSize   = ((kBlobMaxBytes + kPageSize - 1u)
                                  / kPageSize) * kPageSize;    /* 3328 B */
static_assert(kPaddedSize <= MIE_LRU_SLOT_SIZE,
              "LRU blob must fit in one slot");

/* Scratch buffer in .bss. Sizing this to match the reduced LruCache::kCap
 * (64 entries) keeps the total .bss cost below the 4 KB headroom Core 1
 * can spare after LVGL + MIE engine state. Pre-allocating here (vs lazy
 * heap alloc) eliminates fragmentation risk from the throttled save path
 * that may fire hours into runtime. */
alignas(4) uint8_t s_scratch[kPaddedSize];

} // namespace

extern "C" bool lru_persist_init(void)
{
    /* Scratch is now a static BSS buffer — nothing to do. Kept as an
     * explicit entry point so callers document intent and tests can stub
     * out the init point in future iterations. */
    return true;
}

extern "C" bool lru_persist_load(mie::ImeLogic *ime)
{
    if (!ime) return false;

    const uint8_t *xip = reinterpret_cast<const uint8_t *>(MIE_LRU_PARTITION_ADDR);

    /* First 8 bytes are the LRU1 header. If magic doesn't match, the
     * partition is almost certainly 0xFF (erased flash) — skip the load
     * and leave ImeLogic with an empty cache. deserialize() does the same
     * on any malformed buffer, so we can unconditionally pass the full
     * blob window; it will reject a bad header without touching entries. */
    return ime->load_lru(xip, (int)kBlobMaxBytes);
}

extern "C" bool lru_persist_save(mie::ImeLogic *ime)
{
    if (!ime) return false;

    const int need = ime->lru_serialized_size();
    if (need <= 0 || (size_t)need > kBlobMaxBytes) return false;

    /* Serialise the live cache into the scratch buffer. Pad the tail up
     * to kPaddedSize with 0xFF (post-erase flash state) so the program
     * op writes whole pages without dragging in adjacent sector data. */
    memset(s_scratch, 0xFFu, kPaddedSize);
    const int wrote = ime->serialize_lru(s_scratch, (int)kBlobMaxBytes);
    if (wrote != need) return false;

    /* Erase the active slot (2 × 4 KB sectors) then program the scratch.
     * Both calls go through flash_safety_wrap.c's --wrap so Core 0 is
     * parked and interrupts off on Core 1 for the duration. */
    flash_range_erase(MIE_LRU_SLOT_OFFSET, MIE_LRU_SLOT_SIZE);
    flash_range_program(MIE_LRU_SLOT_OFFSET, s_scratch, kPaddedSize);

    /* Invalidate the XIP cache lines that alias the partition so the
     * next read through the cached XIP window sees the new bytes rather
     * than stale pre-erase content. Range is measured from XIP_BASE.   */
    xip_cache_invalidate_range(MIE_LRU_PARTITION_OFFSET, kPaddedSize);
    return true;
}
