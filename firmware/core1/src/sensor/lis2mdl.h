/* lis2mdl.h — ST LIS2MDL 3-axis magnetometer driver (Core 1).
 *
 * Datasheet: DS12144 Rev 6. On MokyaLora Rev A the device lives on the
 * sensor bus (7-bit addr 0x1E = 0011110b per §6.1.1). The bus is GPIO
 * 34/35 time-muxed through i2c1, so all access goes via
 * i2c_bus_acquire(SENSOR, ...).
 *
 * Production settings (§5.3, §8.5-§8.7):
 *   ODR             = 10 Hz           (lowest available; matches master tick)
 *   MD              = continuous
 *   LP              = 0               (high-resolution mode — magnetic RMS
 *                                      noise 3 mgauss per §2.1 Table 2)
 *   COMP_TEMP_EN    = 1               (required for correct operation —
 *                                      §8.5 footnote 1)
 *   OFF_CANC        = 1               (offset cancellation — §8.6)
 *   LPF             = 1               (digital LPF at ODR/4 — §8.6 Table 28)
 *   BDU             = 1               (prevents byte tearing on multi-byte
 *                                      reads — §8.7)
 *
 * The driver exposes a poll function (not a task); the shared sensor_task
 * owns scheduling. See sensor_task.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_LIS2MDL_H
#define MOKYA_CORE1_LIS2MDL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     online;             /* false → I2C failure or WHO_AM_I mismatch */
    int16_t  mag_raw[3];         /* raw two's-complement X/Y/Z              */
    int16_t  mag_ut_x10[3];      /* µT × 10  (1.5 mgauss/LSB = 0.15 µT/LSB) */
    int16_t  temperature_cx10;   /* °C × 10. Zero point = 25 °C per ST HAL
                                  * (raw/8 + 25) — datasheet Table 3 lists
                                  * only sensitivity.                       */
    uint32_t i2c_fail_count;     /* running count of I2C transaction errors */
} lis2mdl_state_t;

/* Run hardware init: verify WHO_AM_I, SW reset, program CFG_REG_A/B/C.
 * Returns true on success. On failure the state is left with online=false
 * and the caller (sensor_task) should retry periodically. Caller MUST have
 * the shared I2C bus mutex released — this function takes it itself. */
bool                      lis2mdl_init(void);

/* Read X/Y/Z + temperature as a single burst, publish into the driver-owned
 * state snapshot. Returns true if a valid sample was captured. */
bool                      lis2mdl_poll(void);

/* Stable pointer to the latest snapshot. Fields are updated without
 * locking — individual field tears are tolerated (UI / SWD observers). */
const lis2mdl_state_t    *lis2mdl_get_state(void);

#endif /* MOKYA_CORE1_LIS2MDL_H */
