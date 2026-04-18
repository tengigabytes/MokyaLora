/* sensor_task.c — single-task scheduler for all sensor-bus drivers.
 *
 * M3.4.5a carries only LPS22HH (1 Hz). As LIS2MDL (M3.4.5b) and
 * LSM6DSV16X (M3.4.5c) land, they slot into the same tick loop with
 * divider counters so each runs at its own ODR without creating extra
 * tasks. The bus is time-muxed i2c1 (see i2c_bus.h); concurrent access
 * from multiple tasks would just serialise on the mutex anyway.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sensor_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lis2mdl.h"
#include "lps22hh.h"

#define TICK_PERIOD_MS       100u     /* 10 Hz master tick */
#define LPS22HH_PERIOD_TICKS 10u      /* 1 Hz poll                        */
#define LIS2MDL_PERIOD_TICKS 1u       /* 10 Hz poll (matches device ODR)  */
#define INIT_RETRY_MS        500u

static void sensor_task(void *pv)
{
    (void)pv;

    /* Init each sensor, retrying forever so a late-plug or brown-out
     * recovery brings the driver online without host intervention. Each
     * retry pauses briefly so the bus isn't hammered. */
    while (!lps22hh_init()) {
        vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_MS));
    }
    while (!lis2mdl_init()) {
        vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_MS));
    }

    TickType_t last = xTaskGetTickCount();
    uint32_t   tick = 0;
    for (;;) {
        if ((tick % LPS22HH_PERIOD_TICKS) == 0) {
            (void)lps22hh_poll();
        }
        if ((tick % LIS2MDL_PERIOD_TICKS) == 0) {
            (void)lis2mdl_poll();
        }
        tick++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_PERIOD_MS));
    }
}

bool sensor_task_start(UBaseType_t priority)
{
    /* 512 words (2 KB): biggest locals are the 5-byte baro burst and i2c
     * SDK call frames; headroom for future IMU 12-byte burst + SFLP
     * quaternion decode. */
    BaseType_t rc = xTaskCreate(sensor_task, "sens", 512, NULL, priority, NULL);
    return rc == pdPASS;
}
