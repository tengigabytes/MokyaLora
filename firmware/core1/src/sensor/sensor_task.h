/* sensor_task.h — Core 1 unified sensor polling task.
 *
 * Owns the periodic cadence for all sensor-bus peripherals (LPS22HH baro,
 * LIS2MDL mag, LSM6DSV16X IMU). Each individual driver exposes an `_init`
 * / `_poll` pair; this task is the single consumer so that the shared-bus
 * time-mux (see i2c_bus.h) never has more than one sensor pending.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_SENSOR_TASK_H
#define MOKYA_CORE1_SENSOR_TASK_H

#include <stdbool.h>

#include "FreeRTOS.h"
#include "portmacro.h"

/* Create the sensor FreeRTOS task. Safe to call pre-scheduler. */
bool sensor_task_start(UBaseType_t priority);

#endif /* MOKYA_CORE1_SENSOR_TASK_H */
