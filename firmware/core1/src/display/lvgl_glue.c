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

#include "lvgl.h"
#include "demos/lv_demos.h"

#include "FreeRTOS.h"
#include "task.h"

#include "pico/time.h"   /* time_us_32 for flush duration measurement */

/* Run lv_demo_benchmark() instead of the colour-cycle smoke test. Compile
 * with -DMOKYA_LVGL_BENCHMARK=0 to go back to the red/green/blue cycle.  */
#ifndef MOKYA_LVGL_BENCHMARK
#define MOKYA_LVGL_BENCHMARK 1
#endif

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

/* ── flush callback ──────────────────────────────────────────────────────── *
 * Flush counter, written to a SWD-readable slot so we can observe (a) that
 * flush_cb is firing, (b) that each call returns past the blocking
 * display_flush_rect_strided. Layout: 0xF1 [enter|exit] [counter].
 * 0x2007FFE0..0x2007FFEC is owned by Core 0 ipc_serial_stub debug
 * counters; we use 0x2007FFF0 which lives in the _tail_pad of the shared
 * IPC region.                                                                 */
#define LVGL_FLUSH_BREADCRUMB_ADDR 0x2007FFF0u
/* Duration of the most recent flush in microseconds. Lives at 0x2007FFD4,
 * one of the two free slots between the bridge breadcrumbs (0x2007FFC0..D0)
 * and the LVGL task breadcrumb (0x2007FFD8). */
#define LVGL_FLUSH_US_LAST_ADDR   0x2007FFD4u

static volatile uint32_t s_flush_count;

static void lvgl_flush_cb(lv_display_t *disp,
                          const lv_area_t *area,
                          uint8_t *px_map)
{
    s_flush_count++;
    *(volatile uint32_t *)LVGL_FLUSH_BREADCRUMB_ADDR =
        0xF1E10000u | (s_flush_count & 0xFFFFu);    /* enter */

    const uint32_t t0 = time_us_32();

    /* DIRECT mode: px_map always equals the framebuffer base; `area` is
     * the dirty rect inside it. The strided helper handles byte-swap
     * + row-by-row DMA without touching the framebuffer. */
    display_flush_rect_strided((uint16_t)area->x1, (uint16_t)area->y1,
                               (uint16_t)area->x2, (uint16_t)area->y2,
                               (const uint16_t *)px_map,
                               (uint16_t)DISPLAY_W);

    *(volatile uint32_t *)LVGL_FLUSH_US_LAST_ADDR = time_us_32() - t0;
    *(volatile uint32_t *)LVGL_FLUSH_BREADCRUMB_ADDR =
        0xF1F10000u | (s_flush_count & 0xFFFFu);    /* finished */

    lv_display_flush_ready(disp);
}

/* ── Smoke-test screen colour rotation ──────────────────────────────────── *
 * Flips the active screen's background colour every second. This exercises
 * the full LVGL -> flush_cb -> panel path; M3.3 replaces it with widgets.  */
static const lv_color_t s_smoke_colours[] = {
    LV_COLOR_MAKE(0xFF, 0x00, 0x00), /* red   */
    LV_COLOR_MAKE(0x00, 0xFF, 0x00), /* green */
    LV_COLOR_MAKE(0x00, 0x00, 0xFF), /* blue  */
};

/* SWD-readable breadcrumb so we can confirm the LVGL task reached each
 * phase even before any pixels reach the panel.                            */
#define LVGL_BREADCRUMB_ADDR 0x2007FFD8u

static inline void lvgl_breadcrumb(uint32_t v)
{
    *(volatile uint32_t *)LVGL_BREADCRUMB_ADDR = v;
}

/* ── LVGL service task ──────────────────────────────────────────────────── */
static void lvgl_task(void *arg)
{
    (void)arg;

    lvgl_breadcrumb(0x16C10000u);  /* task entered */

    if (!display_init()) {
        lvgl_breadcrumb(0xDEAD16C1u);
        vTaskDelete(NULL);
        return;
    }
    lvgl_breadcrumb(0x16C10001u);  /* display up */

    lv_init();
    lv_tick_set_cb(lvgl_tick_get_ms);

    lv_display_t *disp = lv_display_create(DISPLAY_W, DISPLAY_H);
    if (disp == NULL) {
        lvgl_breadcrumb(0xDEAD16C2u);
        vTaskDelete(NULL);
        return;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp,
                           s_framebuffer, NULL,
                           FB_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lvgl_breadcrumb(0x16C10002u);  /* display registered */

#if MOKYA_LVGL_BENCHMARK
    /* Benchmark mode: let lv_demo_benchmark() build its own widget tree
     * and cycle through render/blend/image scenes. Perf monitor overlay
     * reports FPS in the bottom-right corner. */
    lv_demo_benchmark();
    lvgl_breadcrumb(0x16C1BE1Cu);   /* benchmark launched */
#else
    /* Colour-cycle smoke test. */
    lv_obj_set_style_bg_color(lv_screen_active(), s_smoke_colours[0],
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, LV_PART_MAIN);
    lvgl_breadcrumb(0x16C10003u);   /* initial screen painted */
    uint32_t colour_idx = 0;
    TickType_t last_swap = xTaskGetTickCount();
#endif

    for (;;) {
#if !MOKYA_LVGL_BENCHMARK
        if ((xTaskGetTickCount() - last_swap) >= pdMS_TO_TICKS(1000)) {
            colour_idx = (colour_idx + 1u) %
                         (sizeof(s_smoke_colours) / sizeof(s_smoke_colours[0]));
            lv_obj_set_style_bg_color(lv_screen_active(),
                                      s_smoke_colours[colour_idx],
                                      LV_PART_MAIN);
            last_swap = xTaskGetTickCount();
        }
#endif

        uint32_t next = lv_timer_handler();
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
    /* Stamp the flush breadcrumb with a known canary BEFORE the task runs,
     * so we can tell over SWD whether the address is writable at all (vs.
     * the task simply hasn't called flush_cb yet). */
    *(volatile uint32_t *)LVGL_FLUSH_BREADCRUMB_ADDR = 0xCA1FB10Du;

    /* 16 KB stack (= 4096 words). LVGL SW render recurses through widget
     * + draw layers; DIRECT mode keeps per-flush locals modest. */
    return xTaskCreate(lvgl_task, "lvgl", 4096, NULL, priority, NULL);
}
