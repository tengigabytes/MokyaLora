#include "bringup.h"
#include "bringup_menu.h"

// ---------------------------------------------------------------------------
// BQ25622 I2C helpers
// ---------------------------------------------------------------------------

int bq25622_reg_read(uint8_t reg, uint8_t *val) {
    int r = i2c_write_timeout_us(i2c1, BQ25622_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c1, BQ25622_ADDR, val, 1, false, 50000);
}

int bq25622_reg_write(uint8_t reg, uint8_t val) {
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

    if (bq25622_reg_read(REG_PART_INFO, &v) >= 0) {
        uint8_t pn = (v >> 3) & 0x7;
        printf("Part Info  (0x38): 0x%02X  PN=%s  REV=%d\n",
               v, pn == 1 ? "BQ25622" : (pn == 0 ? "BQ25620" : "???"), v & 0x7);
    }
    if (bq25622_reg_read(REG_STATUS0, &v) >= 0) {
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
    if (bq25622_reg_read(REG_STATUS1, &v) >= 0) {
        printf("Status1    (0x1E): 0x%02X  CHG=%s  VBUS=%s\n",
               v, bq_chg_str[(v >> 3) & 0x3], bq_vbus_str[v & 0x7]);
    }
    if (bq25622_reg_read(REG_FAULT_STATUS, &v) >= 0) {
        const char *ts_str[] = {"NORMAL", "COLD/OTG_COLD/no-bias", "HOT/OTG_HOT", "COOL",
                                 "WARM", "PRECOOL", "PREWARM", "TS-bias-fault"};
        printf("Fault      (0x1F): 0x%02X  BAT_FAULT=%d  SYS_FAULT=%d  TSHUT=%d  TS=%s\n",
               v, (v >> 6) & 1, (v >> 5) & 1, (v >> 3) & 1, ts_str[v & 0x7]);
    }
    if (bq25622_reg_read(REG_CHARGER_CTRL1, &v) >= 0) {
        printf("ChgCtrl1   (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
               v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
    }
    if (bq25622_reg_read(REG_NTC_CTRL0, &v) >= 0) {
        printf("NTC_Ctrl0  (0x1A): 0x%02X  TS_IGNORE=%d\n", v, (v >> 7) & 1);
    }
    if (bq25622_reg_read(REG_CHARGER_CTRL3, &v) >= 0) {
        printf("ChgCtrl3   (0x18): 0x%02X  BATFET_CTRL=%d(%s)  BATFET_DLY=%d\n",
               v, v & 0x3,
               (v & 0x3) == 0 ? "Normal" :
               (v & 0x3) == 1 ? "Shutdown" :
               (v & 0x3) == 2 ? "Ship" : "SysRst",
               (v >> 2) & 1);
    }
    if (bq25622_reg_read(REG_FAULT_FLAG0, &v) >= 0) {
        printf("FaultFlag  (0x22): 0x%02X\n", v);
    }
}

void bq25622_disable_charge(void) {
    uint8_t v;
    printf("\n--- BQ25622: Disabling Charge ---\n");
    bq25622_reg_write(REG_CHARGER_CTRL1, 0x84);  // kick WD_RST first
    sleep_ms(10);
    bq25622_reg_write(REG_CHARGER_CTRL1, 0x80);  // EN_CHG=0, WATCHDOG disabled
    bq25622_reg_read(REG_CHARGER_CTRL1, &v);
    printf("ChgCtrl1 (0x16): 0x%02X  EN_CHG=%d  EN_HIZ=%d  WATCHDOG=%d\n",
           v, (v >> 5) & 1, (v >> 4) & 1, v & 0x3);
}

void bq25622_enable_charge(void) {
    uint8_t v, vlo, vhi;
    printf("\n--- BQ25622: Enabling Charge ---\n");

    // 1. Set VREG = 4100 mV
    // VREG_RAW = 4100 / 10 = 410 = 0x19A (9-bit)
    // VREG[4:0] = 0x1A -> REG0x04[7:3] = 0xD0; VREG[8:5] = 0x0C -> REG0x05[3:0] = 0x0C
    bq25622_reg_write(REG_VREG_LO, 0xD0);
    bq25622_reg_write(REG_VREG_HI, 0x0C);
    bq25622_reg_read(REG_VREG_LO, &vlo);
    bq25622_reg_read(REG_VREG_HI, &vhi);
    uint16_t vreg_raw = (uint16_t)(((vhi & 0x0Fu) << 5) | ((vlo >> 3) & 0x1Fu));
    printf("VREG:   lo=0x%02X hi=0x%02X → %u mV\n", vlo, vhi, (unsigned)(vreg_raw * 10u));

    // 2. Set IINDPM = 500 mA
    // IINDPM_RAW = 500 / 20 = 25 = 0x19 (8-bit)
    // IINDPM[3:0] = 9 -> REG0x06[7:4] = 0x90; IINDPM[7:4] = 1 -> REG0x07[3:0] = 0x01
    bq25622_reg_write(REG_IINDPM_LO, 0x90);
    bq25622_reg_write(REG_IINDPM_HI, 0x01);
    bq25622_reg_read(REG_IINDPM_LO, &vlo);
    bq25622_reg_read(REG_IINDPM_HI, &vhi);
    uint16_t iindpm_raw = (uint16_t)(((vhi & 0x0Fu) << 4) | ((vlo >> 4) & 0x0Fu));
    printf("IINDPM: lo=0x%02X hi=0x%02X → %u mA\n", vlo, vhi, (unsigned)(iindpm_raw * 20u));

    // 3. Enable charging
    bq25622_reg_write(REG_CHARGER_CTRL1, 0xA4);  // kick WD_RST
    sleep_ms(10);
    bq25622_reg_write(REG_CHARGER_CTRL1, 0xA1);  // EN_CHG=1, WATCHDOG=01
    bq25622_reg_read(REG_CHARGER_CTRL1, &v);
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
    bq25622_reg_write(REG_ADC_CTRL, 0x80);
    uint8_t adc_ctrl, adc_dis;
    bq25622_reg_read(REG_ADC_CTRL, &adc_ctrl);
    bq25622_reg_read(REG_ADC_FUNC_DIS, &adc_dis);
    printf("ADC_CTRL (0x26): 0x%02X  EN=%d RATE=%s SAMPLE=%ubit\n",
           adc_ctrl, (adc_ctrl >> 7) & 1,
           (adc_ctrl >> 6) & 1 ? "one-shot" : "continuous",
           12u - ((adc_ctrl >> 4) & 0x3u));  // 00->12bit 01->11bit 10->10bit 11->9bit
    printf("ADC_DIS  (0x27): 0x%02X  IBUS=%d IBAT=%d VBUS=%d VBAT=%d VSYS=%d TS=%d TDIE=%d VPMID=%d\n",
           adc_dis,
           (adc_dis >> 7) & 1, (adc_dis >> 6) & 1, (adc_dis >> 5) & 1, (adc_dis >> 4) & 1,
           (adc_dis >> 3) & 1, (adc_dis >> 2) & 1, (adc_dis >> 1) & 1, adc_dis & 1);
    sleep_ms(300);  // 12-bit continuous mode: wait for full channel-scan cycle

    // IBUS: bits[15:1] 2s-complement, 2 mA/step
    bq25622_reg_read(REG_IBUS_ADC_LO, &lo); bq25622_reg_read(REG_IBUS_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    int ibus_ma = (int)((int16_t)raw16 >> 1) * 2;
    printf("  IBUS:  %+d mA  (raw 0x%04X)\n", ibus_ma, raw16);

    // IBAT: bits[15:2] 2s-complement, 4 mA/step
    bq25622_reg_read(REG_IBAT_ADC_LO, &lo); bq25622_reg_read(REG_IBAT_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    int ibat_ma = (int)((int16_t)raw16 >> 2) * 4;
    printf("  IBAT:  %+d mA  (raw 0x%04X)\n", ibat_ma, raw16);

    // VBUS: bits[14:2] unsigned, 3.97 mV/step
    bq25622_reg_read(REG_VBUS_ADC_LO, &lo); bq25622_reg_read(REG_VBUS_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vbus_mv = (unsigned)(((raw16 >> 2) & 0x1FFFu) * 397u / 100u);
    printf("  VBUS:  %u mV  (raw 0x%04X)\n", vbus_mv, raw16);

    // VPMID: bits[14:2] unsigned, 3.97 mV/step
    bq25622_reg_read(REG_VPMID_ADC_LO, &lo); bq25622_reg_read(REG_VPMID_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vpmid_mv = (unsigned)(((raw16 >> 2) & 0x1FFFu) * 397u / 100u);
    printf("  VPMID: %u mV  (raw 0x%04X)\n", vpmid_mv, raw16);

    // VBAT: bits[12:1] unsigned, 1.99 mV/step
    bq25622_reg_read(REG_VBAT_ADC_LO, &lo); bq25622_reg_read(REG_VBAT_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vbat_mv = (unsigned)(((raw16 >> 1) & 0x0FFFu) * 199u / 100u);
    printf("  VBAT:  %u mV  (raw 0x%04X)\n", vbat_mv, raw16);

    // VSYS: bits[12:1] unsigned, 1.99 mV/step
    bq25622_reg_read(REG_VSYS_ADC_LO, &lo); bq25622_reg_read(REG_VSYS_ADC_HI, &hi);
    raw16 = (uint16_t)((hi << 8) | lo);
    unsigned vsys_mv = (unsigned)(((raw16 >> 1) & 0x0FFFu) * 199u / 100u);
    printf("  VSYS:  %u mV  (raw 0x%04X)\n", vsys_mv, raw16);
}

// ---------------------------------------------------------------------------
// BQ25622 register dump
// ---------------------------------------------------------------------------

void bq25622_dump_regs(void) {
    printf("\n  [0x6B] BQ25622 Charger\n");
    uint8_t v;
    bq25622_reg_read(0x38, &v);
    uint8_t pn = (v >> 3) & 0x7;
    printf("    PART_INFO (0x38): 0x%02X  PN=%d(%s)  DEV_REV=%d\n",
           v, pn, pn==1?"BQ25622":pn==0?"BQ25620":"unknown", v&0x7);

    // VREG: REG0x04/05, 10mV/step, 9-bit; VREG[4:0] in 0x04[7:3], VREG[8:5] in 0x05[3:0]
    uint8_t vlo, vhi;
    bq25622_reg_read(REG_VREG_LO, &vlo);
    bq25622_reg_read(REG_VREG_HI, &vhi);
    uint16_t vreg_raw = (uint16_t)(((vhi & 0x0Fu) << 5) | ((vlo >> 3) & 0x1Fu));
    printf("    VREG      (0x04/05): lo=0x%02X hi=0x%02X → %u mV\n", vlo, vhi, (unsigned)(vreg_raw * 10u));

    // IINDPM: REG0x06/07, 20mA/step, 8-bit; IINDPM[3:0] in 0x06[7:4], IINDPM[7:4] in 0x07[3:0]
    bq25622_reg_read(REG_IINDPM_LO, &vlo);
    bq25622_reg_read(REG_IINDPM_HI, &vhi);
    uint16_t iindpm_raw = (uint16_t)(((vhi & 0x0Fu) << 4) | ((vlo >> 4) & 0x0Fu));
    printf("    IINDPM    (0x06/07): lo=0x%02X hi=0x%02X → %u mA\n", vlo, vhi, (unsigned)(iindpm_raw * 20u));

    bq25622_reg_read(0x16, &v);
    printf("    CTRL1     (0x16): 0x%02X  EN_CHG=%d EN_HIZ=%d WD=%d\n", v,(v>>5)&1,(v>>4)&1,v&3);
    bq25622_reg_read(0x17, &v);
    printf("    CTRL2     (0x17): 0x%02X\n", v);
    bq25622_reg_read(0x18, &v);
    printf("    CTRL3     (0x18): 0x%02X  EN_OTG=%d BATFET_CTRL=%d BATFET_DLY=%d\n",
           v,(v>>6)&1,v&3,(v>>2)&1);
    bq25622_reg_read(0x19, &v);
    printf("    CTRL4     (0x19): 0x%02X\n", v);
    bq25622_reg_read(0x1A, &v);
    printf("    NTC_CTRL0 (0x1A): 0x%02X  TS_IGNORE=%d\n", v,(v>>7)&1);
    bq25622_reg_read(0x1D, &v);
    printf("    STATUS0   (0x1D): 0x%02X  VSYS_STAT=%d WD_STAT=%d\n", v,(v>>4)&1,v&1);
    bq25622_reg_read(0x1E, &v);
    printf("    STATUS1   (0x1E): 0x%02X  CHG=%s  VBUS=%s\n",
           v, bq_chg_str[(v>>3)&3], bq_vbus_str[v&7]);
    bq25622_reg_read(0x1F, &v);
    printf("    FAULT     (0x1F): 0x%02X  BAT=%d SYS=%d TSHUT=%d TS=%d\n",
           v,(v>>6)&1,(v>>5)&1,(v>>3)&1,v&7);
}

// ---------------------------------------------------------------------------
// Charger Diagnostic — BQ25622 status + ADC on TFT, 1 Hz refresh
// ---------------------------------------------------------------------------

void charger_diag(void) {
    const int S = 2;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / (6 * S);

    printf("\n--- Charger Diagnostic (1 Hz refresh, BACK to exit) ---\n");
    bus_b_init();

    // Enable ADC: continuous, 12-bit
    bq25622_reg_write(REG_ADC_CTRL, 0x80);
    sleep_ms(300);  // first full ADC cycle

    // Draw static elements once
    menu_clear(MC_BG);
    menu_str(0, 0, " Charger Diag       ", COLS, MC_TITLE, MC_TITBG, S);
    menu_str(0, 9 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);

    for (;;) {
        if (back_key_pressed()) break;

        // --- Read ADC channels ---
        uint8_t lo, hi;
        uint16_t raw16;

        bq25622_reg_read(REG_VBUS_ADC_LO, &lo); bq25622_reg_read(REG_VBUS_ADC_HI, &hi);
        raw16 = (uint16_t)((hi << 8) | lo);
        unsigned vbus_mv = (unsigned)(((raw16 >> 2) & 0x1FFFu) * 397u / 100u);

        bq25622_reg_read(REG_VBAT_ADC_LO, &lo); bq25622_reg_read(REG_VBAT_ADC_HI, &hi);
        raw16 = (uint16_t)((hi << 8) | lo);
        unsigned vbat_mv = (unsigned)(((raw16 >> 1) & 0x0FFFu) * 199u / 100u);

        bq25622_reg_read(REG_VSYS_ADC_LO, &lo); bq25622_reg_read(REG_VSYS_ADC_HI, &hi);
        raw16 = (uint16_t)((hi << 8) | lo);
        unsigned vsys_mv = (unsigned)(((raw16 >> 1) & 0x0FFFu) * 199u / 100u);

        bq25622_reg_read(REG_IBUS_ADC_LO, &lo); bq25622_reg_read(REG_IBUS_ADC_HI, &hi);
        raw16 = (uint16_t)((hi << 8) | lo);
        int ibus_ma = (int)((int16_t)raw16 >> 1) * 2;

        bq25622_reg_read(REG_IBAT_ADC_LO, &lo); bq25622_reg_read(REG_IBAT_ADC_HI, &hi);
        raw16 = (uint16_t)((hi << 8) | lo);
        int ibat_ma = (int)((int16_t)raw16 >> 2) * 4;

        // --- Read status ---
        uint8_t st1;
        bq25622_reg_read(REG_STATUS1, &st1);
        const char *chg_str[] = {"NoCHG", "CC   ", "Taper", "TopOf"};
        const char *vbus_str;
        uint8_t vbus_stat = st1 & 0x7;
        if (vbus_stat == 0) vbus_str = "None";
        else if (vbus_stat == 4) vbus_str = "Adpt";
        else if (vbus_stat == 7) vbus_str = "OTG ";
        else vbus_str = "??? ";

        // --- TFT update (overwrite value rows only, no clear) ---
        char line[24];
        snprintf(line, sizeof(line), " VBUS: %5u mV    ", vbus_mv);
        menu_str(0, 2 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " VBAT: %5u mV    ", vbat_mv);
        menu_str(0, 3 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " VSYS: %5u mV    ", vsys_mv);
        menu_str(0, 4 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " IBUS: %+5d mA    ", ibus_ma);
        menu_str(0, 5 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " IBAT: %+5d mA    ", ibat_ma);
        menu_str(0, 6 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " CHG:%s VBUS:%s ", chg_str[(st1 >> 3) & 3], vbus_str);
        menu_str(0, 7 * CH, line, COLS, MC_OK, MC_BG, S);

        // --- Serial output ---
        printf("  VBUS=%umV VBAT=%umV VSYS=%umV IBUS=%+dmA IBAT=%+dmA CHG=%s VBUS=%s\n",
               vbus_mv, vbat_mv, vsys_mv, ibus_ma, ibat_ma,
               chg_str[(st1 >> 3) & 3], vbus_str);

        // 1 Hz refresh — poll BACK key during wait
        for (int i = 0; i < 20; i++) {
            if (back_key_pressed()) goto done;
            sleep_ms(50);
        }
    }
done:
    bus_b_deinit();
    printf("Charger diag done\n");
}
