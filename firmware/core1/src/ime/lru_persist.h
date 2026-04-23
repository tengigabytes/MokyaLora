/* lru_persist.h -- Phase 1.6 LRU persistence (flash load/save).
 *
 * Reads the LruCache blob from MIE_LRU_PARTITION_ADDR at boot, and
 * writes updated blobs back via flash_range_erase + flash_range_program
 * (both wrapped by flash_safety_wrap.c so Core 0 is parked).
 *
 * Called from firmware/core1/src/ime/ime_task.cpp. Callers must hold
 * ImeLogic's snapshot mutex while the save is running — the routine
 * serialises state directly out of the live ImeLogic instance.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
namespace mie { class ImeLogic; }
extern "C" {
#endif

/* Load: reads the LRU blob at MIE_LRU_SLOT_OFFSET via flash XIP and
 * hands it to ime->load_lru(). Returns true on success. On magic / version
 * / length mismatch returns false with the ImeLogic LRU in its default
 * (empty) state — the caller can then continue without persisted data. */
#ifdef __cplusplus
/* Allocate the save-path scratch buffer. Call exactly once at Core 1 boot
 * when the FreeRTOS heap is most contiguous; caller panics if false so
 * throttled saves never silently fail from fragmented heap later. */
bool lru_persist_init(void);

bool lru_persist_load(mie::ImeLogic *ime);
bool lru_persist_save(mie::ImeLogic *ime);
#else
typedef struct ImeLogic ImeLogic;  /* opaque in C */
bool lru_persist_init(void);
bool lru_persist_load(ImeLogic *ime);
bool lru_persist_save(ImeLogic *ime);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
