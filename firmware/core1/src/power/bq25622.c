/* bq25622.c — TI BQ25622 driver. See bq25622.h for module summary.
 *
 * All register / field names match SLUSEG2D §8.6. Every configuration
 * value is derived from datasheet bit-step constants, not magic numbers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bq25622.h"

#include <string.h>

#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "i2c_bus.h"

/* ── I2C address ─────────────────────────────────────────────────────────── */
#define BQ25622_ADDR          0x6Bu

/* ── Register map (SLUSEG2D §8.6.2) ──────────────────────────────────────── */
#define REG_ICHG_LO           0x02u   /* ICHG[1:0] in [7:6]                   */
#define REG_ICHG_HI           0x03u   /* ICHG[5:2] in [3:0]                   */
#define REG_VREG_LO           0x04u   /* VREG[4:0] in [7:3]                   */
#define REG_VREG_HI           0x05u   /* VREG[8:5] in [3:0]                   */
#define REG_IINDPM_LO         0x06u   /* IINDPM[3:0] in [7:4]                 */
#define REG_IINDPM_HI         0x07u   /* IINDPM[7:4] in [3:0]                 */
#define REG_CHARGER_CTRL1     0x16u   /* EN_AUTO_IBATDIS|FORCE_IBATDIS|EN_CHG|
                                         EN_HIZ|FORCE_PMID_DIS|WD_RST|WATCHDOG */
#define REG_CHARGER_CTRL3     0x18u   /* EN_OTG|PFM_*|BATFET_DLY|BATFET_CTRL  */
#define REG_STATUS0           0x1Du   /* WD_STAT in [0], VSYS_STAT in [4]     */
#define REG_STATUS1           0x1Eu   /* CHG_STAT [4:3], VBUS_STAT [2:0]      */
#define REG_FAULT_STATUS      0x1Fu   /* BAT/SYS/TSHUT/TS                     */
#define REG_ADC_CTRL          0x26u   /* ADC_EN[7] ADC_RATE[6] SAMPLE[5:4]    */
#define REG_IBUS_ADC_LO       0x28u
#define REG_TDIE_ADC_HI       0x37u   /* end of contiguous ADC block          */
#define REG_PART_INFO         0x38u   /* PN[5:3] in 0x1F mask; DEV_REV[2:0]   */

/* CTRL1 bitfields */
#define CTRL1_EN_AUTO_IBATDIS (1u << 7)
#define CTRL1_EN_CHG          (1u << 5)
#define CTRL1_EN_HIZ          (1u << 4)
#define CTRL1_WD_RST          (1u << 2)
#define CTRL1_WATCHDOG_MASK   (0x3u)

/* CTRL3 bitfields */
#define CTRL3_BATFET_CTRL_MASK (0x3u)     /* bits [1:0] */

/* ADC_CTRL value — ADC_EN=1, ADC_RATE=0 (continuous), SAMPLE=00 (12-bit),
 * ADC_AVG=0 (single), reserved bits=0. */
#define ADC_CTRL_PRODUCTION   0x80u

/* STATUS0 bits */
#define STATUS0_WD_STAT       (1u << 0)

/* Production targets — see bq25622.h module header. */
#define TARGET_VREG_MV        4100u
#define TARGET_ICHG_MA        480u
#define TARGET_IINDPM_MA      500u

/* Datasheet bit-step constants (§8.6.2.1/2/3). */
#define VREG_STEP_MV          10u
#define ICHG_STEP_MA          80u
#define IINDPM_STEP_MA        20u

/* Operational constraints. */
#define I2C_TIMEOUT_US        50000u
#define FAIL_DISCONNECT_COUNT 5u
#define POLL_PERIOD_MS        1000u
#define ADC_FIRST_CYCLE_MS    300u     /* §7.7 — first continuous cycle       */

/* ── Driver state ────────────────────────────────────────────────────────── */
static bq25622_state_t s_state = {
    .online     = false,
    .wd_window  = BQ25622_WD_50S,
};

