/* lsm6dsv16x.h — ST LSM6DSV16X 6-axis IMU driver (Core 1).
 *
 * Datasheet: DS13510 Rev 4. On MokyaLora Rev A the device lives on the
 * sensor bus (7-bit addr 0x6A). The bus is GPIO 34/35 time-muxed through
 * i2c1, so all access goes via i2c_bus_acquire(SENSOR, ...).
 *
 * Production settings:
 *   CTRL3 (0x44)     BDU=1, IF_INC=1 (also the post-reset default)
 *   CTRL8 (0x00)     FS_XL = ±2 g       → 0.061 mg/LSB
 *   CTRL6 (0x01)     FS_G  = ±250 dps   → 8.75 mdps/LSB
 *   CTRL1 (0x04)     ODR_XL = 30 Hz (Table 52), OP_MODE_XL = high-performance
 *   CTRL2 (0x04)     ODR_G  = 30 Hz (Table 55), OP_MODE_G  = high-performance
 *
 * The 30 Hz ODR gives the 10 Hz sensor_task tick a fresh sample every poll
 * while keeping BDU latched between reads. Burst 0x20..0x2D (14 bytes) is
 * Temp + Gyro + Accel; unlike LIS2MDL, auto-increment traverses all three
 * output groups cleanly, so a single transaction is enough.
 *
 * The driver exposes a poll function (not a task); the shared sensor_task
 * owns scheduling. See sensor_task.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_LSM6DSV16X_H
#define MOKYA_CORE1_LSM6DSV16X_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     online;             /* false → I2C failure or WHO_AM_I mismatch */
    int16_t  accel_raw[3];       /* raw two's-complement X/Y/Z               */
    int16_t  accel_mg[3];        /* mg = raw * 61 / 1000 (±2 g FS)           */
    int16_t  gyro_raw[3];        /* raw two's-complement X/Y/Z               */
    int16_t  gyro_dps_x10[3];    /* dps × 10 = raw * 875 / 10000 (±250 dps)  */
    int16_t  temperature_cx10;   /* °C × 10 = 250 + raw * 5 / 128
                                  *   (256 LSB/°C, zero point = 25 °C)      */
    uint32_t i2c_fail_count;     /* running count of I2C transaction errors  */
} lsm6dsv16x_state_t;

bool                          lsm6dsv16x_init(void);
bool                          lsm6dsv16x_poll(void);
const lsm6dsv16x_state_t     *lsm6dsv16x_get_state(void);

#endif /* MOKYA_CORE1_LSM6DSV16X_H */
