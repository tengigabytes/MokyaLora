#include "bringup.h"
#include "bringup_menu.h"

// ---------------------------------------------------------------------------
// BQ27441 I2C helpers
// ---------------------------------------------------------------------------

// Read a 16-bit standard command register (little-endian).
int fg_read16(uint8_t reg, uint16_t *val) {
    uint8_t buf[2];
    int r = i2c_write_timeout_us(i2c1, BQ27441_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    r = i2c_read_timeout_us(i2c1, BQ27441_ADDR, buf, 2, false, 50000);
    if (r < 0) return r;
    *val = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return 0;
}

// Send a CONTROL() sub-command (write only, no result read).
int fg_ctrl_write(uint16_t subcmd) {
    uint8_t buf[3] = {0x00, (uint8_t)(subcmd & 0xFFu), (uint8_t)(subcmd >> 8)};
    return i2c_write_timeout_us(i2c1, BQ27441_ADDR, buf, 3, false, 50000);
}

// Send a CONTROL() sub-command and read back the 16-bit result.
// >=66 us bus-free time between write and read required (SLUSBH1C S8.5.1.1).
int fg_ctrl_read(uint16_t subcmd, uint16_t *result) {
    if (fg_ctrl_write(subcmd) < 0) return -1;
    sleep_us(100);
    uint8_t reg = 0x00;
    uint8_t buf[2];
    if (i2c_write_timeout_us(i2c1, BQ27441_ADDR, &reg, 1, true, 50000) < 0) return -1;
    if (i2c_read_timeout_us(i2c1, BQ27441_ADDR, buf, 2, false, 50000) < 0) return -1;
    *result = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return 0;
}

// ---------------------------------------------------------------------------
// BQ27441 full interactive diagnostic (Step 12)
// ---------------------------------------------------------------------------

void bq27441_read(void) {
    printf("\n--- BQ27441 Fuel Gauge (Step 12) ---\n");
    bus_b_init();

    // 1. Probe: any directed I2C transaction wakes gauge from SLEEP/HIBERNATE
    //    via <=100 us clock stretch (SLUSBH1C p.14). Read CONTROL_STATUS first.
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
        // 9 SCL clocks + STOP per UM10204 S3.1.16
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
        // STOP condition: SDA LOW->HIGH while SCL HIGH
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
            // Try BAT_INSERT first — works only when BIE=0 (SLUSBH1C S8.6.1.1.4)
            fg_ctrl_write(0x000C);
            sleep_ms(200);
            fg_read16(BQ27441_REG_FLAGS, &flags);
            bat_det = (flags & BQ27441_FLAG_BAT_DET) ? 1 : 0;

            if (!bat_det) {
                // BIE=1 (hardware default) — BAT_INSERT ignored when BIN not connected.
                // Clear BIE via CONFIG UPDATE so software battery detection works.
                // Reference: SLUUAC9A S3.1 (Extended Data Commands)
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

                    // DataBlock(0x3F) = 0 (offsets 0-31)
                    uint8_t db[2] = {0x3F, 0};
                    i2c_write_timeout_us(i2c1, BQ27441_ADDR, db, 2, false, 50000);
                    sleep_ms(4);  // wait for block buffer load

                    // Read 32 bytes BlockData (0x40-0x5F)
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

// ---------------------------------------------------------------------------
// Quick BQ27441 register dump
// ---------------------------------------------------------------------------

void bq27441_dump_regs(void) {
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
// Gauge Diagnostic — BQ27441 readings on TFT, 1 Hz refresh
// ---------------------------------------------------------------------------

void gauge_diag(void) {
    const int S = 2;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / (6 * S);

    printf("\n--- Fuel Gauge Diagnostic (1 Hz refresh, BACK to exit) ---\n");
    bus_b_init();

    // Probe gauge
    uint16_t cs;
    if (fg_ctrl_read(BQ27441_CTRL_STATUS, &cs) < 0) {
        menu_clear(MC_BG);
        menu_str(0, 0, " Fuel Gauge Diag    ", COLS, MC_TITLE, MC_TITBG, S);
        menu_str(0, 3 * CH, " NACK at 0x55       ", COLS, MC_ERR, MC_BG, S);
        menu_str(0, 5 * CH, " Check battery/pwr  ", COLS, MC_HINT, MC_BG, S);
        menu_str(0, 9 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
        printf("NACK -- BQ27441 not responding at 0x55\n");
        bus_b_deinit();
        // Wait for BACK here (back_key_init already called by menu)
        while (!back_key_pressed()) sleep_ms(50);
        return;
    }

    // Draw static elements once
    menu_clear(MC_BG);
    menu_str(0, 0, " Fuel Gauge Diag    ", COLS, MC_TITLE, MC_TITBG, S);
    menu_str(0, 9 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);

    for (;;) {
        if (back_key_pressed()) break;

        uint16_t raw;
        unsigned vbat = 0;
        int      cur  = 0;
        unsigned soc  = 0;
        float    temp = 0.0f;
        unsigned rem  = 0, full = 0;
        uint8_t  soh_pct = 0;
        uint8_t  soh_st  = 0;

        if (fg_read16(BQ27441_REG_VOLT, &raw) >= 0)    vbat = raw;
        if (fg_read16(BQ27441_REG_AVGCUR, &raw) >= 0)  cur  = (int)(int16_t)raw;
        if (fg_read16(BQ27441_REG_SOC, &raw) >= 0)     soc  = raw;
        if (fg_read16(BQ27441_REG_TEMP, &raw) >= 0)    temp = (float)raw * 0.1f - 273.15f;
        if (fg_read16(BQ27441_REG_REMCAP, &raw) >= 0)  rem  = raw;
        if (fg_read16(BQ27441_REG_FULLCAP, &raw) >= 0) full = raw;
        if (fg_read16(BQ27441_REG_SOH, &raw) >= 0) {
            soh_pct = (uint8_t)(raw >> 8);
            soh_st  = (uint8_t)(raw & 0xFFu);
        }
        static const char *const soh_str[] =
            {"Unk","Bad","VLow","Low","Mid","High","Full","?"};

        // --- TFT update (overwrite value rows only, no clear) ---
        char line[24];
        snprintf(line, sizeof(line), " VBAT: %5u mV    ", vbat);
        menu_str(0, 2 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Curr: %+5d mA    ", cur);
        menu_str(0, 3 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " SOC:    %3u %%      ", soc);
        menu_str(0, 4 * CH, line, COLS, soc > 20 ? MC_OK : MC_ERR, MC_BG, S);
        snprintf(line, sizeof(line), " Temp: %5.1f C     ", temp);
        menu_str(0, 5 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Cap: %u/%u mAh  ", rem, full);
        menu_str(0, 6 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " SOH: %3u%% %s    ", soh_pct,
                 soh_str[soh_st < 7u ? soh_st : 7u]);
        menu_str(0, 7 * CH, line, COLS, MC_FG, MC_BG, S);

        // --- Serial output ---
        printf("  V=%umV I=%+dmA SOC=%u%% T=%.1fC Cap=%u/%umAh SOH=%u%%\n",
               vbat, cur, soc, temp, rem, full, soh_pct);

        // 1 Hz refresh — poll BACK key during wait
        for (int i = 0; i < 20; i++) {
            if (back_key_pressed()) goto done;
            sleep_ms(50);
        }
    }
done:
    bus_b_deinit();
    printf("Gauge diag done\n");
}