/* ── Low-level helpers (caller holds the bus mutex) ──────────────────────── */

static int reg_write_u8(i2c_inst_t *bus, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_write_timeout_us(bus, BQ25622_ADDR, buf, 2, false,
                                I2C_TIMEOUT_US);
}

static int reg_write_u16(i2c_inst_t *bus, uint8_t reg_lo,
                         uint8_t lo_byte, uint8_t hi_byte)
{
    /* Single 3-byte transaction: START | ADDR+W | reg_lo | lo | hi | STOP.
     * BQ25622 auto-increments the internal pointer (§8.5.1.7), so the two
     * byte writes apply to reg_lo and reg_lo+1 atomically from the chip's
     * perspective. */
    uint8_t buf[3] = { reg_lo, lo_byte, hi_byte };
    return i2c_write_timeout_us(bus, BQ25622_ADDR, buf, 3, false,
                                I2C_TIMEOUT_US);
}

static int reg_read_u8(i2c_inst_t *bus, uint8_t reg, uint8_t *out)
{
    int r = i2c_write_timeout_us(bus, BQ25622_ADDR, &reg, 1, /*nostop=*/true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, BQ25622_ADDR, out, 1, false,
                               I2C_TIMEOUT_US);
}

/* Burst-read `len` bytes starting at `reg` (repeated-start, single read
 * transaction). Used for the ADC block (0x28..0x33 = 12 bytes) and the
 * status block (0x1D..0x1F = 3 bytes). */
