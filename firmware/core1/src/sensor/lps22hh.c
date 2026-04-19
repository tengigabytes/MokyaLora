/* lps22hh.c — LPS22HH driver. See lps22hh.h for module summary.
 *
 * All register / field names match DS DocID030890 §8-§9.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lps22hh.h"

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "i2c_bus.h"

/* ── I2C address ─────────────────────────────────────────────────────────── */
/* SA0 tied high on MokyaLora Rev A (bringup Issue #4). */
#define LPS22HH_ADDR          0x5Du

/* ── Register map (§8 Table 17) ──────────────────────────────────────────── */
#define REG_WHO_AM_I          0x0Fu
#define REG_CTRL_REG1         0x10u
#define REG_CTRL_REG2         0x11u
#define REG_STATUS            0x27u
#define REG_PRESSURE_OUT_XL   0x28u

/* Expected WHO_AM_I (§9.5) */
#define WHO_AM_I_VALUE        0xB3u

/* CTRL_REG1 layout (§9.6): [6:4]=ODR, [3]=EN_LPFP, [2]=LPFP_CFG,
 *                         [1]=BDU, [0]=SIM
 *   ODR=001 (1 Hz), EN_LPFP=1, LPFP_CFG=0 (ODR/9), BDU=1, SIM=0
 *   → 0b0_001_1_0_1_0 = 0x1A */
#define CTRL_REG1_PRODUCTION  0x1Au

/* CTRL_REG2 layout (§9.7): [7]=BOOT, [6]=INT_H_L, [5]=PP_OD, [4]=IF_ADD_INC,
 *                         [3]=0,    [2]=SWRESET,  [1]=LOW_NOISE_EN, [0]=ONE_SHOT
 *   IF_ADD_INC=1, LOW_NOISE_EN=1 (valid because ODR < 100 Hz — §9.7)
 *   → 0b0_0_0_1_0_0_1_0 = 0x12 */
#define CTRL_REG2_PRODUCTION  0x12u
#define CTRL_REG2_SWRESET     0x04u

/* STATUS bits (§9.18) */
#define STATUS_P_DA           (1u << 0)
#define STATUS_T_DA           (1u << 1)

/* Operational constraints */
#define I2C_TIMEOUT_US        50000u
#define SWRESET_TIMEOUT_MS    50u
#define FAIL_DISCONNECT_COUNT 5u

/* ── Driver state ────────────────────────────────────────────────────────── */
static lps22hh_state_t s_state = {
    .online = false,
};

/* ── Low-level helpers (caller holds the bus mutex) ──────────────────────── */

static int reg_write_u8(i2c_inst_t *bus, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_write_timeout_us(bus, LPS22HH_ADDR, buf, 2, false,
                                I2C_TIMEOUT_US);
}

static int reg_read_u8(i2c_inst_t *bus, uint8_t reg, uint8_t *out)
{
    int r = i2c_write_timeout_us(bus, LPS22HH_ADDR, &reg, 1, true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, LPS22HH_ADDR, out, 1, false,
                               I2C_TIMEOUT_US);
}

static int reg_read_block(i2c_inst_t *bus, uint8_t reg,
                          uint8_t *out, size_t len)
{
    int r = i2c_write_timeout_us(bus, LPS22HH_ADDR, &reg, 1, true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, LPS22HH_ADDR, out, len, false,
                               I2C_TIMEOUT_US);
}

/* ── Hardware init (§4 + §9) ─────────────────────────────────────────────── */

static int hw_init(i2c_inst_t *bus)
{
    /* 1. Verify WHO_AM_I */
    uint8_t id;
    int r = reg_read_u8(bus, REG_WHO_AM_I, &id);
    if (r < 0) return r;
    if (id != WHO_AM_I_VALUE) return -1;

    /* 2. Software reset — clears volatile registers to POR defaults.
     *    Bit self-clears when the reset is complete (§9.7). */
    if ((r = reg_write_u8(bus, REG_CTRL_REG2, CTRL_REG2_SWRESET)) < 0)
        return r;
    for (int i = 0; i < SWRESET_TIMEOUT_MS; ++i) {
        uint8_t v;
        r = reg_read_u8(bus, REG_CTRL_REG2, &v);
        if (r < 0) return r;
        if ((v & CTRL_REG2_SWRESET) == 0) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* 3. CTRL_REG2 — IF_ADD_INC + LOW_NOISE_EN. Write before CTRL_REG1 so
     *    LOW_NOISE_EN is latched before the first ODR-driven conversion
     *    (§9.7 note: must be changed in power-down mode). */
    if ((r = reg_write_u8(bus, REG_CTRL_REG2, CTRL_REG2_PRODUCTION)) < 0)
        return r;

    /* 4. CTRL_REG1 — leaving power-down by setting ODR != 000. */
    if ((r = reg_write_u8(bus, REG_CTRL_REG1, CTRL_REG1_PRODUCTION)) < 0)
        return r;

    return 0;
}

/* ── Data conversion (§4.4 / §4.5) ───────────────────────────────────────── *
 *
 * Pressure: 24-bit two's complement, 4096 LSB/hPa.
 *   hPa × 100 = (raw × 100) / 4096 = (raw × 25) / 1024
 *   Range 260..1260 hPa → raw 1,064,960..5,160,960 → fits int32_t.
 *
 * Temperature: 16-bit two's complement, 100 LSB/°C.
 *   °C × 10 = raw / 10.                                                     */
static void decode_sample(const uint8_t *b, lps22hh_state_t *s)
{
    int32_t press_raw = (int32_t)((uint32_t)b[0]
                                  | ((uint32_t)b[1] << 8)
                                  | ((uint32_t)b[2] << 16));
    /* Sign-extend bit 23 */
    if (press_raw & 0x00800000) press_raw |= (int32_t)0xFF000000;
    s->pressure_hpa_x100 = (uint32_t)((press_raw * 25) / 1024);

    int16_t temp_raw = (int16_t)((uint16_t)b[3] | ((uint16_t)b[4] << 8));
    s->temperature_cx10 = (int16_t)(temp_raw / 10);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool lps22hh_init(void)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_SENSOR, portMAX_DELAY);
    if (bus == NULL) return false;

    int r = hw_init(bus);

    i2c_bus_release(MOKYA_I2C_SENSOR);

    if (r < 0) {
        s_state.i2c_fail_count++;
        s_state.online = false;
        return false;
    }
    s_state.online = true;
    return true;
}

bool lps22hh_poll(void)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_SENSOR, portMAX_DELAY);
    if (bus == NULL) return false;

    /* Burst 5 bytes: PRESS_OUT_XL, _L, _H, TEMP_OUT_L, _H (0x28..0x2C).
     * IF_ADD_INC=1 makes this a single repeated-start read. */
    uint8_t buf[5];
    int r = reg_read_block(bus, REG_PRESSURE_OUT_XL, buf, sizeof buf);

    i2c_bus_release(MOKYA_I2C_SENSOR);

    if (r < 0) {
        if (s_state.i2c_fail_count < UINT32_MAX) s_state.i2c_fail_count++;
        if (s_state.i2c_fail_count >= FAIL_DISCONNECT_COUNT)
            s_state.online = false;
        return false;
    }

    decode_sample(buf, &s_state);
    s_state.i2c_fail_count = 0;
    s_state.online = true;
    return true;
}

const lps22hh_state_t *lps22hh_get_state(void)
{
    return &s_state;
}
