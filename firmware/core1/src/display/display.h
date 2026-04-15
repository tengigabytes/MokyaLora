/* display.h — Core 1 display driver public API.
 *
 * Initialises the ST7789VI 240×320 IPS panel over the 8080-8 parallel bus
 * (PIO + DMA) and exposes a minimal flush API used by LVGL glue in M3.2.
 *
 * Threading: every function in this header must be called from a single
 * Core 1 FreeRTOS task. The driver does not serialise its own DMA / FIFO
 * accesses across tasks.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DISPLAY_W 240u
#define DISPLAY_H 320u

/* Configure GPIO / PIO / DMA, run the ST7789VI init sequence, and turn the
 * LM27965 backlight on. Returns false if the PIO program could not be loaded
 * or the DMA channel could not be claimed. */
bool display_init(void);

/* Push `pixels` (RGB565, big-endian byte order — i.e. bytes[0]=hi, bytes[1]=lo
 * for COLMOD 0x55) into the rect [x0..x1, y0..y1] inclusive. Blocks until the
 * DMA finishes and the PIO TX FIFO drains. */
void display_flush_rect(uint16_t x0, uint16_t y0,
                        uint16_t x1, uint16_t y1,
                        const uint8_t *pixels);

/* DIRECT-mode flush: the source pixels are the [x0..x1, y0..y1] sub-rect of
 * a larger framebuffer whose row stride is `fb_stride_px` pixels. `fb_base`
 * points to pixel (0, 0) of that framebuffer in native little-endian RGB565
 * (which is what LVGL writes). Each row is byte-swapped through an internal
 * scratch buffer before being DMA'd to the panel, so the framebuffer itself
 * is never mutated. Blocks until the last DMA finishes and the PIO TX FIFO
 * drains. */
void display_flush_rect_strided(uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1,
                                const uint16_t *fb_base,
                                uint16_t fb_stride_px);

/* Block until the next TE rising edge (start of V-blank). M3.1 implements
 * this by polling GPIO 22; M3.2 will switch to GPIO IRQ + task notify. */
void display_wait_te_rise(void);

/* Solid-colour full-screen fill via 2-byte ring DMA — no framebuffer needed.
 * Used by the M3.1 standalone test. */
void display_fill_solid(uint16_t rgb565);
