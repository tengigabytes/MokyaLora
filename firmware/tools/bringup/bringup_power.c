#include "bringup.h"

// ---------------------------------------------------------------------------
// LM27965 helpers
// ---------------------------------------------------------------------------

int lm_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(i2c1, LM27965_ADDR, buf, 2, false, 50000);
}

static int lm_read(uint8_t reg, uint8_t *val) {
    int r = i2c_write_timeout_us(i2c1, LM27965_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c1, LM27965_ADDR, val, 1, false, 50000);
}

void lm27965_cycle(void) {
    printf("\n--- LM27965 LED Cycle (non-LCD) ---\n");

    // 40 % brightness = 12.1 mA/pin (safe for all attached LEDs)
    lm_write(LM27965_BANKA, 0x16);
    lm_write(LM27965_BANKB, 0x16);
    lm_write(LM27965_BANKC, 0xFD);  // bits[1:0] = 01 = 40 %

    // GP bit assignments: bit0=ENA(TFT,skip) bit1=ENB(Key D1B+D2B) bit2=ENC(D1C red)
    //                     bit3=EN5A  bit4=EN3B(D3B green)  bit5=reserved(keep 1)
    // ENB(bit1) must be set for any Bank B output (required for D3B green too).
    // EN3B(bit4) gates only D3B; D1B+D2B are on whenever ENB=1.
    typedef struct { uint8_t gp; const char *name; } step_t;
    step_t seq[] = {
        { 0x22, "Keyboard backlight (D1B+D2B)"          },
        { 0x24, "Red indicator (D1C)"                   },
        { 0x32, "Green indicator (D3B) + Keyboard"      },
        { 0x36, "All: Keyboard + Red + Green"           },
    };

    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < 4; i++) {
            lm_write(LM27965_GP, seq[i].gp);
            printf("  [%d] %s\n", cycle, seq[i].name);
            sleep_ms(600);
        }
        lm_write(LM27965_GP, 0x20);  // brief off
        sleep_ms(200);
    }

    lm_write(LM27965_GP, 0x20);
    printf("Done - LEDs off\n");
}

// ---------------------------------------------------------------------------
// Power bus (Bus B) init / deinit
// ---------------------------------------------------------------------------

void bus_b_init(void) {
    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_B_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_B_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_B_SDA);
    gpio_pull_up(BUS_B_SCL);
}

