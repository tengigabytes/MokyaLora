/*
 * core1_freertos_test.c — Plan B: FreeRTOS + Manual TinyUSB CDC on Core 1
 *
 * Architecture:
 *   Core 0 — bare-metal launcher, then __wfe() idle forever.
 *   Core 1 — FreeRTOS scheduler + manually-driven TinyUSB CDC.
 *            Bypasses pico_stdio_usb (which hard-asserts Core 0).
 *            USB IRQ registered on Core 1's NVIC; tud_task() polled
 *            by a high-priority FreeRTOS task — same core, no race.
 *
 * USB descriptors provided by pico_stdio_usb's stdio_usb_descriptors.c
 * (linked but stdio_usb_init never called).
 *
 * Expected: COM port appears, heartbeat messages every 2 s.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"

/* ---------------------------------------------------------------------------
 * cdc_write_str — write a string to USB CDC.
 * ---------------------------------------------------------------------------*/
static void cdc_write_str(const char *s)
{
    if (!tud_cdc_connected()) return;

    uint32_t len = strlen(s);
    uint32_t pos = 0;

    while (pos < len) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_cdc_write_flush();
            vTaskDelay(1);
            continue;
        }
        uint32_t chunk = len - pos;
        if (chunk > avail) chunk = avail;
        tud_cdc_write(s + pos, chunk);
        pos += chunk;
    }
    tud_cdc_write_flush();
}

/* ---------------------------------------------------------------------------
 * usb_device_task — high-priority task that polls tud_task().
 * tusb_init() already called before scheduler start.
 * ---------------------------------------------------------------------------*/
static void usb_device_task(void *pv)
{
    (void)pv;
    for (;;) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ---------------------------------------------------------------------------
 * heartbeat_task — infinite heartbeat, writes directly via CDC.
 * ---------------------------------------------------------------------------*/
static void heartbeat_task(void *pv)
{
    (void)pv;

    /* Wait for USB to enumerate. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    char buf[100];
    for (int i = 1; ; i++) {
        snprintf(buf, sizeof(buf),
                 "[Core1-RTOS] HEARTBEAT %d — core %d — tick %lu\r\n",
                 i, (int)get_core_num(),
                 (unsigned long)xTaskGetTickCount());
        cdc_write_str(buf);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ---------------------------------------------------------------------------
 * core1_entry — launched by Core 0 via multicore_launch_core1().
 *
 * 1. tusb_init() on Core 1 stack (before scheduler)
 * 2. Busy-poll tud_task() for 2 s to complete USB enumeration
 * 3. Create FreeRTOS tasks
 * 4. Start scheduler — USB polling continues via usb_device_task
 * ---------------------------------------------------------------------------*/
static void core1_entry(void)
{
    /* Init TinyUSB — registers USBCTRL_IRQ on Core 1's NVIC. */
    tusb_init();

    /* Busy-poll to let USB enumerate before scheduler takes over. */
    for (int i = 0; i < 2000; i++) {
        tud_task();
        busy_wait_ms(1);
    }

    /* Create tasks. */
    xTaskCreate(usb_device_task, "usb", 1024, NULL,
                configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 512, NULL,
                tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();
    panic("FreeRTOS scheduler exited unexpectedly");
}

/* ---------------------------------------------------------------------------
 * main — executed by Core 0.  Launches Core 1 then idles forever.
 * ---------------------------------------------------------------------------*/
int main(void)
{
    multicore_launch_core1(core1_entry);

    while (true)
        __wfe();
}
