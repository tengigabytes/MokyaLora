/* lvgl_glue.c — LVGL v9.2.2 wired to the ST7789VI display driver.
 *
 * M3.2 first cut: partial render mode with a single 240×40 RGB565 draw buffer
 * (19.2 KB, BSS static). flush_cb byte-swaps RGB565 in place because LVGL
 * renders little-endian and the panel under COLMOD 0x55 expects big-endian
 * (high byte first), then ships the rect via display_flush_rect() which
 * blocks until DMA + PIO TX FIFO drain. lv_display_flush_ready() is signalled
 * synchronously — there is no DMA-completion notification yet (M3.3 will add
 * it together with TE-IRQ tearing avoidance).
 *
 * Tick source: xTaskGetTickCount(). configTICK_RATE_HZ is 1000 so the value
 * is already in ms — exactly what lv_tick_set_cb expects.
 *
 * Smoke test: the task body cycles the active screen background colour
 * red → green → blue at 1 Hz so we can eyeball the full LVGL → flush_cb →
 * panel path end-to-end. M3.3 replaces this with real widgets.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lvgl_glue.h"
#include "display.h"

#include "lvgl.h"

#include "FreeRTOS.h"
#include "task.h"

/* ── Draw buffer ─────────────────────────────────────────────────────────── *
 * 240 × 40 lines × 2 B/px = 19 200 B. Lives in BSS so it does NOT consume
 * the 32 KB FreeRTOS heap nor the 48 KB LVGL internal heap. The 4-byte
 * alignment matches the DMA + PIO transfer width.                            */
#define LVGL_BUF_LINES      40u
#define LVGL_BUF_PIXELS     ((uint32_t)DISPLAY_W * LVGL_BUF_LINES)
#define LVGL_BUF_BYTES      (LVGL_BUF_PIXELS * 2u)

static uint8_t __attribute__((aligned(4))) s_lvgl_draw_buf[LVGL_BUF_BYTES];

/* ── Tick source ─────────────────────────────────────────────────────────── */
static uint32_t lvgl_tick_get_ms(void)
{
    /* configTICK_RATE_HZ == 1000 — TickType_t is already milliseconds. */
    return (uint32_t)xTaskGetTickCount();
}

/* ── flush callback ──────────────────────────────────────────────────────── */
/* Flush counter, written to a separate SWD-readable slot so we can tell
 * (a) whether flush_cb is being called at all, (b) whether each call returns
 * past the blocking display_flush_rect. Layout: 0xF1 [enter|exit] [counter].
 * 0x2007FFE0..0x2007FFEC is owned by Core 0 ipc_serial_stub debug counters
 * (see firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/ipc_serial_stub.cpp);
 * we use 0x2007FFF0 onwards which is currently free in _tail_pad. */
#define LVGL_FLUSH_BREADCRUMB_ADDR 0x2007FFF0u

static volatile uint32_t s_flush_count;

static void lvgl_flush_cb(lv_display_t *disp,
                          const lv_area_t *area,
                          uint8_t *px_map)
{
    s_flush_count++;
    *(volatile uint32_t *)LVGL_FLUSH_BREADCRUMB_ADDR =
        0xF1E10000u | (s_flush_count & 0xFFFFu);    /* enter */

    /* LVGL renders RGB565 little-endian (low byte first); the panel under
     * COLMOD 0x55 expects high byte first. Swap in place. */
    const uint32_t pixel_count =
        (uint32_t)(area->x2 - area->x1 + 1) *
        (uint32_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, pixel_count);

    display_flush_rect((uint16_t)area->x1, (uint16_t)area->y1,
                       (uint16_t)area->x2, (uint16_t)area->y2,
                       px_map);

    *(volatile uint32_t *)LVGL_FLUSH_BREADCRUMB_ADDR =
        0xF1F10000u | (s_flush_count & 0xFFFFu);    /* finished */

    /* display_flush_rect blocks until DMA + PIO drain, so the buffer is
     * free to be reused immediately. */
    lv_display_flush_ready(disp);
}

/* ── Smoke-test screen colour rotation ──────────────────────────────────── *
 * Flips the active screen's background colour every second. This is the
 * minimal LVGL workload that exercises the full render → flush path; M3.3
 * replaces it with widgets.                                                  */
static const lv_color_t s_smoke_colours[] = {
    LV_COLOR_MAKE(0xFF, 0x00, 0x00), /* red   */
    LV_COLOR_MAKE(0x00, 0xFF, 0x00), /* green */
    LV_COLOR_MAKE(0x00, 0x00, 0xFF), /* blue  */
};

/* SWD-readable breadcrumb so we can confirm the LVGL task actually entered
 * each phase even before any pixels reach the panel. Lives in the same
 * 0x2007FFD8 slot the old display_test_task used.                            */
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
                           s_lvgl_draw_buf, NULL,
                           LVGL_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lvgl_breadcrumb(0x16C10002u);  /* display registered */

    /* Paint the initial colour so the smoke test starts with a known frame. */
    lv_obj_set_style_bg_color(lv_screen_active(), s_smoke_colours[0],
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, LV_PART_MAIN);
    lvgl_breadcrumb(0x16C10003u);   /* initial screen painted */

    uint32_t colour_idx = 0;
    TickType_t last_swap = xTaskGetTickCount();

    for (;;) {
        /* Rotate the background colour every second so we can eyeball the
         * full LVGL → flush_cb → panel path end-to-end. */
        if ((xTaskGetTickCount() - last_swap) >= pdMS_TO_TICKS(1000)) {
            colour_idx = (colour_idx + 1u) %
                         (sizeof(s_smoke_colours) / sizeof(s_smoke_colours[0]));
            lv_obj_set_style_bg_color(lv_screen_active(),
                                      s_smoke_colours[colour_idx],
                                      LV_PART_MAIN);
            last_swap = xTaskGetTickCount();
        }

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
     * so we can tell at SWD whether the address is writable at all (vs. the
     * task simply hasn't called flush_cb yet). */
    *(volatile uint32_t *)LVGL_FLUSH_BREADCRUMB_ADDR = 0xCA1FB10Du;

    /* 16 KB stack (= 4096 words). LVGL SW render recurses through widget
     * + draw layers; partial-mode flush adds a few hundred bytes of locals. */
    return xTaskCreate(lvgl_task, "lvgl", 4096, NULL, priority, NULL);
}
