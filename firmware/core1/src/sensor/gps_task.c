/* gps_task.c — Drives teseo_liv3fl at a fixed 100 ms drain cadence.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gps_task.h"

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "teseo_liv3fl.h"

#define DRAIN_PERIOD_MS  100u
#define INIT_RETRY_MS    500u

static void gps_task(void *pv)
{
    (void)pv;

    /* Retry init forever — a late-plug or transient bus stall during
     * boot shouldn't permanently disable the task. */
    while (!teseo_init()) {
        vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_MS));
    }

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        (void)teseo_poll();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(DRAIN_PERIOD_MS));
    }
}

bool gps_task_start(UBaseType_t priority)
{
    /* 512 words (2 KB). Drain buffer and line accumulator are static
     * (no stack cost). NMEA parsing calls atof/strtoul which draw a
     * ~400-byte libc frame — still comfortably within budget. */
    BaseType_t rc = xTaskCreate(gps_task, "gps", 512, NULL, priority, NULL);
    return rc == pdPASS;
}
