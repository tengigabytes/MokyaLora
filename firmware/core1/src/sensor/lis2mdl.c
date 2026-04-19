/* lis2mdl.c — LIS2MDL driver. See lis2mdl.h for module summary.
 *
 * All register / field names match DS12144 Rev 6 §7-§8.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lis2mdl.h"

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "i2c_bus.h"

/* ── I2C address (§6.1.1 Table 20) ───────────────────────────────────────── */
#define LIS2MDL_ADDR          0x1Eu

/* Auto-increment flag for multi-byte I²C reads/writes (§6.1.1): SUB[7]=1
 * makes the device auto-increment the sub-address for each byte. The SDK
 * does NOT set this for us — drivers must OR it in explicitly. */
#define SUB_AUTO_INC          0x80u

/* ── Register map (§7 Table 21) ──────────────────────────────────────────── */
#define REG_WHO_AM_I          0x4Fu
#define REG_CFG_REG_A         0x60u
#define REG_CFG_REG_B         0x61u
#define REG_CFG_REG_C         0x62u
#define REG_STATUS_REG        0x67u
#define REG_OUTX_L_REG        0x68u   /* 0x68..0x6D = X/Y/Z (6 bytes)    */
#define REG_TEMP_OUT_L_REG    0x6Eu   /* 0x6E..0x6F = temperature        */

/* Expected WHO_AM_I (§8.4) */
#define WHO_AM_I_VALUE        0x40u

/* CFG_REG_A layout (§8.5):
 *   [7]=COMP_TEMP_EN  [6]=REBOOT  [5]=SOFT_RST  [4]=LP
 *   [3:2]=ODR         [1:0]=MD
 *   Production: COMP_TEMP_EN=1, LP=0 (hi-res), ODR=00 (10 Hz),
 *   MD=00 (continuous) → 0b1_0_0_0_00_00 = 0x80 */
/* CFG_REG_A layout (§8.5):
 *   [7]=COMP_TEMP_EN  [6]=REBOOT  [5]=SOFT_RST  [4]=LP
 *   [3:2]=ODR         [1:0]=MD
 *   Production: COMP_TEMP_EN=1, LP=0 (hi-res), ODR=00 (10 Hz),
 *   MD=00 (continuous) → 0b1_0_0_0_00_00 = 0x80 */
#define CFG_REG_A_PRODUCTION  0x80u
#define CFG_REG_A_SOFT_RST    0x20u

/* CFG_REG_B layout (§8.6):
 *   [4]=INT_on_DataOFF [3]=Set_FREQ [1]=OFF_CANC [0]=LPF
 *   Production: OFF_CANC=1, LPF=1 → 0b000_0_0_0_1_1 = 0x03 */
#define CFG_REG_B_PRODUCTION  0x03u

/* CFG_REG_C layout (§8.7):
 *   [6]=INT_on_PIN [5]=I2C_DIS [4]=BDU [3]=BLE [2]=4WSPI
 *   [1]=Self_test  [0]=DRDY_on_PIN
 *   Production: BDU=1 → 0b0_0_0_1_0_0_0_0 = 0x10 */
#define CFG_REG_C_PRODUCTION  0x10u

/* Operational constraints */
#define I2C_TIMEOUT_US        50000u
#define SWRESET_DELAY_MS      10u
#define FAIL_DISCONNECT_COUNT 5u

/* ── Driver state ────────────────────────────────────────────────────────── */
static lis2mdl_state_t s_state = {
    .online = false,
};

/* ── Low-level helpers (caller holds the bus mutex) ──────────────────────── */

static int reg_write_u8(i2c_inst_t *bus, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_write_timeout_us(bus, LIS2MDL_ADDR, buf, 2, false,
                                I2C_TIMEOUT_US);
}

static int reg_read_u8(i2c_inst_t *bus, uint8_t reg, uint8_t *out)
{
    int r = i2c_write_timeout_us(bus, LIS2MDL_ADDR, &reg, 1, true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, LIS2MDL_ADDR, out, 1, false,
                               I2C_TIMEOUT_US);
}

/* Burst read. Datasheet §6.1.1 states SUB[7]=1 enables auto-increment, but
 * bringup (bringup_sensors.c:mag_read) reads 6 mag bytes with plain SUB=0x68
 * and gets correct data, so auto-increment appears to be active regardless.
 * Keep the low-address form to match the known-good bringup sequence. */
static int reg_read_block(i2c_inst_t *bus, uint8_t reg,
                          uint8_t *out, size_t len)
{
    int r = i2c_write_timeout_us(bus, LIS2MDL_ADDR, &reg, 1, true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, LIS2MDL_ADDR, out, len, false,
                               I2C_TIMEOUT_US);
}

/* ── Hardware init (§5.3 + §8) ───────────────────────────────────────────── */

