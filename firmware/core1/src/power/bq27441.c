/* bq27441.c — STUB.
 *
 * See bq27441.h for the rationale (Rev A Issues #9 and #10). No I2C traffic
 * is generated; no task is created. Every field reports "offline / zero" so
 * that callers wire up cleanly and then light up for free once the Rev B
 * decision lands.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bq27441.h"

static const bq27441_state_t s_state = {
    .online           = false,
    .voltage_mv       = 0,
    .current_ma       = 0,
    .soc_pct          = 0,
    .remaining_mah    = 0,
    .temperature_cx10 = 0,
};

bool bq27441_start_task(UBaseType_t priority)
{
    (void)priority;
    /* Intentional no-op. Rev B will either build a real task here or delete
     * this stub and fold SoC estimation into bq25622.c. */
    return true;
}

const bq27441_state_t *bq27441_get_state(void)
{
    return &s_state;
}