static int reg_read_block(i2c_inst_t *bus, uint8_t reg,
                          uint8_t *out, size_t len)
{
    int r = i2c_write_timeout_us(bus, BQ25622_ADDR, &reg, 1, /*nostop=*/true,
                                 I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(bus, BQ25622_ADDR, out, len, false,
                               I2C_TIMEOUT_US);
}

/* Read-modify-write on CTRL1 preserving unknown bits. `clear_mask` bits are
 * forced to 0, `set_mask` bits are forced to 1. */
static int ctrl1_update(i2c_inst_t *bus, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t v;
    int r = reg_read_u8(bus, REG_CHARGER_CTRL1, &v);
    if (r < 0) return r;
    v = (uint8_t)((v & ~clear_mask) | set_mask);
    return reg_write_u8(bus, REG_CHARGER_CTRL1, v);
}

/* ── Field packing (SLUSEG2D §8.6.2.1/2/3 bit layouts) ───────────────────── */

static void pack_vreg(uint16_t mv, uint8_t *lo, uint8_t *hi)
{
    /* VREG is 9 bits, bit step 10 mV, range 3500..4800 mV. */
    uint16_t raw = mv / VREG_STEP_MV;
    *lo = (uint8_t)((raw & 0x1Fu) << 3);          /* VREG[4:0] → reg_lo[7:3] */
    *hi = (uint8_t)((raw >> 5) & 0x0Fu);          /* VREG[8:5] → reg_hi[3:0] */
}

static void pack_ichg(uint16_t ma, uint8_t *lo, uint8_t *hi)
{
    /* ICHG is 6 bits, bit step 80 mA, range 80..3520 mA. */
    uint16_t raw = ma / ICHG_STEP_MA;
    *lo = (uint8_t)((raw & 0x03u) << 6);          /* ICHG[1:0] → reg_lo[7:6] */
    *hi = (uint8_t)((raw >> 2) & 0x0Fu);          /* ICHG[5:2] → reg_hi[3:0] */
}

static void pack_iindpm(uint16_t ma, uint8_t *lo, uint8_t *hi)
{
    /* IINDPM is 8 bits, bit step 20 mA, range 100..3200 mA. */
    uint16_t raw = ma / IINDPM_STEP_MA;
    *lo = (uint8_t)((raw & 0x0Fu) << 4);          /* IINDPM[3:0] → reg_lo[7:4] */
    *hi = (uint8_t)((raw >> 4) & 0x0Fu);          /* IINDPM[7:4] → reg_hi[3:0] */
}

/* ── ADC block decoder (SLUSEG2D §8.6.2.30..37) ──────────────────────────── *
 *
 * The 16-byte block starting at 0x28 packs 8 little-endian 16-bit words:
 *     0x28/29 IBUS   bits[15:1]  signed  2 mA/step
 *     0x2A/2B IBAT   bits[15:2]  signed  4 mA/step
 *     0x2C/2D VBUS   bits[14:2]  usgn    3.97 mV/step
 *     0x2E/2F VPMID  bits[14:2]  usgn    3.97 mV/step
 *     0x30/31 VBAT   bits[12:1]  usgn    1.99 mV/step
 *     0x32/33 VSYS   bits[12:1]  usgn    1.99 mV/step
 *     0x34/35 TS     bits[11:0]  usgn    0.0961%/step (VREGN reference)
 *     0x36/37 TDIE   bits[11:0]  signed  0.5°C/step (2's complement)
 *
 * The raw bit widths are chosen so the low unused bit(s) decode as 0 and
 * arithmetic shift on signed raw values preserves sign extension.          */
#define ADC_BLOCK_BYTES   16u

static void decode_adc(const uint8_t *b, bq25622_state_t *s)
{
    const uint16_t ibus_raw = (uint16_t)(b[0]  | (b[1]  << 8));
    const uint16_t ibat_raw = (uint16_t)(b[2]  | (b[3]  << 8));
    const uint16_t vbus_raw = (uint16_t)(b[4]  | (b[5]  << 8));
    const uint16_t vpmid_raw= (uint16_t)(b[6]  | (b[7]  << 8));
    const uint16_t vbat_raw = (uint16_t)(b[8]  | (b[9]  << 8));
    const uint16_t vsys_raw = (uint16_t)(b[10] | (b[11] << 8));
    const uint16_t ts_raw   = (uint16_t)(b[12] | (b[13] << 8)) & 0x0FFFu;
    uint16_t       tdie_raw = (uint16_t)(b[14] | (b[15] << 8)) & 0x0FFFu;

    s->ibus_ma  = (int16_t)(((int16_t)ibus_raw >> 1) * 2);
    s->ibat_ma  = (int16_t)(((int16_t)ibat_raw >> 2) * 4);
    s->vbus_mv  = (uint16_t)(((vbus_raw  >> 2) & 0x1FFFu) * 397u / 100u);
    s->vpmid_mv = (uint16_t)(((vpmid_raw >> 2) & 0x1FFFu) * 397u / 100u);
    s->vbat_mv  = (uint16_t)(((vbat_raw  >> 1) & 0x0FFFu) * 199u / 100u);
    s->vsys_mv  = (uint16_t)(((vsys_raw  >> 1) & 0x0FFFu) * 199u / 100u);

    /* TS: 12-bit unsigned, 0.0961 % / LSB → percent × 10 for display */
    s->ts_pct_x10 = (uint16_t)((ts_raw * 961u) / 1000u);

    /* TDIE: 12-bit 2's complement, 0.5 °C / LSB. Sign-extend bit 11. */
    if (tdie_raw & 0x0800u) tdie_raw |= 0xF000u;
    s->tdie_cx10 = (int16_t)((int16_t)tdie_raw * 5);
}

/* ── Hardware init (datasheet §8.5 + §8.6) ───────────────────────────────── *
 *
 * Caller holds the bus mutex. Returns 0 on success, negative on I2C error.
 *
 * Sequence:
 *   1. Verify PART_INFO  — PN[5:3] = 001 means BQ25622
 *   2. Program 16-bit regs (3-byte multi-writes):
 *        VREG / ICHG / IINDPM
 *   3. RMW CTRL1 — set WATCHDOG field + kick WD_RST, ensure EN_CHG stays 1
 *   4. Enable ADC — continuous 12-bit                                         */
static int hw_init(i2c_inst_t *bus, bq25622_wd_window_t wd)
{
    int r;

    /* 1. PART_INFO check */
    uint8_t pi;
    r = reg_read_u8(bus, REG_PART_INFO, &pi);
    if (r < 0) return r;
    const uint8_t pn = (uint8_t)((pi >> 3) & 0x07u);
    if (pn != 0x01u) return -1;   /* wrong part — refuse to init */

    /* 2. Charge regulation limits */
    uint8_t lo, hi;
    pack_vreg(TARGET_VREG_MV, &lo, &hi);
    if ((r = reg_write_u16(bus, REG_VREG_LO, lo, hi)) < 0) return r;

    pack_ichg(TARGET_ICHG_MA, &lo, &hi);
    if ((r = reg_write_u16(bus, REG_ICHG_LO, lo, hi)) < 0) return r;

    pack_iindpm(TARGET_IINDPM_MA, &lo, &hi);
    if ((r = reg_write_u16(bus, REG_IINDPM_LO, lo, hi)) < 0) return r;

    /* 3. CTRL1 — clear WATCHDOG field, set requested value + kick + EN_CHG.
     *    EN_AUTO_IBATDIS is POR-default 1 (battery OVP discharge); preserve
     *    it via RMW rather than blind write. */
    const uint8_t clear = (uint8_t)(CTRL1_WATCHDOG_MASK);
    const uint8_t set   = (uint8_t)(CTRL1_EN_CHG | CTRL1_WD_RST | (uint8_t)wd);
    if ((r = ctrl1_update(bus, clear, set)) < 0) return r;

    /* 4. ADC */
    if ((r = reg_write_u8(bus, REG_ADC_CTRL, ADC_CTRL_PRODUCTION)) < 0)
        return r;

    return 0;
}

/* ── Poll cycle ──────────────────────────────────────────────────────────── *
 *
 * Acquires bus, kicks WD, reads status+ADC, publishes. Handles WD expiry
 * by re-running hw_init() in place. Increments i2c_fail_count on error. */
static void poll_once(bq25622_state_t *s)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return;   /* should never happen with portMAX_DELAY */

    int r;

    /* STATUS block: 0x1D..0x1F (WD_STAT, CHG/VBUS, FAULT) */
    uint8_t st[3];
    if ((r = reg_read_block(bus, REG_STATUS0, st, 3)) < 0)
        goto fail;

    const bool wd_expired = (st[0] & STATUS0_WD_STAT) != 0u;
    if (wd_expired) {
        /* §8.4.1 — WD fired: ICHG halved, several CTRL1 bits reset. Recover
         * by re-applying every configured value. Fresh hw_init() inherently
         * kicks WD_RST and sets the configured WATCHDOG window. */
        if ((r = hw_init(bus, s->wd_window)) < 0) goto fail;
        s->wd_expired_count++;
        /* Re-read status after recovery so WD_STAT reflects post-kick state. */
        if ((r = reg_read_block(bus, REG_STATUS0, st, 3)) < 0) goto fail;
    } else if (s->wd_window != BQ25622_WD_OFF) {
        /* Regular kick — WD_RST auto-clears after the timer reset.          */
        if ((r = ctrl1_update(bus, 0, CTRL1_WD_RST)) < 0) goto fail;
    }

    /* Publish status bits */
    s->chg_stat  = (uint8_t)((st[1] >> 3) & 0x03u);
    s->vbus_stat = (uint8_t)(st[1] & 0x07u);
    s->ts_stat   = (uint8_t)(st[2] & 0x07u);
    s->tshut     = ((st[2] >> 3) & 1u) != 0u;
    s->sys_fault = ((st[2] >> 5) & 1u) != 0u;
    s->bat_fault = ((st[2] >> 6) & 1u) != 0u;

    /* ADC block: 16 bytes from 0x28 (IBUS..TDIE) */
    uint8_t adc[ADC_BLOCK_BYTES];
    if ((r = reg_read_block(bus, REG_IBUS_ADC_LO, adc, ADC_BLOCK_BYTES)) < 0)
        goto fail;
    decode_adc(adc, s);

    s->i2c_fail_count = 0;
    s->online = true;
    i2c_bus_release(MOKYA_I2C_POWER);
    return;

fail:
    i2c_bus_release(MOKYA_I2C_POWER);
    if (s->i2c_fail_count < UINT32_MAX) s->i2c_fail_count++;
    if (s->i2c_fail_count >= FAIL_DISCONNECT_COUNT) s->online = false;
}

