/* bq25622.h — TI BQ25622 I2C battery charger driver (Core 1).
 *
 * Datasheet: SLUSEG2D (BQ25620 / BQ25622), September 2022 / February 2025.
 * On MokyaLora Rev A the device lives on the power bus (7-bit addr 0x6B,
 * GPIO 6/7 routed through the shared `i2c1` peripheral — see
 * firmware/core1/src/i2c/i2c_bus.c).
 *
 * Production settings:
 *   VREG     = 4100 mV   (BL-4C 890 mAh; 100 mV below max for cycle life)
 *   ICHG     = 480 mA    (~0.5C; conservative)
 *   IINDPM   = 500 mA    (Rev A bringup Step 20 — avoids VSYS droop)
 *   WATCHDOG = 50 s      (shortest window; kicked every 1 s by charger_task)
 *   ADC      = 12-bit continuous, all 6 channels
 *
 * The watchdog is the charger's last-line safety: if this MCU hangs, the
 * BQ25622 reverts to its POR-default safe state (EN_CHG=1, ICHG halved,
 * HIZ=0) so charging behaviour remains defined even with a dead host. The
 * charger_task kicks WD_RST on every 1 Hz poll and re-applies all settings
 * on WD_STAT=1 so the production values survive a WD expiry event.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOKYA_CORE1_BQ25622_H
#define MOKYA_CORE1_BQ25622_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "portmacro.h"

typedef enum {
    BQ25622_WD_OFF   = 0,   /* Disabled — no safety net, debug only       */
    BQ25622_WD_50S   = 1,   /* 50 s window, POR default, production target */
    BQ25622_WD_100S  = 2,   /* 100 s                                       */
    BQ25622_WD_200S  = 3,   /* 200 s                                       */
} bq25622_wd_window_t;

typedef struct {
    bool                online;        /* false → I2C comms lost recently */
    /* ADC (12-bit, 1 Hz refresh) */
    uint16_t            vbus_mv;
    uint16_t            vbat_mv;
    uint16_t            vsys_mv;
    uint16_t            vpmid_mv;
    int16_t             ibus_ma;
    int16_t             ibat_ma;
    /* Status */
    uint8_t             chg_stat;      /* 0=NoCHG 1=CC 2=Taper 3=TopOff  */
    uint8_t             vbus_stat;     /* 0=None 4=UnknownAdapter 7=OTG  */
    uint8_t             ts_stat;       /* 0..7 per REG0x1F[2:0]          */
    bool                bat_fault;
    bool                sys_fault;
    bool                tshut;
    /* Diagnostics */
    bq25622_wd_window_t wd_window;
    uint32_t            wd_expired_count;  /* increments on WD_STAT=1    */
    uint32_t            i2c_fail_count;    /* consecutive I2C failures   */
} bq25622_state_t;

/* Starts the 1 Hz charger_task which owns hardware init + periodic poll.
 * MUST be called after the FreeRTOS scheduler is running (or as the last
 * step before vTaskStartScheduler — task creation is scheduler-safe pre-
 * start, but the task itself will not execute until the scheduler runs).
 * Returns pdPASS on success; pdFAIL means heap exhaustion (see FreeRTOS
 * heap audit in core1-driver-development.md §4.1). */
bool                     bq25622_start_task(UBaseType_t priority);

/* Pointer to the globally-updated state snapshot. Pointer is stable; the
 * fields it points to are updated atomically once per second by
 * charger_task. Callers may read without locking — individual field tears
 * are tolerated (UI / SWD observers). */
const bq25622_state_t   *bq25622_get_state(void);

/* Runtime control. Safe to call from any task; acquires the bus mutex.
 * Returns true on I2C success. */
bool                     bq25622_set_charge_enabled(bool enabled);
bool                     bq25622_set_watchdog(bq25622_wd_window_t win);

#endif /* MOKYA_CORE1_BQ25622_H */
