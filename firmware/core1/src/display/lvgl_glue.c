/* lvgl_glue.c — LVGL v9.2.2 wired to the ST7789VI display driver in
 * DIRECT mode with a full 240x320 RGB565 framebuffer.
 *
 * Per firmware-architecture.md §2.2 / §4.3 the architecture calls for a
 * single 150 KB framebuffer in SRAM and LVGL configured in
 * LV_DISPLAY_RENDER_MODE_DIRECT: LVGL keeps the buffer between frames,
 * redraws only dirty rectangles, and each dirty rect is pushed to the
 * panel via DMA. No partial-buffer re-renders, no per-flush full-frame
 * swaps.
 *
 * Framebuffer placement. 240x320x2 = 153 600 B = 150 KB, placed in the
 * `.framebuffer` NOLOAD section at the start of Core 1's 312 KB SRAM
 * carve-out (memmap_core1_bridge.ld). Native little-endian RGB565 — this
 * is what LVGL writes and what `lv_obj_*` / compositor code expects to
 * read back for alpha blending etc.
 *
 * flush_cb. In DIRECT mode `px_map` always points to the START of the
 * framebuffer and `area` is the dirty rect. We call
 * display_flush_rect_strided() which row-by-row byte-swaps each row into
 * an internal scratch buffer and DMA's it to the panel — the framebuffer
 * itself is never mutated. display_flush_rect_strided blocks until DMA +
 * PIO FIFO drain so we can signal lv_display_flush_ready() synchronously;
 * async DMA + TE-IRQ tearing avoidance is M3.3 scope.
 *
 * Tick source: xTaskGetTickCount(). configTICK_RATE_HZ is 1000 so the
 * value is already in ms.
 *
 * Smoke test: the task body cycles the active screen background colour
 * red -> green -> blue at 1 Hz; M3.3 replaces it with real widgets.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lvgl_glue.h"
#include "display.h"
#include "keypad_view.h"
#include "lm27965.h"

#include "lvgl.h"

#include "FreeRTOS.h"
#include "task.h"

/* ── Framebuffer ─────────────────────────────────────────────────────────── *
 * 240 x 320 x 2 B/px = 153 600 B = exactly 150 KB. Lives in the dedicated
 * `.framebuffer` NOLOAD section so it does NOT consume the 48 KB lv_mem
 * heap, the 32 KB FreeRTOS Heap4, or BSS. 4-byte alignment matches the
 * DMA + PIO transfer width.                                                  */
#define FB_PIXELS           ((uint32_t)DISPLAY_W * (uint32_t)DISPLAY_H)
#define FB_BYTES            (FB_PIXELS * 2u)

static uint16_t __attribute__((section(".framebuffer"), aligned(4)))
    s_framebuffer[FB_PIXELS];

/* ── Tick source ─────────────────────────────────────────────────────────── */
static uint32_t lvgl_tick_get_ms(void)
{
    /* configTICK_RATE_HZ == 1000 — TickType_t is already milliseconds. */
    return (uint32_t)xTaskGetTickCount();
}

/* ── flush callback ──────────────────────────────────────────────────────── */
static void lvgl_flush_cb(lv_display_t *disp,
                          const lv_area_t *area,
                          uint8_t *px_map)
{
    /* DIRECT mode: px_map always equals the framebuffer base; `area` is
     * the dirty rect inside it. The strided helper handles byte-swap
     * + row-by-row DMA without touching the framebuffer. */
    display_flush_rect_strided((uint16_t)area->x1, (uint16_t)area->y1,
                               (uint16_t)area->x2, (uint16_t)area->y2,
                               (const uint16_t *)px_map,
                               (uint16_t)DISPLAY_W);

    lv_display_flush_ready(disp);
}

/* ── LVGL service task ──────────────────────────────────────────────────── */
static void lvgl_task(void *arg)
{
    (void)arg;

    if (!display_init()) {
        vTaskDelete(NULL);
        return;
    }

    /* Panel is up; bring backlight on. Duty code 0x16 ≈ 40 % — the boot
     * default inherited from Rev A bringup. UI can change later via
     * lm27965_set_tft_backlight(). */
    (void)lm27965_init(0x16);

    lv_init();
    lv_tick_set_cb(lvgl_tick_get_ms);

    lv_display_t *disp = lv_display_create(DISPLAY_W, DISPLAY_H);
    if (disp == NULL) {
        vTaskDelete(NULL);
        return;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp,
                           s_framebuffer, NULL,
                           FB_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    /* Phase C diagnostic view — 6×6 grid driven by the KeyEvent queue. */
    keypad_view_init(lv_screen_active());

    for (;;) {
        uint32_t next = lv_timer_handler();
        keypad_view_tick();
        if (next == LV_NO_TIMER_READY || next > 100u) {
            next = 100u;
        }
        if (next < (uint32_t)LV_DEF_REFR_PERIOD) {
            next = (uint32_t)LV_DEF_REFR_PERIOD;
        }
        vTaskDelay(pdMS_TO_TICKS(next));
    }
}

BaseType_t lvgl_glue_start(UBaseType_t priority)
{
    /* 12 KB stack (= 3072 words). LVGL SW render recurses through widget
     * + draw layers; DIRECT mode keeps per-flush locals modest. 16 KB
     * was the Phase 3.2 default before keypad_probe_task competed for
     * the 32 KB FreeRTOS heap; benchmark + flush path is well under
     * 12 KB of actual stack use. Bump back up if HighWaterMark ever
     * drops close to the limit. */
    return xTaskCreate(lvgl_task, "lvgl", 3072, NULL, priority, NULL);
}
