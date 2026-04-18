/* gps_task.h — Core 1 Teseo-LIV3FL polling task.
 *
 * Separate from sensor_task because the GNSS ODR (1–10 Hz) and drain
 * cadence (100 ms) are both faster than the shared sensor tick, and
 * the NMEA line accumulator keeps parser state between ticks that the
 * unified task would otherwise need to adopt.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_GPS_TASK_H
#define MOKYA_CORE1_GPS_TASK_H

#include <stdbool.h>

#include "FreeRTOS.h"
#include "portmacro.h"

/* Spawn the GNSS task. Safe to call pre-scheduler; all I2C traffic
 * (init restart + periodic drain) runs after the scheduler starts. */
bool gps_task_start(UBaseType_t priority);

#endif /* MOKYA_CORE1_GPS_TASK_H */
