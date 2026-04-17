/*
 * Copyright 2026 MokyaLora Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico SDK board header for the MokyaLora Rev A hardware.
 *
 * MokyaLora Rev A uses the RP2350B in QFN-80 (48 GPIOs) paired with
 * an APS6404L 8 MB QSPI PSRAM and a W25Q128JW 16 MB flash.  The stock
 * Pico SDK `pico2` board header targets RP2350A (30 GPIOs), which makes
 * `check_gpio_param` silently reject any of our column/row pins
 * (GPIO 36-47) and leaves the matching pad registers in their boot-time
 * ISO=1 / IE=0 state.  A dedicated board header is the minimum needed
 * to declare the correct part variant so `NUM_BANK0_GPIOS` resolves to
 * 48 everywhere in the SDK.
 *
 * The few other fields in this header mirror the defaults that the
 * `pico2` header would have set.  Arduino-Pico / Core 0 still uses the
 * `rp2350_base` variant, not this header — so the power-of-two RAM /
 * flash sizes below only influence the Core 1 build.
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_MOKYA_REV_A_H
#define _BOARDS_MOKYA_REV_A_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection within the firmware
#define MOKYA_REV_A

// RP2350B — 48 GPIOs.  This is the one line that actually differs from
// the pico2 board header.
#define PICO_RP2350A 0

// A2 silicon revision is what ships on Rev A.
pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

// Flash: W25Q128JW, 16 MB.  Boot stage 2 generic-W25Q has worked on
// all bring-up builds so far and matches what pico2 would pick.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#endif
