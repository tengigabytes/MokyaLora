/* lm27965.c — see lm27965.h for module summary.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lm27965.h"

#include "hardware/i2c.h"

#include "FreeRTOS.h"
#include "portmacro.h"

#include "i2c_bus.h"

/* ── I2C address + register map ──────────────────────────────────────────── */
#define LM27965_ADDR          0x36u

#define REG_GP                0x10u   /* ENA|ENB|ENC|EN5A|EN3B + reserved[5] */
#define REG_BANKA             0xA0u   /* [4:0] duty for D1A..D5A             */
#define REG_BANKB             0xB0u   /* [4:0] duty for D1B/D2B + D3B share  */
#define REG_BANKC             0xC0u   /* [1:0] duty for D1C                  */

/* GP bit layout. bit 5 (reserved) is always written as 1 per datasheet.    */
#define GP_ENA                (1u << 0)   /* Bank A on (TFT BL)              */
#define GP_ENB                (1u << 1)   /* Bank B on (keypad D1B/D2B)      */
#define GP_ENC                (1u << 2)   /* Bank C on (red)                 */
#define GP_EN5A               (1u << 3)   /* 5th A channel (Rev A unused)    */
#define GP_EN3B               (1u << 4)   /* D3B green gate (Bank B duty)    */
#define GP_RESERVED           (1u << 5)   /* must be 1                       */

#define I2C_TIMEOUT_US        50000u

/* ── Driver state (cache) ────────────────────────────────────────────────── */
static lm27965_state_t s_state = {
    .online      = false,
    .gp          = GP_RESERVED,   /* GP = 0x20, all enables off              */
    .bank_a_duty = 0,
    .bank_b_duty = 0,
    .bank_c_duty = 0,
};

/* ── Low-level I2C (caller holds bus mutex) ──────────────────────────────── */

static int reg_write(i2c_inst_t *bus, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_write_timeout_us(bus, LM27965_ADDR, buf, 2, false,
                                I2C_TIMEOUT_US);
}

/* ── Duty helpers ────────────────────────────────────────────────────────── */

static inline uint8_t clamp_ab(uint8_t d)
{
    return d > LM27965_DUTY_AB_MAX ? LM27965_DUTY_AB_MAX : d;
}

static inline uint8_t clamp_c(uint8_t d)
{
    return d > LM27965_DUTY_C_MAX ? LM27965_DUTY_C_MAX : d;
}

/* ── Internal apply helpers ──────────────────────────────────────────────── *
 *
 * Each public API takes the bus mutex, mutates the relevant fields in
 * s_state, writes the duty register (if changed) and the GP register,
 * then releases. GP is always rewritten from the cache so any concurrent
 * bit change is consistent with the chip.                                  */

static bool apply_gp(i2c_inst_t *bus)
{
    /* Always set reserved bit 5 — datasheet requirement. */
    const uint8_t v = (uint8_t)(s_state.gp | GP_RESERVED);
    int r = reg_write(bus, REG_GP, v);
    if (r < 0) return false;
    s_state.gp = v;
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool lm27965_init(uint8_t tft_duty)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    const uint8_t duty = clamp_ab(tft_duty);
    int r;

    /* Order matters: write Bank A duty BEFORE GP.ENA goes high so the
     * panel never sees a brief full-scale current spike from a stale
     * register (31/31 POR-default duty). bringup_tft.c and led_apply()
     * follow the same sequence. */
    r = reg_write(bus, REG_BANKA, duty);
    if (r < 0) goto fail;
    s_state.bank_a_duty = duty;

    /* Start with Bank B and C fully off; duty registers untouched so the
     * cached zeros match the chip. */
    r = reg_write(bus, REG_BANKB, 0);
    if (r < 0) goto fail;
    r = reg_write(bus, REG_BANKC, 0);
    if (r < 0) goto fail;

    /* GP: only ENA if caller asked for non-zero duty. */
    s_state.gp = GP_RESERVED | (duty > 0 ? GP_ENA : 0u);
    if (!apply_gp(bus)) goto fail;

    s_state.online = true;
    i2c_bus_release(MOKYA_I2C_POWER);
    return true;

fail:
    s_state.online = false;
    i2c_bus_release(MOKYA_I2C_POWER);
    return false;
}

bool lm27965_set_tft_backlight(uint8_t duty)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    const uint8_t d = clamp_ab(duty);
    int r = reg_write(bus, REG_BANKA, d);
    if (r < 0) { s_state.online = false; i2c_bus_release(MOKYA_I2C_POWER); return false; }
    s_state.bank_a_duty = d;

    if (d > 0) s_state.gp |=  GP_ENA;
    else       s_state.gp &= (uint8_t)~GP_ENA;

    bool ok = apply_gp(bus);
    s_state.online = ok;
    i2c_bus_release(MOKYA_I2C_POWER);
    return ok;
}

bool lm27965_set_keypad_backlight(bool kbd_on, bool green_on, uint8_t duty)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    const uint8_t d = clamp_ab(duty);
    int r = reg_write(bus, REG_BANKB, d);
    if (r < 0) { s_state.online = false; i2c_bus_release(MOKYA_I2C_POWER); return false; }
    s_state.bank_b_duty = d;

    /* ENB gates D1B + D2B; EN3B gates D3B. Both consume Bank-B duty. */
    s_state.gp = (uint8_t)((s_state.gp & (uint8_t)~(GP_ENB | GP_EN3B))
                           | (kbd_on   ? GP_ENB  : 0u)
                           | (green_on ? GP_EN3B : 0u));

    bool ok = apply_gp(bus);
    s_state.online = ok;
    i2c_bus_release(MOKYA_I2C_POWER);
    return ok;
}

bool lm27965_set_led_red(bool on, uint8_t duty)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    const uint8_t d = clamp_c(duty);
    int r = reg_write(bus, REG_BANKC, d);
    if (r < 0) { s_state.online = false; i2c_bus_release(MOKYA_I2C_POWER); return false; }
    s_state.bank_c_duty = d;

    if (on) s_state.gp |=  GP_ENC;
    else    s_state.gp &= (uint8_t)~GP_ENC;

    bool ok = apply_gp(bus);
    s_state.online = ok;
    i2c_bus_release(MOKYA_I2C_POWER);
    return ok;
}

bool lm27965_all_off(void)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    /* Clear all enables but keep reserved bit 5. Duty registers untouched —
     * re-enabling the same bank restores the previous brightness. */
    s_state.gp = GP_RESERVED;
    bool ok = apply_gp(bus);
    s_state.online = ok;
    i2c_bus_release(MOKYA_I2C_POWER);
    return ok;
}

const lm27965_state_t *lm27965_get_state(void)
{
    return &s_state;
}
