/* i2c_bus.c — Core 1 shared I2C bus module. See i2c_bus.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "i2c_bus.h"

#include "hardware/gpio.h"
#include "hardware/regs/i2c.h"
#include "pico/time.h"   /* busy_wait_us */

#include "FreeRTOS.h"
#include "semphr.h"

/* ── Pin map (Rev A schematic) ───────────────────────────────────────────── */
#define PIN_POWER_SDA    6u
#define PIN_POWER_SCL    7u
#define PIN_SENSOR_SDA   34u
#define PIN_SENSOR_SCL   35u

#define I2C_DEFAULT_BAUD  400000u   /* 400 kHz — matches bringup FW */

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

/* Replicates the SDK's `i2c_set_baudrate` but with the hard-coded Core 1
 * clk_peri of 150 MHz. Sets BOTH standard-speed AND fast-speed timing
 * registers, plus sda_hold, mirroring the SDK so any code path that
 * consults either register set sees a valid value. Speed mode is chosen
 * per baudrate (STANDARD ≤100 kHz, else FAST). */
static void i2c_set_baudrate_core1(i2c_inst_t *i2c, uint baudrate)
{
    uint freq_in = CORE1_CLK_PERI_HZ;
    uint period = (freq_in + baudrate / 2u) / baudrate;
    uint lcnt = period * 3u / 5u;
    uint hcnt = period - lcnt;
    uint sda_tx_hold_count =
        (baudrate < 1000000u) ? ((freq_in * 3u) / 10000000u + 1u)
                              : ((freq_in * 3u) / 25000000u + 1u);

    i2c->hw->enable = 0;
    hw_write_masked(&i2c->hw->con,
                    (baudrate <= 100000u
                        ? I2C_IC_CON_SPEED_VALUE_STANDARD
                        : I2C_IC_CON_SPEED_VALUE_FAST)
                        << I2C_IC_CON_SPEED_LSB,
                    I2C_IC_CON_SPEED_BITS);
    i2c->hw->fs_scl_hcnt = hcnt;
    i2c->hw->fs_scl_lcnt = lcnt;
    i2c->hw->fs_spklen = (lcnt < 16u) ? 1u : lcnt / 16u;
    /* Duplicate into SS timing too so either speed path is sane. */
    i2c->hw->ss_scl_hcnt = hcnt;
    i2c->hw->ss_scl_lcnt = lcnt;
    hw_write_masked(&i2c->hw->sda_hold,
                    sda_tx_hold_count << I2C_IC_SDA_HOLD_IC_SDA_TX_HOLD_LSB,
                    I2C_IC_SDA_HOLD_IC_SDA_TX_HOLD_BITS);
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
    /* Changing SDA/SCL pads under i2c1's feet drops the peripheral into an
     * unrecoverable state. Full re-init after every pin change, matching
     * the bringup firmware's per-entry i2c_init pattern. */
    i2c_init(i2c1, I2C_DEFAULT_BAUD);
    i2c_set_baudrate_core1(i2c1, I2C_DEFAULT_BAUD);
    s_active = id;
}

i2c_inst_t *i2c_bus_acquire(mokya_i2c_id_t id, TickType_t timeout)
{
    if (xSemaphoreTake(s_bus_mutex, timeout) != pdTRUE)
        return NULL;
    switch_pinmux(id);
    return i2c1;
}

/* ── Bus stuck-low recovery (SWD-triggered) ──────────────────────── *
 *
 * If a slave dies mid-byte holding SDA low, no I2C controller can
 * recover — the master needs to bit-bang 9 SCL pulses to clock out
 * any in-flight bit, then issue a STOP. We expose this as a manual
 * SWD trigger because automatic detection at boot is fragile (the
 * SCL line could legitimately be low when this MCU is mid-bus
 * transaction with another core … not a concern on Rev A but worth
 * keeping the option explicit).
 *
 * Call sequence (test scripts):
 *   write any non-zero value to g_i2c_bus_recovery_request
 *   bridge_task polls and runs i2c_bus_recovery on the SENSOR pair,
 *   then mirrors the request value into g_i2c_bus_recovery_done. */

volatile uint32_t g_i2c_bus_recovery_request __attribute__((used)) = 0u;
volatile uint32_t g_i2c_bus_recovery_done    __attribute__((used)) = 0u;
volatile uint8_t  g_i2c_bus_recovery_sda_after __attribute__((used)) = 0u;
volatile uint8_t  g_i2c_bus_recovery_scl_after __attribute__((used)) = 0u;

void i2c_bus_recovery(mokya_i2c_id_t id)
{
    uint sda_pin = (id == MOKYA_I2C_POWER) ? PIN_POWER_SDA : PIN_SENSOR_SDA;
    uint scl_pin = (id == MOKYA_I2C_POWER) ? PIN_POWER_SCL : PIN_SENSOR_SCL;

    /* Take the bus mutex so no other task fights us. */
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    /* Switch both pins to plain SIO output, internal pull-ups on. */
    gpio_set_function(scl_pin, GPIO_FUNC_SIO);
    gpio_set_function(sda_pin, GPIO_FUNC_SIO);
    gpio_pull_up(scl_pin);
    gpio_pull_up(sda_pin);
    gpio_set_dir(scl_pin, GPIO_OUT);
    gpio_set_dir(sda_pin, GPIO_IN);   /* let SDA float — we sense it */
    gpio_put(scl_pin, 1);
    busy_wait_us(20);

    /* 9 SCL pulses at ~50 kHz (10 µs high / 10 µs low). */
    for (int i = 0; i < 9; i++) {
        gpio_put(scl_pin, 0);
        busy_wait_us(10);
        gpio_put(scl_pin, 1);
        busy_wait_us(10);
        if (gpio_get(sda_pin)) break;   /* slave released SDA */
    }

    /* STOP condition: drive SDA low while SCL high, then release SDA. */
    gpio_set_dir(sda_pin, GPIO_OUT);
    gpio_put(sda_pin, 0);
    busy_wait_us(10);
    gpio_put(scl_pin, 1);
    busy_wait_us(10);
    gpio_put(sda_pin, 1);
    busy_wait_us(10);

    g_i2c_bus_recovery_sda_after = (uint8_t)gpio_get(sda_pin);
    g_i2c_bus_recovery_scl_after = (uint8_t)gpio_get(scl_pin);

    /* Restore I2C pinmux + reinit peripheral so it's clean. */
    gpio_set_dir(sda_pin, GPIO_IN);
    gpio_set_dir(scl_pin, GPIO_IN);
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    i2c_init(i2c1, I2C_DEFAULT_BAUD);
    i2c_set_baudrate_core1(i2c1, I2C_DEFAULT_BAUD);
    s_active = id;

    xSemaphoreGive(s_bus_mutex);
}

void i2c_bus_release(mokya_i2c_id_t id)
{
    (void)id;
    xSemaphoreGive(s_bus_mutex);
}
