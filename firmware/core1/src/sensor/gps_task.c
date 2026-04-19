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

/* TEMP commissioning — flip via `#define MOKYA_COMMISSION_RF_DEBUG` just
 * below and reflash ONCE; Teseo NVM persists the mask across boots. */
/* #define MOKYA_COMMISSION_RF_DEBUG 1 */
#ifdef MOKYA_COMMISSION_RF_DEBUG
volatile uint32_t g_rf_commission_rc;
#endif

static void gps_task(void *pv)
{
    (void)pv;

    /* Retry init forever — a late-plug or transient bus stall during
     * boot shouldn't permanently disable the task. */
    while (!teseo_init()) {
        vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_MS));
    }

    /* TEMP one-shot RF-debug commissioning. When enabled at build time,
     * writes CDB 231 to switch on $PSTMRF / $PSTMNOISE / $PSTMNOTCHSTATUS /
     * $PSTMCPU / $GPGST permanently (NVM). Remove after one successful
     * boot so subsequent boots don't re-save NVM. */
#ifdef MOKYA_COMMISSION_RF_DEBUG
    extern volatile uint32_t g_rf_commission_rc;
    vTaskDelay(pdMS_TO_TICKS(2000));
    g_rf_commission_rc = teseo_enable_rf_debug_messages(true) ? 1u : 0u;
#endif

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
