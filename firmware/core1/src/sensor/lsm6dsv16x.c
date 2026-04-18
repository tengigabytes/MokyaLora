/* lsm6dsv16x.c — LSM6DSV16X driver. See lsm6dsv16x.h for module summary.
 *
 * All register / field names match DS13510 Rev 4 §9.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lsm6dsv16x.h"

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "i2c_bus.h"

/* ── I2C address ─────────────────────────────────────────────────────────── */
#define LSM6DSV16X_ADDR       0x6Au

/* ── Register map (§8 Table 24) ──────────────────────────────────────────── */
#define REG_WHO_AM_I          0x0Fu
#define REG_CTRL1             0x10u
#define REG_CTRL2             0x11u
#define REG_CTRL3             0x12u
#define REG_CTRL6             0x15u
#define REG_CTRL8             0x17u
#define REG_STATUS_REG        0x1Eu
#define REG_OUT_TEMP_L        0x20u   /* 0x20..0x2D = T(2)+G(6)+A(6) = 14B */

/* Expected WHO_AM_I (§9.13) */
#define WHO_AM_I_VALUE        0x70u

/* CTRL1 (§9.14 Table 50-52): [6:4]=OP_MODE_XL, [3:0]=ODR_XL.
 *   OP_MODE_XL=000 (high-performance), ODR_XL=0100 (30 Hz) → 0x04 */
#define CTRL1_PRODUCTION      0x04u

/* CTRL2 (§9.15 Table 53-55): [6:4]=OP_MODE_G, [3:0]=ODR_G.
 *   OP_MODE_G=000 (high-performance), ODR_G=0100 (30 Hz) → 0x04 */
#define CTRL2_PRODUCTION      0x04u

/* CTRL3 (§9.16 Table 56): [7]=BOOT, [6]=BDU, [2]=IF_INC, [0]=SW_RESET.
 *   BDU=1, IF_INC=1 → 0x44 (also the post-reset default per Table 24). */
#define CTRL3_PRODUCTION      0x44u
#define CTRL3_SW_RESET        0x01u

/* CTRL6 (§9.19 Table 62): [6:4]=LPF1_G_BW, [3:0]=FS_G.
 *   FS_G=0001 (±250 dps) → 0x01 */
#define CTRL6_PRODUCTION      0x01u

/* CTRL8 (§9.21 Table 67): [7:5]=HP_LPF2_XL_BW, [3]=XL_DualC_EN, [1:0]=FS_XL.
 *   FS_XL=00 (±2 g) → 0x00 */
#define CTRL8_PRODUCTION      0x00u

/* Operational constraints */
#define I2C_TIMEOUT_US        50000u
#define SWRESET_POLL_MS       1u
#define SWRESET_TIMEOUT_MS    50u
#define FAIL_DISCONNECT_COUNT 5u

/* Temperature zero point: °C × 10. Datasheet §9.29 defines sensitivity
 * (256 LSB/°C) and ST HAL (lsm6dsv16x_from_lsb_to_celsius) encodes the
 * zero point at 25 °C. */
#define TEMP_ZERO_POINT_CX10  250

/* ── Driver state ────────────────────────────────────────────────────────── */
static lsm6dsv16x_state_t s_state = {
    .online = false,
};

/* ── Low-level helpers (caller holds the bus mutex) ──────────────────────── */

static int reg_write_u8(i2c_inst_t *bus, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_write_timeout_us(bus, LSM6DSV16X_ADDR, buf, 2, false,
                                I2C_TIMEOUT_US);
}

static int reg_read_u8(i2c_inst_t *bus, uint8_t reg, uint8_t *out)
{
    int r = i2c_write_timeout_us(bus, LSM6DSV16X_ADDR, &reg, 1, true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, LSM6DSV16X_ADDR, out, 1, false,
                               I2C_TIMEOUT_US);
}

static int reg_read_block(i2c_inst_t *bus, uint8_t reg,
                          uint8_t *out, size_t len)
{
    int r = i2c_write_timeout_us(bus, LSM6DSV16X_ADDR, &reg, 1, true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, LSM6DSV16X_ADDR, out, len, false,
                               I2C_TIMEOUT_US);
}

/* ── Hardware init ───────────────────────────────────────────────────────── */

