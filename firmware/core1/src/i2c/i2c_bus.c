/* i2c_bus.c — Core 1 shared I2C bus module. See i2c_bus.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "i2c_bus.h"

#include "hardware/gpio.h"
#include "hardware/regs/i2c.h"

#include "FreeRTOS.h"
#include "semphr.h"

/* ── Pin map (Rev A schematic) ───────────────────────────────────────────── */
#define PIN_POWER_SDA    6u
#define PIN_POWER_SCL    7u
#define PIN_SENSOR_SDA   34u
#define PIN_SENSOR_SCL   35u

#define I2C_DEFAULT_BAUD  100000u   /* 100 kHz — all power/sensor devices */

/* ── Core 1 baudrate fix ─────────────────────────────────────────────────── *
 *
 * Core 1 skips runtime_init_clocks (Core 0 owns clock init), so
 * clock_get_hz(clk_peri) returns 0 and i2c_init() computes a garbage SCL
 * divisor. We set the SCL timing registers using the known 150 MHz clk_peri
 * (Arduino-Pico default on Core 0).
 *
 * Formula mirrors Pico SDK i2c.c:
 *   period = freq_in / baudrate
 *   lcnt   = period * 3/5 - 1   (low phase, slightly longer)
 *   hcnt   = period - lcnt - 8  (high phase, accounts for rise time)
 *   For 100 kHz @ 150 MHz: period=1500, lcnt=899, hcnt=593                    */
#define CORE1_CLK_PERI_HZ  150000000u

static void i2c_set_baudrate_core1(i2c_inst_t *i2c, uint baudrate)
{
    uint period = CORE1_CLK_PERI_HZ / baudrate;
    uint lcnt = period * 3u / 5u - 1u;
    uint hcnt = period - lcnt - 8u;
    if (hcnt < 8u) hcnt = 8u;
    if (lcnt < 1u) lcnt = 1u;
    i2c->hw->enable = 0;
    hw_write_masked(&i2c->hw->con,
                    I2C_IC_CON_SPEED_VALUE_STANDARD << I2C_IC_CON_SPEED_LSB,
                    I2C_IC_CON_SPEED_BITS);
    i2c->hw->ss_scl_hcnt = hcnt;
    i2c->hw->ss_scl_lcnt = lcnt;
    i2c->hw->fs_spklen = lcnt < 16u ? 1u : lcnt / 16u;
    i2c->hw->enable = 1;
}

/* ── State ───────────────────────────────────────────────────────────────── */
static SemaphoreHandle_t s_bus_mutex;
static mokya_i2c_id_t    s_active = MOKYA_I2C_POWER;

void i2c_bus_init_all(void)
{
    /* Enable internal pull-ups on both pairs so deselected lines stay idle-
     * HIGH. External pull-ups dominate electrically; this is cheap insurance. */
    gpio_pull_up(PIN_POWER_SDA);
    gpio_pull_up(PIN_POWER_SCL);
    gpio_pull_up(PIN_SENSOR_SDA);
    gpio_pull_up(PIN_SENSOR_SCL);

    /* i2c1 peripheral up once — stays enabled for the lifetime of the image. */
    i2c_init(i2c1, I2C_DEFAULT_BAUD);
    i2c_set_baudrate_core1(i2c1, I2C_DEFAULT_BAUD);

    /* Default the pinmux to the POWER pair so early boot traffic (if any)
     * lands on the correct bus. acquire() always re-muxes anyway. */
    gpio_set_function(PIN_POWER_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_POWER_SCL, GPIO_FUNC_I2C);
    s_active = MOKYA_I2C_POWER;

    /* Dynamic allocation — heap_4 is available pre-scheduler. Mutex control
     * block is ~80 B; fits comfortably in the 32 KB heap budget. */
    s_bus_mutex = xSemaphoreCreateMutex();
}

static void switch_pinmux(mokya_i2c_id_t id)
{
    if (id == s_active) return;
    if (id == MOKYA_I2C_POWER) {
        gpio_set_function(PIN_SENSOR_SDA, GPIO_FUNC_NULL);
        gpio_set_function(PIN_SENSOR_SCL, GPIO_FUNC_NULL);
        gpio_set_function(PIN_POWER_SDA,  GPIO_FUNC_I2C);
        gpio_set_function(PIN_POWER_SCL,  GPIO_FUNC_I2C);
    } else {
        gpio_set_function(PIN_POWER_SDA,  GPIO_FUNC_NULL);
        gpio_set_function(PIN_POWER_SCL,  GPIO_FUNC_NULL);
        gpio_set_function(PIN_SENSOR_SDA, GPIO_FUNC_I2C);
        gpio_set_function(PIN_SENSOR_SCL, GPIO_FUNC_I2C);
    }
    s_active = id;
}

i2c_inst_t *i2c_bus_acquire(mokya_i2c_id_t id, TickType_t timeout)
{
    if (xSemaphoreTake(s_bus_mutex, timeout) != pdTRUE)
        return NULL;
    switch_pinmux(id);
    return i2c1;
}

void i2c_bus_release(mokya_i2c_id_t id)
{
    (void)id;
    xSemaphoreGive(s_bus_mutex);
}
