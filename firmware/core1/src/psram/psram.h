/* psram.h -- APS6404L 8 MB QSPI PSRAM driver for Core 1.
 *
 * The stock rpipico2 board header does not initialise PSRAM, and
 * Arduino-Pico's psram.cpp is gated on RP2350_PSRAM_CS which is not
 * defined for this variant (we bypass its TLSF heap anyway and use
 * direct XIP). This module runs the documented Rev A bring-up
 * sequence (see docs/bringup/rev-a-bringup-log.md §9 / bringup_psram.c)
 * directly from Core 1 boot.
 *
 * After psram_init() returns true:
 *   - GPIO0 is bound to XIP_CS1 (FUNCSEL=9)
 *   - QMI M1 is configured for APS6404L QPI at CLKDIV=2 (37.5 MHz)
 *   - PSRAM is mapped at 0x11000000 (cached) / 0x15000000 (uncached)
 *   - XIP_CTRL.WRITABLE_M1 is set so writes to 0x15000000+ land on PSRAM
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSRAM XIP windows (RP2350 QMI M1). */
#define MOKYA_PSRAM_CACHED_BASE    0x11000000u
#define MOKYA_PSRAM_UNCACHED_BASE  0x15000000u
#define MOKYA_PSRAM_SIZE           (8u * 1024u * 1024u)

/* APS6404L device ID first two bytes, as returned by cmd 0x9F. */
#define MOKYA_PSRAM_MFID           0x0Du   /* AP Memory */
#define MOKYA_PSRAM_KGD            0x5Du   /* Known-Good Die marker */

/* One-shot PSRAM initialisation. Must be called before any access to
 * 0x11000000. Safe to call from Core 1 boot (before the FreeRTOS
 * scheduler starts); idempotent in the sense that a second call will
 * reset and re-enter QPI mode, but not needed in normal operation.
 *
 * Returns true when the APS6404L Read-ID response matches the
 * MFID/KGD constants above, indicating the chip is responsive and
 * QMI M1 is correctly configured.
 *
 * Implementation disables interrupts for the duration of the direct-
 * mode QMI transfers (~200 us); safe to call after crt0 runtime init
 * but before any ISR that might access PSRAM.
 */
bool psram_init(void);

/* Last read-ID response bytes (8 bytes from cmd 0x9F). Valid after
 * psram_init() returns; useful for SWD inspection. */
extern volatile uint8_t g_psram_read_id[8];

#ifdef __cplusplus
} /* extern "C" */
#endif