static int hw_init(i2c_inst_t *bus)
{
    /* 1. Verify WHO_AM_I */
    uint8_t id;
    int r = reg_read_u8(bus, REG_WHO_AM_I, &id);
    if (r < 0) return r;
    if (id != WHO_AM_I_VALUE) return -1;

    /* 2. Software reset. §9.16 says SW_RESET is self-clearing. Poll until
     *    bit 0 reads back 0, or time out. */
    if ((r = reg_write_u8(bus, REG_CTRL3, CTRL3_SW_RESET)) < 0) return r;
    {
        uint8_t c = CTRL3_SW_RESET;
        uint32_t waited = 0;
        while (waited < SWRESET_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(SWRESET_POLL_MS));
            waited += SWRESET_POLL_MS;
            if (reg_read_u8(bus, REG_CTRL3, &c) < 0) continue;
            if (!(c & CTRL3_SW_RESET)) break;
        }
        if (c & CTRL3_SW_RESET) return -2;
    }

    /* 3. CTRL3: BDU=1, IF_INC=1 (post-reset default, but set explicitly). */
    if ((r = reg_write_u8(bus, REG_CTRL3, CTRL3_PRODUCTION)) < 0) return r;

    /* 4. Full-scale ranges before leaving power-down. */
    if ((r = reg_write_u8(bus, REG_CTRL8, CTRL8_PRODUCTION)) < 0) return r;
    if ((r = reg_write_u8(bus, REG_CTRL6, CTRL6_PRODUCTION)) < 0) return r;

    /* 5. Activate accelerometer + gyroscope at 30 Hz high-performance. */
    if ((r = reg_write_u8(bus, REG_CTRL1, CTRL1_PRODUCTION)) < 0) return r;
    if ((r = reg_write_u8(bus, REG_CTRL2, CTRL2_PRODUCTION)) < 0) return r;

    return 0;
}

/* ── Data conversion ─────────────────────────────────────────────────────── *
 *
 * Accel: 0.061 mg/LSB at ±2 g FS → mg = raw × 61 / 1000.
 *        ±2000 mg range fits int16_t.
 * Gyro : 8.75 mdps/LSB at ±250 dps FS → dps × 10 = raw × 875 / 10000.
 *        ±2500 (dps × 10) range fits int16_t.
 * Temp : 256 LSB/°C, zero point 25 °C → °C × 10 = 250 + raw × 5 / 128.
 */

static int16_t le16(const uint8_t *b)
{
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static void decode_burst(const uint8_t *b, lsm6dsv16x_state_t *s)
{
    int16_t t_raw = le16(&b[0]);
    s->temperature_cx10 = (int16_t)(TEMP_ZERO_POINT_CX10
                                    + ((int32_t)t_raw * 5) / 128);

    for (int i = 0; i < 3; ++i) {
        int16_t g = le16(&b[2 + i * 2]);
        s->gyro_raw[i]       = g;
        s->gyro_dps_x10[i]   = (int16_t)(((int32_t)g * 875) / 10000);
    }
    for (int i = 0; i < 3; ++i) {
        int16_t a = le16(&b[8 + i * 2]);
        s->accel_raw[i]      = a;
        s->accel_mg[i]       = (int16_t)(((int32_t)a * 61) / 1000);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool lsm6dsv16x_init(void)
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

bool lsm6dsv16x_poll(void)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_SENSOR, portMAX_DELAY);
    if (bus == NULL) return false;

    /* Single 14-byte burst covers Temp + Gyro + Accel. BDU=1 + IF_INC=1
     * keep the sample coherent across the whole read. */
    uint8_t buf[14];
    int r = reg_read_block(bus, REG_OUT_TEMP_L, buf, sizeof buf);

    i2c_bus_release(MOKYA_I2C_SENSOR);

    if (r < 0) {
        if (s_state.i2c_fail_count < UINT32_MAX) s_state.i2c_fail_count++;
        if (s_state.i2c_fail_count >= FAIL_DISCONNECT_COUNT)
            s_state.online = false;
        return false;
    }

    decode_burst(buf, &s_state);
    s_state.i2c_fail_count = 0;
    s_state.online = true;
    return true;
}

const lsm6dsv16x_state_t *lsm6dsv16x_get_state(void)
{
    return &s_state;
}