/* ── FreeRTOS task ───────────────────────────────────────────────────────── */

static void charger_task(void *pv)
{
    (void)pv;

    /* Initial hardware setup — retry until success so hot-plug / brown-out
     * scenarios don't leave the driver permanently offline. */
    for (;;) {
        i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
        int r = hw_init(bus, s_state.wd_window);
        i2c_bus_release(MOKYA_I2C_POWER);
        if (r == 0) {
            s_state.online = true;
            break;
        }
        s_state.i2c_fail_count++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* First full ADC cycle completes within ~300 ms of ADC_EN=1. */
    vTaskDelay(pdMS_TO_TICKS(ADC_FIRST_CYCLE_MS));

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        poll_once(&s_state);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool bq25622_start_task(UBaseType_t priority)
{
    /* 512 words (2 KB) — driver uses < 100 B locals + ~200 B for i2c SDK /
     * semaphore call frames. Leaves headroom for future sensor / LM27965
     * tasks within the 32 KB FreeRTOS heap. */
    BaseType_t rc = xTaskCreate(charger_task, "chg", 512, NULL, priority, NULL);
    return rc == pdPASS;
}

const bq25622_state_t *bq25622_get_state(void)
{
    return &s_state;
}

bool bq25622_set_charge_enabled(bool enabled)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    const uint8_t clear = enabled ? 0u              : CTRL1_EN_CHG;
    const uint8_t set   = enabled ? (uint8_t)(CTRL1_EN_CHG | CTRL1_WD_RST)
                                  : CTRL1_WD_RST;
    int r = ctrl1_update(bus, clear, set);

    i2c_bus_release(MOKYA_I2C_POWER);
    return r >= 0;
}

bool bq25622_set_watchdog(bq25622_wd_window_t win)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    /* Change WATCHDOG field and kick WD_RST in the same write so the old
     * timer doesn't expire mid-transition. */
    const uint8_t clear = CTRL1_WATCHDOG_MASK;
    const uint8_t set   = (uint8_t)(CTRL1_WD_RST | (uint8_t)win);
    int r = ctrl1_update(bus, clear, set);

    i2c_bus_release(MOKYA_I2C_POWER);
    if (r < 0) return false;

    s_state.wd_window = win;
    return true;
}

