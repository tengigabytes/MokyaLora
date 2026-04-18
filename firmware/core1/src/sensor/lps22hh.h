/* lps22hh.h — ST LPS22HH MEMS pressure sensor driver (Core 1).
 *
 * Datasheet: DS DocID030890 Rev 2 (February 2019), plus AN5209 application
 * note. On MokyaLora Rev A the device lives on the sensor bus (7-bit addr
 * 0x5D — SA0=high on Rev A hardware, contra datasheet default 0x5C; see
 * docs/bringup/rev-a-bringup-log.md Issue #4). The bus is GPIO 34/35 time-
 * muxed through i2c1, so all access goes via i2c_bus_acquire(SENSOR, ...).
 *
 * Production settings (§9.6, §9.7, §9.9):
 *   ODR      = 1 Hz          (altimeter use; lowest supply current 12 μA)
 *   LPF      = ODR/9         (EN_LPFP=1, LPFP_CFG=0)
 *   LOW_NOISE_EN = 1         (Table 20: RMS noise 0.9 Pa at ODR/9 LPF)
 *   BDU      = 1             (prevents byte tearing on multi-byte reads)
 *   IF_ADD_INC = 1           (multi-byte auto-increment, POR default)
 *   FIFO     = bypass        (no buffering — we poll)
 *
 * The driver exposes a poll function (not a task); the shared sensor_task
 * owns scheduling. See sensor_task.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_LPS22HH_H
#define MOKYA_CORE1_LPS22HH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     online;             /* false → I2C failure or WHO_AM_I mismatch */
    uint32_t pressure_hpa_x100;  /* hPa × 100  (260.00 .. 1260.00 hPa range) */
    int16_t  temperature_cx10;   /* °C × 10                                  */
    uint32_t i2c_fail_count;     /* running count of I2C transaction errors  */
} lps22hh_state_t;

/* Run hardware init: verify WHO_AM_I, SW reset, program CTRL_REG1/2.
 * Returns true on success. On failure the state is left with online=false
 * and the caller (sensor_task) should retry periodically. Caller MUST have
 * the shared I2C bus mutex released — this function takes it itself. */
bool                      lps22hh_init(void);

/* Read pressure + temperature + status, publish into the driver-owned
 * state snapshot. Returns true if a valid sample was captured. */
bool                      lps22hh_poll(void);

/* Stable pointer to the latest snapshot. Fields are updated without
 * locking — individual field tears are tolerated (UI / SWD observers). */
const lps22hh_state_t    *lps22hh_get_state(void);

#endif /* MOKYA_CORE1_LPS22HH_H */
