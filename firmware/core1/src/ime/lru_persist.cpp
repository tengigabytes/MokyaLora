/* lru_persist.cpp -- See lru_persist.h for contract.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lru_persist.h"
#include "mie_lru_partition.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"          /* pvPortMalloc / vPortFree */

#include "hardware/flash.h"
#include "hardware/xip_cache.h"

#include <mie/ime_logic.h>

namespace {

/* Header + max entries. 128 × 48 B + 8 B header = 6152 B. 4-byte aligned
 * for flash_range_program. Rounded up to the flash page size (256 B) so
 * the entire program call hits whole pages — 25 pages = 6400 B.
 *
 * Phase 1.6.1 (2026-04-26): kCap bumped 64 → 128. To make BSS room for
 * the +3 KB LruCache growth in g_ime_storage, the scratch buffer was
 * moved off BSS and is now allocated on the FreeRTOS heap inside
 * lru_persist_save() and freed on return. Cost analysis:
 *   - Save fires only on commit-50 / mode-cycle / 30 s-idle tripwire,
 *     so 6.4 KB heap pressure is rare and short-lived.
 *   - Boot heap free ~9.3 KB; one 6.4 KB malloc takes free to ~2.9 KB
 *     transiently, then snaps back. Below the 15 % boot panic threshold
 *     but above runtime starvation.
 *   - pvPortMalloc returning NULL = save skipped (returns false); the
 *     next tripwire retries. No data loss; entries persist in RAM.
 */
constexpr size_t kBlobMaxBytes = 8u + (size_t)mie::LruCache::kCap * 48u;
constexpr size_t kPageSize     = FLASH_PAGE_SIZE;    /* 256 B */
constexpr size_t kPaddedSize   = ((kBlobMaxBytes + kPageSize - 1u)
                                  / kPageSize) * kPageSize;
static_assert(kPaddedSize <= MIE_LRU_SLOT_SIZE,
              "LRU blob must fit in one slot");

} // namespace

extern "C" bool lru_persist_init(void)
{
    /* Heap-allocated scratch is acquired lazily inside lru_persist_save.
     * Kept as an explicit entry point so callers document intent and
     * tests can stub out the init point in future iterations. */
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

    /* Allocate the scratch buffer from the FreeRTOS heap. Freed before
     * any non-error return path. NULL return means we couldn't get the
     * 6.4 KB right now — the next tripwire (commit-50 / mode / idle)
     * will retry and the in-RAM LRU is unchanged. */
    uint8_t *scratch = (uint8_t *)pvPortMalloc(kPaddedSize);
    if (!scratch) return false;

    /* Serialise the live cache into the scratch buffer. Pad the tail up
     * to kPaddedSize with 0xFF (post-erase flash state) so the program
     * op writes whole pages without dragging in adjacent sector data. */
    memset(scratch, 0xFFu, kPaddedSize);
    const int wrote = ime->serialize_lru(scratch, (int)kBlobMaxBytes);
    if (wrote != need) {
        vPortFree(scratch);
        return false;
    }

    /* Erase the active slot (2 × 4 KB sectors) then program the scratch.
     * Both calls go through flash_safety_wrap.c's --wrap so Core 0 is
     * parked and interrupts off on Core 1 for the duration. */
    flash_range_erase(MIE_LRU_SLOT_OFFSET, MIE_LRU_SLOT_SIZE);
    flash_range_program(MIE_LRU_SLOT_OFFSET, scratch, kPaddedSize);

    vPortFree(scratch);

    /* Invalidate the XIP cache lines that alias the partition so the
     * next read through the cached XIP window sees the new bytes rather
     * than stale pre-erase content. Range is measured from XIP_BASE.   */
    xip_cache_invalidate_range(MIE_LRU_PARTITION_OFFSET, kPaddedSize);
    return true;
}