bool bq25622_set_hiz(bool enabled)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    /* Kick WD in the same write so this long-lived RMW doesn't race the
     * watchdog. */
    const uint8_t clear = enabled ? 0u           : CTRL1_EN_HIZ;
    const uint8_t set   = enabled ? (uint8_t)(CTRL1_EN_HIZ | CTRL1_WD_RST)
                                  : CTRL1_WD_RST;
    int r = ctrl1_update(bus, clear, set);

    i2c_bus_release(MOKYA_I2C_POWER);
    return r >= 0;
}

bool bq25622_set_batfet_mode(bq25622_batfet_mode_t mode)
{
    i2c_inst_t *bus = i2c_bus_acquire(MOKYA_I2C_POWER, portMAX_DELAY);
    if (bus == NULL) return false;

    /* RMW CTRL3[1:0]. BATFET_DLY (bit 2) left at POR 1 = 12.5 s delay,
     * giving the host time to finish shutdown work before the action
     * actually takes effect. */
    uint8_t v;
    int r = reg_read_u8(bus, REG_CHARGER_CTRL3, &v);
    if (r >= 0) {
        v = (uint8_t)((v & (uint8_t)~CTRL3_BATFET_CTRL_MASK)
                      | ((uint8_t)mode & CTRL3_BATFET_CTRL_MASK));
        r = reg_write_u8(bus, REG_CHARGER_CTRL3, v);
    }

    i2c_bus_release(MOKYA_I2C_POWER);
    return r >= 0;
}