static int hw_init(i2c_inst_t *bus)
{
    /* 1. Verify WHO_AM_I */
    uint8_t id;
    int r = reg_read_u8(bus, REG_WHO_AM_I, &id);
    if (r < 0) return r;
    if (id != WHO_AM_I_VALUE) return -1;

    /* 2. Software reset — datasheet does not guarantee self-clear, use a
     *    fixed delay. App-note §5.3 omits reset entirely, so 10 ms is
     *    generous. */
    if ((r = reg_write_u8(bus, REG_CFG_REG_A, CFG_REG_A_SOFT_RST)) < 0)
        return r;
    vTaskDelay(pdMS_TO_TICKS(SWRESET_DELAY_MS));

    /* 3. CFG_REG_B and CFG_REG_C before CFG_REG_A — OFF_CANC, LPF, and BDU
     *    should be latched before leaving power-down / idle mode. */
    if ((r = reg_write_u8(bus, REG_CFG_REG_B, CFG_REG_B_PRODUCTION)) < 0)
        return r;
    if ((r = reg_write_u8(bus, REG_CFG_REG_C, CFG_REG_C_PRODUCTION)) < 0)
        return r;

    /* 4. CFG_REG_A — leaves idle by setting MD=00 (continuous) and enables
     *    temp compensation as required by §8.5 footnote 1. */
    if ((r = reg_write_u8(bus, REG_CFG_REG_A, CFG_REG_A_PRODUCTION)) < 0)
        return r;

    return 0;
}

/* ── Data conversion (§2.1 / §8.13-§8.16) ────────────────────────────────── *
 *
 * Magnetic axes: 16-bit two's complement, 1.5 mgauss/LSB = 0.15 µT/LSB.
 *   µT × 10 = (raw × 15) / 10 = (raw × 3) / 2.
 *   Full-range ±50 gauss ≈ ±5 mT → raw ≈ ±33,333, (raw*3/2) fits int16_t.
 *
 * Temperature: 16-bit two's complement, 8 LSB/°C, 12-bit resolution with
 * bits 15-12 = sign extension (§2.2 note 2, §8.16). Zero point is 25 °C
 * — datasheet doesn't state this, but ST's official HAL (lis2mdl_reg.c,
 * lis2mdl_from_lsb_to_celsius) encodes raw/8 + 25.
 *   °C × 10 = 250 + raw × 10 / 8 = 250 + (raw × 5) / 4.
 */
#define TEMP_ZERO_POINT_CX10  250  /* 25 °C × 10 */

static void decode_mag(const uint8_t *b, lis2mdl_state_t *s)
{
    for (int i = 0; i < 3; ++i) {
        int16_t raw = (int16_t)((uint16_t)b[i * 2]
                                | ((uint16_t)b[i * 2 + 1] << 8));
        s->mag_raw[i]    = raw;
        s->mag_ut_x10[i] = (int16_t)(((int32_t)raw * 3) / 2);
    }
}

static void decode_temp(const uint8_t *b, lis2mdl_state_t *s)
{
    int16_t t_raw = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    s->temperature_cx10 = (int16_t)(TEMP_ZERO_POINT_CX10
                                    + ((int32_t)t_raw * 5) / 4);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool lis2mdl_init(void)
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

bool lis2mdl_poll(void)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_SENSOR, portMAX_DELAY);
    if (bus == NULL) return false;

    /* Split mag and temp into two transactions. LIS2MDL auto-increment
     * wraps at the end of the mag OUT block (0x6D → 0x68) and does NOT
     * cross into TEMP_OUT at 0x6E, so an 8-byte burst from 0x68 would
     * return X/Y/Z then X/Y again — not temperature. Verified via SWD:
     * direct 2-byte read at 0x6E gives +28 (≈28 °C), burst bytes 6-7
     * repeat OUTX_L/H. BDU=1 still prevents low/high-byte tearing within
     * each read. */
    uint8_t mbuf[6];
    int r = reg_read_block(bus, REG_OUTX_L_REG, mbuf, sizeof mbuf);

    uint8_t tbuf[2] = { 0, 0 };
    int r2 = (r >= 0)
                ? reg_read_block(bus, REG_TEMP_OUT_L_REG, tbuf, sizeof tbuf)
                : -1;

    i2c_bus_release(MOKYA_I2C_SENSOR);

    if (r < 0 || r2 < 0) {
        if (s_state.i2c_fail_count < UINT32_MAX) s_state.i2c_fail_count++;
        if (s_state.i2c_fail_count >= FAIL_DISCONNECT_COUNT)
            s_state.online = false;
        return false;
    }

    decode_mag(mbuf, &s_state);
    decode_temp(tbuf, &s_state);
    s_state.i2c_fail_count = 0;
    s_state.online = true;
    return true;
}

const lis2mdl_state_t *lis2mdl_get_state(void)
{
    return &s_state;
}
