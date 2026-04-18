/* i2c_bus.h — Core 1 shared I2C bus module.
 *
 * Two physical buses on MokyaLora Rev A:
 *   POWER  = GPIO 6/7   — BQ25622, BQ27441, LM27965
 *   SENSOR = GPIO 34/35 — LSM6DSV16X, LIS2MDL, LPS22HH, Teseo-LIV3FL
 *
 * RP2350 pinmux constraint (see docs/design-notes/mcu-gpio-allocation.md):
 *   Both GPIO pairs can only route to the i2c1 peripheral — GPIO 6/10/14/…/34/…
 *   are I2C1 SDA and GPIO 7/11/…/35/… are I2C1 SCL; no I2C0 alternative exists
 *   on either pair. The two buses therefore share a single SDK peripheral and
 *   are time-multiplexed by switching FUNCSEL between the two pin pairs.
 *
 * This module owns the i2c1 peripheral and serialises access with a FreeRTOS
 * mutex. Drivers call i2c_bus_acquire() to obtain the peripheral handle
 * (which also reconfigures pinmux to the requested pair) and
 * i2c_bus_release() when done.
 *
 * Also: Core 1 skips runtime_init_clocks, so clock_get_hz(clk_peri) returns 0
 * and the SDK's baudrate divisor math silently produces garbage. This module
 * sets the SCL timing registers using the known 150 MHz clk_peri.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_I2C_BUS_H
#define MOKYA_CORE1_I2C_BUS_H

#include "hardware/i2c.h"
#include "FreeRTOS.h"
#include "portmacro.h"

typedef enum {
    MOKYA_I2C_POWER  = 0,   /* GPIO 6/7   — power bus   (1.8 V pull-up rail) */
    MOKYA_I2C_SENSOR = 1,   /* GPIO 34/35 — sensor+GNSS (3.3 V pull-up rail) */
} mokya_i2c_id_t;

/* Boot-time setup: i2c1 peripheral init + baud fix, pull-ups on both pairs,
 * mutex creation. Idempotent. Must be called once from main() before any
 * driver uses i2c_bus_acquire(). Safe to call pre-scheduler. */
void        i2c_bus_init_all(void);

/* Take the bus mutex (blocks up to timeout) and reconfigure FUNCSEL so the
 * requested pin pair is connected to i2c1. Returns the peripheral handle on
 * success, NULL on timeout. Caller MUST pair every successful acquire with
 * a release. */
i2c_inst_t *i2c_bus_acquire(mokya_i2c_id_t id, TickType_t timeout);

/* Release the bus mutex. The active pinmux is left untouched — the next
 * acquire will re-mux as needed. */
void        i2c_bus_release(mokya_i2c_id_t id);

#endif /* MOKYA_CORE1_I2C_BUS_H */
