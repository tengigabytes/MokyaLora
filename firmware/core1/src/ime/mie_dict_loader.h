/* mie_dict_loader.h -- Copy the MDBL dict blob from flash to PSRAM.
 *
 * The blob is flashed as an independent partition at
 * MIE_DICT_PARTITION_ADDR by scripts/build_and_flash.sh. This loader
 * reads the TOC at boot (via flash XIP), validates magic/version, and
 * memcpy's the four sections into PSRAM. Runtime MIE code reads the
 * sections from PSRAM for faster random access than flash XIP.
 *
 * PSRAM layout (cached base 0x11000000):
 *   0x11000000  zh_dat  (dict_dat.bin)
 *   0x11200000  zh_val  (dict_values.bin)
 *   0x11500000  en_dat  (en_dat.bin)
 *   0x11501000  en_val  (en_values.bin)
 *   0x11600000  free (for LVGL frame buffers etc.)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pointer pack returned by the loader. All pointers are into PSRAM at
 * the cached base; sizes are zero for sections the blob omitted. */
typedef struct {
    const uint8_t *zh_dat; size_t zh_dat_size;
    const uint8_t *zh_val; size_t zh_val_size;
    const uint8_t *en_dat; size_t en_dat_size;
    const uint8_t *en_val; size_t en_val_size;
} mie_dict_pointers_t;

/* Outcome of a load attempt. NONE = not tried; OK = all sections landed
 * in PSRAM and the pointers are valid. The error codes pinpoint which
 * check failed so SWD inspection can distinguish a missing blob from a
 * format mismatch. */
typedef enum {
    MIE_DICT_LOAD_NONE          = 0,
    MIE_DICT_LOAD_OK            = 1,
    MIE_DICT_LOAD_ERR_MAGIC     = 2,
    MIE_DICT_LOAD_ERR_VERSION   = 3,
    MIE_DICT_LOAD_ERR_GEOMETRY  = 4,  /* section offset/size exceeds partition */
    MIE_DICT_LOAD_ERR_NO_PSRAM  = 5,  /* psram_init reported failure */
} mie_dict_load_status_t;

/* Copy the MDBL blob from MIE_DICT_PARTITION_ADDR (flash XIP) to
 * PSRAM, populate *out with pointers to the four sections, and
 * return true on success. On failure *out is zeroed and the status
 * register g_mie_dict_load_status records the reason.
 *
 * Requires psram_init() to have succeeded first.
 *
 * Writes go through the UNCACHED alias (0x15000000) so they land on
 * PSRAM directly without cache bookkeeping. The cache is then
 * invalidated by address range so subsequent reads via the cached
 * alias (0x11000000) miss-and-fill from the freshly-written PSRAM
 * rather than from pre-load stale lines. Total copy ~5 MB takes
 * ~160 ms at 75 MHz QPI (~31 MB/s via uncached) plus flash-read time.
 */
bool mie_dict_load_to_psram(mie_dict_pointers_t *out);

/* SWD-observable load status (from the enum above). */
extern volatile uint32_t g_mie_dict_load_status;

#ifdef __cplusplus
} /* extern "C" */
#endif
