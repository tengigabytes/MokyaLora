/* bq27441.h — TI BQ27441-G1 fuel gauge driver (Core 1).
 *
 * STUB — not implemented on Rev A. The Rev A hardware has two known defects
 * that make this device unusable in production (see
 * docs/bringup/rev-a-bringup-log.md, Issues #9 and #10):
 *
 *   #9  BIN pin left floating on the PCB → BIE default = 1 (hardware
 *       battery-insertion detection), so BAT_DET never asserts and INITCOMP
 *       remains stuck at 0. A CONFIG UPDATE clearing BIE plus a manual
 *       BAT_INSERT subcommand restores operation, but the workaround is
 *       fragile.
 *   #10 Cold-boot I2C NACK — after any power-on that includes battery
 *       insertion the gauge becomes permanently unresponsive. Suspected
 *       ESD latchup on the SDA/SCL inputs (1.8 V pull-up rail asserted
 *       before the gauge's internal LDO ramps). Standard 9-clock bus
 *       recovery does not resolve it.
 *
 * Rev B will likely drop this part from the BOM and derive SoC from the
 * BQ25622 VBAT ADC + coulomb counting inside charger_task. Until that
 * decision lands, this module presents the API that a UI / battery-monitor
 * layer would eventually call, but every accessor returns a safe default
 * ("offline", zeros) and no I2C traffic is generated.
 *
 * When Rev B makes a final call, either:
 *   (a) wire up the real driver (device address 0x55 on the power bus,
 *       time-muxed via `i2c_bus_acquire(MOKYA_I2C_POWER, ...)`), or
 *   (b) delete this stub and fold the SoC estimator into bq25622.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_BQ27441_H
#define MOKYA_CORE1_BQ27441_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "portmacro.h"

typedef struct {
    bool      online;            /* false → no I2C comms / not implemented */
    uint16_t  voltage_mv;        /* Pack voltage                           */
    int16_t   current_ma;        /* Average current; + = charge, - = load  */
    uint16_t  soc_pct;           /* State of charge, 0..100 %              */
    uint16_t  remaining_mah;     /* RemainingCapacity                      */
    int16_t   temperature_cx10;  /* Internal temperature, °C × 10          */
} bq27441_state_t;

/* Stub: returns true but does NOT create a FreeRTOS task. Reserved so a
 * future Rev B implementation can keep the same call-site in main().
 * Safe to call pre-scheduler. */
bool                     bq27441_start_task(UBaseType_t priority);

/* Stub: returns a pointer to a statically-initialised all-offline snapshot.
 * Pointer is stable for the lifetime of the image. */
const bq27441_state_t   *bq27441_get_state(void);

#endif /* MOKYA_CORE1_BQ27441_H */