void bus_b_deinit(void) {
    i2c_deinit(i2c1);
    gpio_set_function(BUS_B_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_B_SCL, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// BQ25622 helpers
// ---------------------------------------------------------------------------

static int bq_read(uint8_t reg, uint8_t *val) {
    int r = i2c_write_timeout_us(i2c1, BQ25622_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c1, BQ25622_ADDR, val, 1, false, 50000);
}

static int bq_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(i2c1, BQ25622_ADDR, buf, 2, false, 50000);
}

// BQ25622 VBUS_STAT[2:0] decode (SLUSEG2D Table 8-28, BQ25622 only)
// BQ25622 valid values: 000=not powered, 100=unknown adapter, 111=OTG
static const char *const bq_vbus_str[] = {
    "Not powered",       // 000
    "reserved(001)",     // 001
    "reserved(010)",     // 010
    "reserved(011)",     // 011
    "Unknown Adapter",   // 100 — default IINDPM setting
    "reserved(101)",     // 101
    "reserved(110)",     // 110
    "OTG boost",         // 111
};

// BQ25622 CHG_STAT[4:3] decode (SLUSEG2D Table 8-28)
static const char *const bq_chg_str[] = {
    "Not Charging",      // 00
    "CC (Trickle/Pre/Fast)",  // 01
    "Taper (CV)",        // 10
    "Top-off",           // 11
};

void bq25622_print_status(void) {
    uint8_t v;
    printf("\n--- BQ25622 Status ---\n");

    if (bq_read(REG_PART_INFO, &v) >= 0) {
        uint8_t pn = (v >> 3) & 0x7;
        printf("Part Info  (0x38): 0x%02X  PN=%s  REV=%d\n",
               v, pn == 1 ? "BQ25622" : (pn == 0 ? "BQ25620" : "???"), v & 0x7);
    }
    if (bq_read(REG_STATUS0, &v) >= 0) {
        printf("Status0    (0x1D): 0x%02X  ADC_DONE=%d  TREG=%d  VSYS=%s  IINDPM=%d  VINDPM=%d  SAFETY_TMR=%d  WD=%d\n",
               v,
               (v >> 6) & 1,
               (v >> 5) & 1,
               (v >> 4) & 1 ? "BAT<VSYSMIN!" : "OK",
               (v >> 3) & 1,
               (v >> 2) & 1,
               (v >> 1) & 1,
               v & 1);
    }
    if (bq_read(REG_STATUS1, &v) >= 0) {
        printf("Status1    (0x1E): 0x%02X  CHG=%s  VBUS=%s\n",
               v, bq_chg_str[(v >> 3) & 0x3], bq_vbus_str[v & 0x7]);
    }
    if (bq_read(REG_FAULT_STATUS, &v) >= 0) {
        const char *ts_str[] = {"NORMAL", "COLD/OTG_COLD/no-bias", "HOT/OTG_HOT", "COOL",
                                 "WARM", "PRECOOL", "PREWARM", "TS-bias-fault"};
        printf("Fault      (0x1F): 0x%02X  BAT_FAULT=%d  SYS_FAULT=%d  TSHUT=%d  TS=%s\n",
               v, (v >> 6) & 1, (v >> 5) & 1, (v >> 3) & 1, ts_str[v & 0x7]);
    }
    if (bq_read(REG_CHARGER_CTRL1, &v) >= 0) {
        printf("ChgCtrl1   (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
               v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
    }
    if (bq_read(REG_NTC_CTRL0, &v) >= 0) {
        printf("NTC_Ctrl0  (0x1A): 0x%02X  TS_IGNORE=%d\n", v, (v >> 7) & 1);
    }
    if (bq_read(REG_CHARGER_CTRL3, &v) >= 0) {
        printf("ChgCtrl3   (0x18): 0x%02X  BATFET_CTRL=%d(%s)  BATFET_DLY=%d\n",
               v, v & 0x3,
               (v & 0x3) == 0 ? "Normal" :
               (v & 0x3) == 1 ? "Shutdown" :
               (v & 0x3) == 2 ? "Ship" : "SysRst",
               (v >> 2) & 1);
    }
    if (bq_read(REG_FAULT_FLAG0, &v) >= 0) {
        printf("FaultFlag  (0x22): 0x%02X\n", v);
    }
}

void bq25622_disable_charge(void) {
    uint8_t v;
    printf("\n--- BQ25622: Disabling Charge ---\n");
    bq_write(REG_CHARGER_CTRL1, 0x84);  // kick WD_RST first
    sleep_ms(10);
    bq_write(REG_CHARGER_CTRL1, 0x80);  // EN_CHG=0, WATCHDOG disabled
    bq_read(REG_CHARGER_CTRL1, &v);
    printf("ChgCtrl1 (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
           v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
}

void bq25622_enable_charge(void) {
    uint8_t v, vlo, vhi;
    printf("\n--- BQ25622: Enabling Charge ---\n");

    // 1. Set VREG = 4100 mV
    // VREG_RAW = 4100 / 10 = 410 = 0x19A (9-bit)
    // VREG[4:0] = 0x1A → REG0x04[7:3] = 0xD0; VREG[8:5] = 0x0C → REG0x05[3:0] = 0x0C
    bq_write(REG_VREG_LO, 0xD0);
    bq_write(REG_VREG_HI, 0x0C);
    bq_read(REG_VREG_LO, &vlo);
    bq_read(REG_VREG_HI, &vhi);
    uint16_t vreg_raw = (uint16_t)(((vhi & 0x0Fu) << 5) | ((vlo >> 3) & 0x1Fu));
    printf("VREG:   lo=0x%02X hi=0x%02X → %u mV\n", vlo, vhi, (unsigned)(vreg_raw * 10u));

    // 2. Set IINDPM = 100 mA
    // IINDPM_RAW = 100 / 20 = 5 (8-bit)
    // IINDPM[3:0] = 5 → REG0x06[7:4] = 0x50; IINDPM[7:4] = 0 → REG0x07[3:0] = 0x00
    bq_write(REG_IINDPM_LO, 0x50);
    bq_write(REG_IINDPM_HI, 0x00);
    bq_read(REG_IINDPM_LO, &vlo);
    bq_read(REG_IINDPM_HI, &vhi);
    uint16_t iindpm_raw = (uint16_t)(((vhi & 0x0Fu) << 4) | ((vlo >> 4) & 0x0Fu));
    printf("IINDPM: lo=0x%02X hi=0x%02X → %u mA\n", vlo, vhi, (unsigned)(iindpm_raw * 20u));

    // 3. Enable charging
    bq_write(REG_CHARGER_CTRL1, 0xA4);  // kick WD_RST
    sleep_ms(10);
    bq_write(REG_CHARGER_CTRL1, 0xA1);  // EN_CHG=1, WATCHDOG=01
    bq_read(REG_CHARGER_CTRL1, &v);
    printf("ChgCtrl1 (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
           v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
}

// ---------------------------------------------------------------------------
// BQ25622 ADC readout
// ---------------------------------------------------------------------------

void bq25622_read_adc(void) {
    uint8_t lo, hi;
    uint16_t raw16;
    printf("\n--- BQ25622 ADC Readings ---\n");

    // Enable ADC: continuous, 12-bit resolution (ADC_SAMPLE=00), all channels on
    bq_write(REG_ADC_CTRL, 0x80);
    uint8_t adc_ctrl, adc_dis;
    bq_read(REG_ADC_CTRL, &adc_ctrl);
    bq_read(REG_ADC_FUNC_DIS, &adc_dis);
    printf("ADC_CTRL (0x26): 0x%02X  EN=%d RATE=%s SAMPLE=%ubit\n",
           adc_ctrl, (adc_ctrl >> 7) & 1,
           (adc_ctrl >> 6) & 1 ? "one-shot" : "continuous",
           12u - ((adc_ctrl >> 4) & 0x3u));  // 00→12bit 01→11bit 10→10bit 11→9bit
    printf("ADC_DIS  (0x27): 0x%02X  IBUS=%d IBAT=%d VBUS=%d VBAT=%d VSYS=%d TS=%d TDIE=%d VPMID=%d\n",
           adc_dis,
           (adc_dis >> 7) & 1, (adc_dis >> 6) & 1, (adc_dis >> 5) & 1, (adc_dis >> 4) & 1,
           (adc_dis >> 3) & 1, (adc_dis >> 2) & 1, (adc_dis >> 1) & 1, adc_dis & 1);
    sleep_ms(300);  // 12-bit continuous mode: wait for full channel-scan cycle

    // IBUS: bits[15:1] 2s-complement, 2 mA/step
    bq_read(REG_IBUS_ADC_LO, &lo); bq_read(REG_IBUS_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    int ibus_ma = (int)((int16_t)raw16 >> 1) * 2;
    printf("  IBUS:  %+d mA  (raw 0x%04X)\n", ibus_ma, raw16);

    // IBAT: bits[15:2] 2s-complement, 4 mA/step
    bq_read(REG_IBAT_ADC_LO, &lo); bq_read(REG_IBAT_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    int ibat_ma = (int)((int16_t)raw16 >> 2) * 4;
    printf("  IBAT:  %+d mA  (raw 0x%04X)\n", ibat_ma, raw16);

    // VBUS: bits[14:2] unsigned, 3.97 mV/step
    bq_read(REG_VBUS_ADC_LO, &lo); bq_read(REG_VBUS_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vbus_mv = (unsigned)(((raw16 >> 2) & 0x1FFFu) * 397u / 100u);
    printf("  VBUS:  %u mV  (raw 0x%04X)\n", vbus_mv, raw16);

    // VPMID: bits[14:2] unsigned, 3.97 mV/step
    bq_read(REG_VPMID_ADC_LO, &lo); bq_read(REG_VPMID_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vpmid_mv = (unsigned)(((raw16 >> 2) & 0x1FFFu) * 397u / 100u);
    printf("  VPMID: %u mV  (raw 0x%04X)\n", vpmid_mv, raw16);

    // VBAT: bits[12:1] unsigned, 1.99 mV/step
    bq_read(REG_VBAT_ADC_LO, &lo); bq_read(REG_VBAT_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vbat_mv = (unsigned)(((raw16 >> 1) & 0x0FFFu) * 199u / 100u);
    printf("  VBAT:  %u mV  (raw 0x%04X)\n", vbat_mv, raw16);

    // VSYS: bits[12:1] unsigned, 1.99 mV/step
    bq_read(REG_VSYS_ADC_LO, &lo); bq_read(REG_VSYS_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vsys_mv = (unsigned)(((raw16 >> 1) & 0x0FFFu) * 199u / 100u);
    printf("  VSYS:  %u mV  (raw 0x%04X)\n", vsys_mv, raw16);
}

// ---------------------------------------------------------------------------
// BQ27441 fuel gauge (Bus B, 0x55)
// Reference: SLUSBH1C (datasheet) + SLUUAC9A (TRM)
// ---------------------------------------------------------------------------

// Read a 16-bit standard command register (little-endian).
static int fg_read16(uint8_t reg, uint16_t *val) {
    uint8_t buf[2];
    int r = i2c_write_timeout_us(i2c1, BQ27441_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    r = i2c_read_timeout_us(i2c1, BQ27441_ADDR, buf, 2, false, 50000);
    if (r < 0) return r;
    *val = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return 0;
}

// Send a CONTROL() sub-command (write only, no result read).
static int fg_ctrl_write(uint16_t subcmd) {
    uint8_t buf[3] = {0x00, (uint8_t)(subcmd & 0xFFu), (uint8_t)(subcmd >> 8)};
    return i2c_write_timeout_us(i2c1, BQ27441_ADDR, buf, 3, false, 50000);
}

// Send a CONTROL() sub-command and read back the 16-bit result.
// ≥66 µs bus-free time between write and read required (SLUSBH1C §8.5.1.1).
static int fg_ctrl_read(uint16_t subcmd, uint16_t *result) {
    if (fg_ctrl_write(subcmd) < 0) return -1;
    sleep_us(100);
    uint8_t reg = 0x00;
    uint8_t buf[2];
    if (i2c_write_timeout_us(i2c1, BQ27441_ADDR, &reg, 1, true, 50000) < 0) return -1;
    if (i2c_read_timeout_us(i2c1, BQ27441_ADDR, buf, 2, false, 50000) < 0) return -1;
    *result = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return 0;
}

void bq27441_read(void) {
    printf("\n--- BQ27441 Fuel Gauge (Step 12) ---\n");
    bus_b_init();

    // 1. Probe: any directed I2C transaction wakes gauge from SLEEP/HIBERNATE
    //    via ≤100 µs clock stretch (SLUSBH1C p.14). Read CONTROL_STATUS first.
    //    If NACK: attempt I2C bus recovery (9 SCL clocks + STOP) and retry.
    //    BQ27441 I2C engine can lock up during cold boot when SDA/SCL are held
    //    low by MCU pull-downs before the 1.8V pull-up rail is active.
    uint16_t cs;
    int probe_ok = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (fg_ctrl_read(BQ27441_CTRL_STATUS, &cs) >= 0) {
            probe_ok = 1;
            break;
        }
        printf("  NACK at 0x55 (attempt %d/3) — I2C bus recovery...\n", attempt + 1);
        // Deinit I2C peripheral so we can bitbang GPIO for recovery
        bus_b_deinit();
        // 9 SCL clocks + STOP per UM10204 §3.1.16
        const uint scl = BUS_B_SCL;
        const uint sda = BUS_B_SDA;
        gpio_init(scl);
        gpio_set_dir(scl, GPIO_OUT);
        gpio_put(scl, 1);
        gpio_init(sda);
        gpio_set_dir(sda, GPIO_IN);
        gpio_pull_up(sda);
        sleep_us(10);
        for (int i = 0; i < 9; i++) {
            gpio_put(scl, 0);
            sleep_us(5);
            gpio_put(scl, 1);
            sleep_us(5);
            if (gpio_get(sda)) break;
        }
        // STOP condition: SDA LOW→HIGH while SCL HIGH
        gpio_set_dir(sda, GPIO_OUT);
        gpio_put(sda, 0);
        sleep_us(5);
        gpio_put(scl, 1);
        sleep_us(5);
        gpio_put(sda, 1);
        sleep_us(10);
        // Release pins
        gpio_set_dir(scl, GPIO_IN);
        gpio_set_dir(sda, GPIO_IN);
        gpio_disable_pulls(scl);
        gpio_disable_pulls(sda);
        sleep_ms(100);  // let gauge settle
        // Reinit I2C
        bus_b_init();
    }
    if (!probe_ok) {
        printf("NACK — BQ27441 not responding at 0x55 after 3 recovery attempts\n");
        printf("Check: battery installed? BAT pin voltage > 2 V (UVLOIT)?\n");
        bus_b_deinit();
        return;
    }

    // 2. Decode CONTROL_STATUS
    int initcomp  = (cs & BQ27441_CS_INITCOMP)  ? 1 : 0;
    int hibernate = (cs & BQ27441_CS_HIBERNATE) ? 1 : 0;
    int sleep_on  = (cs & BQ27441_CS_SLEEP)     ? 1 : 0;
    int sealed    = (cs & BQ27441_CS_SEALED)    ? 1 : 0;
    printf("CTRL_STATUS:  0x%04X  INITCOMP=%d  HIBERNATE=%d  SLEEP=%d  SEALED=%d\n",
           cs, initcomp, hibernate, sleep_on, sealed);

    // 3. If HIBERNATE: force exit with CLEAR_HIBERNATE (0x0012)
    if (hibernate) {
        printf("  HIBERNATE active — sending CLEAR_HIBERNATE...\n");
        fg_ctrl_write(BQ27441_CTRL_CLR_HIB);
        sleep_ms(100);
        fg_ctrl_read(BQ27441_CTRL_STATUS, &cs);
        initcomp  = (cs & BQ27441_CS_INITCOMP)  ? 1 : 0;
        hibernate = (cs & BQ27441_CS_HIBERNATE) ? 1 : 0;
        printf("  CTRL_STATUS after clear: 0x%04X  HIBERNATE=%d\n", cs, hibernate);
    }

    // 4. Confirm device type
    uint16_t dev_type;
    if (fg_ctrl_read(BQ27441_CTRL_DEVTYPE, &dev_type) >= 0)
        printf("DEVICE_TYPE:  0x%04X  %s\n", dev_type,
               dev_type == 0x0421 ? "BQ27441-G1 ✓" : "unexpected — check device");

    // 5. Flags register
    uint16_t flags = 0;
    if (fg_read16(BQ27441_REG_FLAGS, &flags) >= 0) {
        int itpor    = (flags & BQ27441_FLAG_ITPOR)    ? 1 : 0;
        int bat_det  = (flags & BQ27441_FLAG_BAT_DET)  ? 1 : 0;
        int ocvtaken = (flags & BQ27441_FLAG_OCVTAKEN) ? 1 : 0;
        int fc       = (flags & BQ27441_FLAG_FC)       ? 1 : 0;
        int chg      = (flags & BQ27441_FLAG_CHG)      ? 1 : 0;
        int dsg      = (flags & BQ27441_FLAG_DSG)      ? 1 : 0;
        int soc1     = (flags & BQ27441_FLAG_SOC1)     ? 1 : 0;
        int socf     = (flags & BQ27441_FLAG_SOCF)     ? 1 : 0;
        printf("FLAGS:        0x%04X  ITPOR=%d  BAT_DET=%d  OCVTAKEN=%d"
               "  FC=%d  CHG=%d  DSG=%d  SOC1=%d  SOCF=%d\n",
               flags, itpor, bat_det, ocvtaken, fc, chg, dsg, soc1, socf);
        if (itpor)
            printf("  NOTE: ITPOR=1 — RAM reset to ROM defaults; Design Capacity = ROM default\n");
        if (!bat_det) {
            // Try BAT_INSERT first — works only when BIE=0 (SLUSBH1C §8.6.1.1.4)
            fg_ctrl_write(0x000C);
            sleep_ms(200);
            fg_read16(BQ27441_REG_FLAGS, &flags);
            bat_det = (flags & BQ27441_FLAG_BAT_DET) ? 1 : 0;

            if (!bat_det) {
                // BIE=1 (hardware default) — BAT_INSERT ignored when BIN not connected.
                // Clear BIE via CONFIG UPDATE so software battery detection works.
                // Reference: SLUUAC9A §3.1 (Extended Data Commands)
                printf("  BAT_DET=0 (BIE=1, BIN unconnected) — CONFIG UPDATE to clear BIE...\n");

                // --- Enter CFGUPDATE mode ---
                fg_ctrl_write(0x0013);  // SET_CFGUPDATE
                sleep_ms(10);

                // Poll FLAGS[CFGUPMODE] = bit4
                uint16_t f2;
                int cfgup = 0;
                for (int i = 0; i < 50 && !cfgup; i++) {
                    sleep_ms(100);
                    if (fg_read16(BQ27441_REG_FLAGS, &f2) >= 0 && (f2 & (1u << 4)))
                        cfgup = 1;
                }
                if (!cfgup) {
                    printf("  ERROR: CFGUPMODE not set — cannot update OpConfig\n");
                } else {
                    printf("  CFGUPMODE set. Reading OpConfig (subclass 64, block 0)...\n");

                    // Enable block data: BlockDataControl(0x61) = 0x00
                    uint8_t bdc[2] = {0x61, 0x00};
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, bdc, 2, false, 50000);
                    sleep_ms(2);

                    // DataClass(0x3E) = 64
                    uint8_t dc[2] = {0x3E, 64};
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, dc, 2, false, 50000);
                    sleep_ms(2);

                    // DataBlock(0x3F) = 0 (offsets 0–31)
                    uint8_t db[2] = {0x3F, 0};
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, db, 2, false, 50000);
                    sleep_ms(4);  // wait for block buffer load

                    // Read 32 bytes BlockData (0x40–0x5F)
                    uint8_t block[32];
                    uint8_t reg = 0x40;
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, &reg, 1, true, 50000);
                    i2c_read_timeout_us(i2c1, BQ27441_ADDR, block, 32, false, 50000);

                    // Read existing checksum (0x60)
                    uint8_t csreg = 0x60, old_cs;
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, &csreg, 1, true, 50000);
                    i2c_read_timeout_us(i2c1, BQ27441_ADDR, &old_cs, 1, false, 50000);

                    // Verify: checksum = 0xFF - (sum_of_32_bytes & 0xFF)
                    uint32_t sum = 0;
                    for (int i = 0; i < 32; i++) sum += block[i];
                    uint8_t calc_cs = (uint8_t)(0xFF - (sum & 0xFF));
                    printf("  OpConfig raw: 0x%02X%02X  Checksum read=0x%02X calc=0x%02X %s\n",
                           block[0], block[1], old_cs, calc_cs,
                           old_cs == calc_cs ? "OK" : "MISMATCH");

                    // OpConfig is 16-bit big-endian at offset 0:
                    //   block[0] = bits[15:8], block[1] = bits[7:0]
                    // BIE = bit13 = bit5 of block[0]; mask = 0x20
                    uint8_t old0 = block[0];
                    block[0] &= ~0x20u;  // clear BIE
                    printf("  OpConfig: 0x%02X%02X -> 0x%02X%02X  (BIE %d->0)\n",
                           old0, block[1], block[0], block[1], (old0 >> 5) & 1);

                    // Recompute checksum
                    sum = 0;
                    for (int i = 0; i < 32; i++) sum += block[i];
                    uint8_t new_cs = (uint8_t)(0xFF - (sum & 0xFF));

                    // Write modified block back (reg 0x40 + 32 bytes, one transaction)
                    uint8_t wbuf[33];
                    wbuf[0] = 0x40;
                    for (int i = 0; i < 32; i++) wbuf[1 + i] = block[i];
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, wbuf, 33, false, 50000);
                    sleep_ms(2);

                    // Write new checksum (0x60)
                    uint8_t cs_wbuf[2] = {0x60, new_cs};
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, cs_wbuf, 2, false, 50000);
                    sleep_ms(100);
                    printf("  New checksum: 0x%02X\n", new_cs);

                    // Exit CONFIG UPDATE with SOFT_RESET (clears ITPOR, re-runs OCV sim)
                    printf("  SOFT_RESET to apply...\n");
                    fg_ctrl_write(0x0042);  // SOFT_RESET
                    sleep_ms(2000);

                    // Poll CFGUPMODE=0 to confirm exit
                    int exited = 0;
                    for (int i = 0; i < 30 && !exited; i++) {
                        sleep_ms(100);
                        if (fg_read16(BQ27441_REG_FLAGS, &f2) >= 0 && !(f2 & (1u << 4)))
                            exited = 1;
                    }
                    printf("  CONFIG UPDATE %s\n", exited ? "exit OK" : "exit TIMEOUT");

                    // Now BIE=0 — BAT_INSERT works
                    sleep_ms(200);
                    fg_ctrl_write(0x000C);  // BAT_INSERT
                    sleep_ms(500);
                    fg_read16(BQ27441_REG_FLAGS, &flags);
                    bat_det  = (flags & BQ27441_FLAG_BAT_DET) ? 1 : 0;
                    initcomp = (flags >> 5) & 1;  // re-read after soft-reset cleared ITPOR
                    printf("  FLAGS after CONFIG UPDATE + BAT_INSERT: 0x%04X  BAT_DET=%d\n",
                           flags, bat_det);
                    if (!bat_det)
                        printf("  WARNING: BAT_DET still 0 — check battery connection\n");
                }
            }
        }
    }

    // Re-read CTRL_STATUS in case SOFT_RESET changed initcomp
    if (fg_ctrl_read(BQ27441_CTRL_STATUS, &cs) >= 0)
        initcomp = (cs & BQ27441_CS_INITCOMP) ? 1 : 0;

    // 6. Wait for INITCOMP if needed (max 10 s; required before gauging is valid)
    if (!initcomp) {
        printf("  INITCOMP=0 — waiting up to 10 s for gauge initialisation...\n");
        for (int i = 0; i < 50; i++) {
            sleep_ms(200);
            if (fg_ctrl_read(BQ27441_CTRL_STATUS, &cs) >= 0
                    && (cs & BQ27441_CS_INITCOMP)) {
                initcomp = 1;
                printf("  INITCOMP set after %d ms\n", (i + 1) * 200);
                break;
            }
        }
        if (!initcomp)
            printf("  ⚠ INITCOMP still 0 after 10 s — readings shown as-is\n");
    }

    // 7. Standard readings
    uint16_t raw;
    printf("\n");

    if (fg_read16(BQ27441_REG_VOLT, &raw) >= 0)
        printf("Voltage:       %u mV\n", raw);

    if (fg_read16(BQ27441_REG_AVGCUR, &raw) >= 0)
        printf("AvgCurrent:    %+d mA\n", (int)(int16_t)raw);

    if (fg_read16(BQ27441_REG_REMCAP, &raw) >= 0)
        printf("RemCapacity:   %u mAh\n", raw);

    if (fg_read16(BQ27441_REG_FULLCAP, &raw) >= 0)
        printf("FullCapacity:  %u mAh\n", raw);

    if (fg_read16(BQ27441_REG_SOC, &raw) >= 0)
        printf("StateOfCharge: %u %%\n", raw);

    if (fg_read16(BQ27441_REG_TEMP, &raw) >= 0) {
        float temp_c = (float)raw * 0.1f - 273.15f;
        printf("Temperature:   %.1f °C\n", temp_c);
    }

    if (fg_read16(BQ27441_REG_SOH, &raw) >= 0) {
        uint8_t soh_pct    = (uint8_t)(raw >> 8);
        uint8_t soh_status = (uint8_t)(raw & 0xFFu);
        static const char *const soh_str[] =
            {"Unknown","Bad","Very Low","Low","Mid","High","Full","?"};
        printf("StateOfHealth: %u %%  Status=%s\n",
               soh_pct, soh_str[soh_status < 7u ? soh_status : 7u]);
    }

    bus_b_deinit();
    printf("\nResult: %s\n",
           initcomp ? "✅ PASS" : "⚠️ CONDITIONAL (INITCOMP=0 — wait longer or cycle power)");
}

// Quick BQ27441 register dump for dump_bus_b()
static void dump_bq27441_quick(void) {
    printf("\n  [0x55] BQ27441 Fuel Gauge\n");
    uint16_t cs;
    if (fg_ctrl_read(BQ27441_CTRL_STATUS, &cs) < 0) {
        printf("    NACK — not responding (battery installed?)\n");
        return;
    }
    printf("    CTRL_STATUS: 0x%04X  INITCOMP=%d HIBERNATE=%d SLEEP=%d SEALED=%d\n",
           cs,
           (cs & BQ27441_CS_INITCOMP)  ? 1 : 0,
           (cs & BQ27441_CS_HIBERNATE) ? 1 : 0,
           (cs & BQ27441_CS_SLEEP)     ? 1 : 0,
           (cs & BQ27441_CS_SEALED)    ? 1 : 0);
    uint16_t flags;
    if (fg_read16(BQ27441_REG_FLAGS, &flags) >= 0)
        printf("    FLAGS:       0x%04X  ITPOR=%d BAT_DET=%d DSG=%d FC=%d\n",
               flags,
               (flags & BQ27441_FLAG_ITPOR)   ? 1 : 0,
               (flags & BQ27441_FLAG_BAT_DET) ? 1 : 0,
               (flags & BQ27441_FLAG_DSG)     ? 1 : 0,
               (flags & BQ27441_FLAG_FC)      ? 1 : 0);
    uint16_t v;
    if (fg_read16(BQ27441_REG_VOLT, &v) >= 0)
        printf("    Voltage:     %u mV\n", v);
    if (fg_read16(BQ27441_REG_SOC, &v) >= 0)
        printf("    SOC:         %u %%\n", v);
}

// ---------------------------------------------------------------------------
// Vibration motor — breathing PWM
// ---------------------------------------------------------------------------

void motor_breathe(void) {
    printf("\n--- Motor Breathe (GPIO%d, PWM4_B) ---\n", MTR_PWM_PIN);

    gpio_set_function(MTR_PWM_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(MTR_PWM_PIN);
    uint chan  = pwm_gpio_to_channel(MTR_PWM_PIN);

    // 1 kHz PWM: 125 MHz / 125 / 1000 = 1000 Hz
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 125.0f);
    pwm_config_set_wrap(&cfg, 999);
    pwm_init(slice, &cfg, true);
    pwm_set_chan_level(slice, chan, 0);

    for (int b = 0; b < 5; b++) {
        for (int d = 0; d <= 1000; d += 20) {
            pwm_set_chan_level(slice, chan, d > 999 ? 999 : d);
            sleep_ms(20);
        }
        for (int d = 999; d >= 0; d -= 20) {
            pwm_set_chan_level(slice, chan, d < 0 ? 0 : d);
            sleep_ms(20);
        }
        printf("  breath %d done\n", b + 1);
    }

    pwm_set_chan_level(slice, chan, 0);
    pwm_set_enabled(slice, false);
    gpio_set_function(MTR_PWM_PIN, GPIO_FUNC_NULL);
    printf("Motor off\n");
}

// ---------------------------------------------------------------------------
// Bus B register dump
// ---------------------------------------------------------------------------

static void dump_bq25622(void) {
    printf("\n  [0x6B] BQ25622 Charger\n");
    uint8_t v;
    bq_read(0x38, &v);
    uint8_t pn = (v >> 3) & 0x7;
    printf("    PART_INFO (0x38): 0x%02X  PN=%d(%s)  DEV_REV=%d\n",
           v, pn, pn==1?"BQ25622":pn==0?"BQ25620":"unknown", v&0x7);

    // VREG: REG0x04/05, 10mV/step, 9-bit; VREG[4:0] in 0x04[7:3], VREG[8:5] in 0x05[3:0]
    uint8_t vlo, vhi;
    bq_read(REG_VREG_LO, &vlo);
    bq_read(REG_VREG_HI, &vhi);
    uint16_t vreg_raw = (uint16_t)(((vhi & 0x0Fu) << 5) | ((vlo >> 3) & 0x1Fu));
    printf("    VREG      (0x04/05): lo=0x%02X hi=0x%02X → %u mV\n", vlo, vhi, (unsigned)(vreg_raw * 10u));

    // IINDPM: REG0x06/07, 20mA/step, 8-bit; IINDPM[3:0] in 0x06[7:4], IINDPM[7:4] in 0x07[3:0]
    bq_read(REG_IINDPM_LO, &vlo);
    bq_read(REG_IINDPM_HI, &vhi);
    uint16_t iindpm_raw = (uint16_t)(((vhi & 0x0Fu) << 4) | ((vlo >> 4) & 0x0Fu));
    printf("    IINDPM    (0x06/07): lo=0x%02X hi=0x%02X → %u mA\n", vlo, vhi, (unsigned)(iindpm_raw * 20u));

    bq_read(0x16, &v);
    printf("    CTRL1     (0x16): 0x%02X  EN_CHG=%d EN_HIZ=%d WD=%d\n", v,(v>>5)&1,(v>>4)&1,v&3);
    bq_read(0x17, &v);
    printf("    CTRL2     (0x17): 0x%02X\n", v);
    bq_read(0x18, &v);
    printf("    CTRL3     (0x18): 0x%02X  EN_OTG=%d BATFET_CTRL=%d BATFET_DLY=%d\n",
           v,(v>>6)&1,v&3,(v>>2)&1);
    bq_read(0x19, &v);
    printf("    CTRL4     (0x19): 0x%02X\n", v);
    bq_read(0x1A, &v);
    printf("    NTC_CTRL0 (0x1A): 0x%02X  TS_IGNORE=%d\n", v,(v>>7)&1);
    bq_read(0x1D, &v);
    printf("    STATUS0   (0x1D): 0x%02X  VSYS_STAT=%d WD_STAT=%d\n", v,(v>>4)&1,v&1);
    bq_read(0x1E, &v);
    printf("    STATUS1   (0x1E): 0x%02X  CHG=%s  VBUS=%s\n",
           v, bq_chg_str[(v>>3)&3], bq_vbus_str[v&7]);
    bq_read(0x1F, &v);
    printf("    FAULT     (0x1F): 0x%02X  BAT=%d SYS=%d TSHUT=%d TS=%d\n",
           v,(v>>6)&1,(v>>5)&1,(v>>3)&1,v&7);
}

static void dump_lm27965_regs(void) {
    printf("\n  [0x36] LM27965 LED Driver\n");
    uint8_t v;
    lm_read(0x10, &v);
    printf("    GP     (0x10): 0x%02X  ENA=%d(TFT) ENB=%d(Kbd+D3Bg) ENC=%d(D1Cr) EN5A=%d EN3B=%d\n",
           v, v&1, (v>>1)&1, (v>>2)&1, (v>>3)&1, (v>>4)&1);
    lm_read(0xA0, &v);
    printf("    BANK_A (0xA0): 0x%02X  code=%d (TFT backlight)\n", v, v&0x1F);
    lm_read(0xB0, &v);
    printf("    BANK_B (0xB0): 0x%02X  code=%d (keyboard BL + D3B green)\n", v, v&0x1F);
    lm_read(0xC0, &v);
    printf("    BANK_C (0xC0): 0x%02X  code=%d (D1C red; 00=20%% 01=40%% 10=70%% 11=100%%)\n",
           v, v&0x3);
}

void dump_bus_b(void) {
    printf("\n=== Bus B register dump (Power, GPIO 6/7) ===\n");
    bus_b_init();
    dump_bq25622();
    dump_bq27441_quick();
    dump_lm27965_regs();
    bus_b_deinit();
    printf("\n");
}
