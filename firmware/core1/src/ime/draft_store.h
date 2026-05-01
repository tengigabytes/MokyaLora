/* draft_store.h — LFS-backed UTF-8 draft persistence for IME text input.
 *
 * Used by `ime_request_text()` to recover an in-progress message after a
 * BACK / reboot / power loss. Each draft is keyed by a caller-defined
 * `draft_id` (e.g. peer node id for A-2 conversation, setting key for
 * Settings text fields). Saving an empty string is equivalent to clear.
 *
 * Phase 6 (Task #70): backing storage migrated from a raw 64 KB flash
 * partition (0x10C10000) to LFS files at /.draft_XXXXXXXX (where
 * XXXXXXXX is the draft_id in lowercase hex). Public API unchanged.
 *
 * The previous fixed-cap eviction policy (16 slots, FIFO) is gone —
 * draft count is now bounded only by available LFS space. Each draft
 * lives in its own file, so concurrent saves to different ids do not
 * conflict (c1_storage's recursive mutex serialises file-level access).
 *
 * Concurrency: in practice only the IME task touches drafts, so the
 * single-caller assumption from the raw-flash version still holds.
 * Reads tolerate missing files (return false).
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

/* Single-draft text capacity (bytes). Kept at the original 4080 for
 * call-site compatibility; LFS imposes no such cap, but ime_text_*
 * callers size their buffers around this value. */
#define DRAFT_STORE_TEXT_MAX  4080u

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
