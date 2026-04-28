/* draft_store.h — Flash-backed UTF-8 draft persistence for IME text input.
 *
 * Used by `ime_request_text()` to recover an in-progress message after a
 * BACK / reboot / power loss. Each draft is keyed by a caller-defined
 * `draft_id` (e.g. peer node id for A-2 conversation, setting key for
 * Settings text fields). Saving an empty string is equivalent to clear.
 *
 * Flash partition: 0x10C10000 .. 0x10C20000 (64 KB). Reserved adjacent
 * to MIE LRU at 0x10C00000 (see firmware/core1/src/ime/mie_lru_partition.h).
 *
 * Layout: 16 slots × 4 KB. Each slot:
 *   header  uint32_t magic       'DRFT' = 0x54465244 (little-endian)
 *           uint32_t draft_id    caller-defined non-zero id
 *           uint16_t text_len    UTF-8 byte length, 0 .. SLOT_TEXT_MAX
 *           uint16_t reserved
 *           uint32_t crc32       not yet computed (0)
 *   data    text_len bytes UTF-8 (no NUL); padding 0xFF up to slot end.
 *
 * Erased slot has magic == 0xFFFFFFFF — treated as empty.
 *
 * Lookups are linear scan (16 slots). Save: erase + reprogram one
 * 4 KB sector. All flash writes go through the existing
 * `flash_safety_wrap.c` shim so Core 0 is parked (see P2-11).
 *
 * Concurrency: callers must serialise; draft_store assumes single
 * caller per partition (currently only the IME task). Reads tolerate
 * concurrent erased state (will simply return false).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MOKYA_CORE1_DRAFT_STORE_H
#define MOKYA_CORE1_DRAFT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single-slot text capacity (bytes). 4096 - 16 byte header = 4080. */
#define DRAFT_STORE_TEXT_MAX  4080u

/* Number of slots; partition must be SLOT_COUNT × 4 KB. */
#define DRAFT_STORE_SLOT_COUNT 16u

/* No-op call kept for API symmetry with `lru_persist_init`. */
bool draft_store_init(void);

/* Look up `draft_id`, copy its UTF-8 bytes into `buf` (no NUL appended).
 * On success returns true and writes `*out_len` (≤ cap). Returns false
 * if the draft does not exist OR the cap is smaller than the stored
 * length (no truncation; caller must size the buffer or accept loss). */
bool draft_store_load(uint32_t draft_id, char *buf, size_t cap, size_t *out_len);

/* Save `text` (`text_len` bytes UTF-8). If a slot already holds this
 * id, it is overwritten (sector erased + reprogrammed). If the slot
 * does not exist yet, the first empty slot is used; if the partition
 * is full, the least-recently-touched slot is reclaimed (Phase 2 MVP:
 * just the first occupied slot — wear-leveling deferred).
 *
 * `text_len == 0` is equivalent to `draft_store_clear(draft_id)`. */
bool draft_store_save(uint32_t draft_id, const char *text, size_t text_len);

/* Erase the slot holding `draft_id` if present. No-op if not found. */
bool draft_store_clear(uint32_t draft_id);

/* Boot-time round-trip self-test: clear slot 0xC0FFEEDB, save 32 bytes
 * of known content, load it back, compare. Returns true only if the
 * load yields exactly the saved bytes. Slot is cleared at the end so
 * production doesn't carry the test draft. Emits TRACE markers
 * (`drft,test_*`) for SWD-side observability. Safe to call once during
 * Core 1 init after the FreeRTOS scheduler is running. */
bool draft_store_self_test(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif  /* MOKYA_CORE1_DRAFT_STORE_H */
