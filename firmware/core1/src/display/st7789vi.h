/* st7789vi.h — ST7789VI panel-specific opcodes and init sequence.
 *
 * Internal to the display driver: callers should use display.h, not this.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

/* User commands actually emitted by st7789vi_init / window / write paths.
 * Names follow ST7789VI datasheet V1.3 section 9.1. */
#define ST7789_SWRESET    0x01
#define ST7789_SLPOUT     0x11
#define ST7789_INVON      0x21
#define ST7789_DISPON     0x29
#define ST7789_CASET      0x2A
#define ST7789_RASET      0x2B
#define ST7789_RAMWR      0x2C
#define ST7789_TEON       0x35
#define ST7789_MADCTL     0x36
#define ST7789_COLMOD     0x3A
#define ST7789_PORCTRL    0xB2
#define ST7789_GCTRL      0xB7
#define ST7789_VCOMS      0xBB
#define ST7789_LCMCTRL    0xC0
#define ST7789_VDVVRHEN   0xC2
#define ST7789_VRHS       0xC3
#define ST7789_VDVS       0xC4
#define ST7789_FRCTRL2    0xC6
#define ST7789_PWCTRL1    0xD0
#define ST7789_PVGAMCTRL  0xE0
#define ST7789_NVGAMCTRL  0xE1

/* Caller contract: the display driver has already
 *   - asserted nCS LOW,
 *   - finished hardware reset (nRST pulse),
 *   - configured PIO + DMA so that st7789_send_cmd() / st7789_send_data() work.
 *
 * st7789_init runs the full power-up + gamma + window + DISPON sequence and
 * leaves DCX = 1 (data phase) ready for RAMWR streaming.
 */
typedef void (*st7789_send_cmd_fn)(uint8_t cmd);
typedef void (*st7789_send_data_fn)(uint8_t data);

void st7789_init(st7789_send_cmd_fn send_cmd, st7789_send_data_fn send_data);
