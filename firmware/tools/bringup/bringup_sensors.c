#include "bringup.h"
#include "bringup_menu.h"

// ---------------------------------------------------------------------------
// I2C sensor bus helpers (Bus A: GPIO 34/35)
// ---------------------------------------------------------------------------

static int dev_read(i2c_inst_t *i2c, uint8_t addr, uint8_t reg, uint8_t *val) {
    int r = i2c_write_timeout_us(i2c, addr, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c, addr, val, 1, false, 50000);
}

static int dev_write(i2c_inst_t *i2c, uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(i2c, addr, buf, 2, false, 50000);
}

// ---------------------------------------------------------------------------
// LPS22HH Barometer (0x5D)
// ---------------------------------------------------------------------------

void baro_read(void) {
    // Register map (DS12503):
    //   CTRL_REG1 (0x10): [6:4]=odr [1]=bdu [0]=sim
    //   CTRL_REG2 (0x11): [7]=boot [4]=if_add_inc [2]=swreset [1]=low_noise_en [0]=one_shot
    //   STATUS    (0x27): [1]=t_da [0]=p_da
    //   PRESS_OUT: 0x28(XL) 0x29(L) 0x2A(H)  → uint24; hPa = raw/4096.0
    //   TEMP_OUT:  0x2B(L)  0x2C(H)           → int16; °C = raw/100.0
    printf("\n--- LPS22HH Barometer live display (BACK to exit) ---\n");

    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    dev_write(i2c1, 0x5D, 0x11, 0x04);  // SW reset
    for (int i = 0; i < 50; i++) {
        uint8_t c = 0xFF;
        dev_read(i2c1, 0x5D, 0x11, &c);
        if (!(c & 0x04)) break;
        sleep_ms(1);
    }
    dev_write(i2c1, 0x5D, 0x11, 0x12);  // IF_ADD_INC=1, LOW_NOISE_EN=1
    dev_write(i2c1, 0x5D, 0x10, 0x32);  // BDU=1, ODR=25Hz

    // Wait for first data ready
    uint8_t status = 0;
    for (int i = 0; i < 100; i++) {
        dev_read(i2c1, 0x5D, 0x27, &status);
        if ((status & 0x03) == 0x03) break;
        sleep_ms(5);
    }

    // TFT rendering constants (scale=2: 12x16 px per char)
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W = menu_tft_width();
    const int COLS = W / CW;

    // Draw static header
    menu_clear(MC_BG);
    menu_str(0, 0,      " Baro Live          ", COLS, MC_TITLE, MC_TITBG, S);
    menu_str(0, 2 * CH, " Pressure (hPa)     ", COLS, MC_HINT,  MC_BG,   S);
    menu_str(0, 5 * CH, " Temperature (C)    ", COLS, MC_HINT,  MC_BG,   S);
    menu_str(0, 8 * CH, " Altitude est (m)   ", COLS, MC_HINT,  MC_BG,   S);

    int samples = 0;
    char line[40];

    while (true) {
        if (back_key_pressed()) break;

        dev_read(i2c1, 0x5D, 0x27, &status);
        if ((status & 0x03) != 0x03) {
            sleep_ms(10);
            continue;
        }

        uint8_t reg = 0x28;
        uint8_t raw[5] = {0};
        i2c_write_timeout_us(i2c1, 0x5D, &reg, 1, true, 50000);
        i2c_read_timeout_us(i2c1, 0x5D, raw, 5, false, 100000);

        uint32_t p_raw = (uint32_t)raw[0] | ((uint32_t)raw[1]<<8) | ((uint32_t)raw[2]<<16);
        int16_t  t_raw = (int16_t)((uint16_t)raw[3] | ((uint16_t)raw[4]<<8));
        float pressure = p_raw / 4096.0f;
        float temp     = t_raw / 100.0f;
        // Barometric altitude estimate: ISA formula, sea-level 1013.25 hPa
        float altitude = 44330.0f * (1.0f - powf(pressure / 1013.25f, 0.1903f));

        // Update TFT
        snprintf(line, sizeof(line), " %.2f              ", pressure);
        menu_str(0, 3 * CH, line, COLS, MC_FG, MC_BG, S);

        snprintf(line, sizeof(line), " %.2f              ", temp);
        menu_str(0, 6 * CH, line, COLS, MC_FG, MC_BG, S);

        snprintf(line, sizeof(line), " %.1f              ", altitude);
        menu_str(0, 9 * CH, line, COLS, MC_FG, MC_BG, S);

        if (++samples % 5 == 0) {
            printf("  P: %.2f hPa  T: %.2f C  Alt: %.1f m\n",
                   pressure, temp, altitude);
        }

        sleep_ms(100);  // ~10 Hz display update
    }

    // --- Cleanup ---
    dev_write(i2c1, 0x5D, 0x10, 0x00);  // power down

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// LIS2MDL Magnetometer (0x1E)
// ---------------------------------------------------------------------------

void mag_read(void) {
    // Register map (AN5069):
    //   CFG_REG_A (0x60): [7]=comp_temp_en [5]=soft_rst [4]=lp [3:2]=odr [1:0]=md
    //   CFG_REG_B (0x61): [1:0]=set_rst (01=OFF_CANC every ODR)
    //   CFG_REG_C (0x62): [4]=bdu
    //   STATUS    (0x67): [3]=ZYXDA
    //   OUT: 0x68(X_L)..0x6D(Z_H)  — 1.5 mGauss/LSB
    //   TEMP_OUT: 0x6E(L) 0x6F(H)  — 8 LSB/°C, 25°C = 0
    printf("\n--- LIS2MDL Magnetometer live display (BACK to exit) ---\n");

    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    dev_write(i2c1, 0x1E, 0x60, 0x20);  // SW reset
    for (int i = 0; i < 50; i++) {
        uint8_t c = 0xFF;
        dev_read(i2c1, 0x1E, 0x60, &c);
        if (!(c & 0x20)) break;
        sleep_ms(1);
    }
    dev_write(i2c1, 0x1E, 0x60, 0x8C);  // comp_temp_en=1, odr=50Hz, continuous
    dev_write(i2c1, 0x1E, 0x61, 0x02);  // OFF_CANC every ODR
    dev_write(i2c1, 0x1E, 0x62, 0x10);  // BDU=1

    // Wait for first data ready
    uint8_t status = 0;
    for (int i = 0; i < 40; i++) {
        dev_read(i2c1, 0x1E, 0x67, &status);
        if (status & 0x08) break;
        sleep_ms(5);
    }

    // TFT rendering constants
    const int S = 2;
    const int CH = 8 * S;
    const int W = menu_tft_width();
    const int COLS = W / (6 * S);

    menu_clear(MC_BG);
    menu_str(0, 0,      " Mag Live           ", COLS, MC_TITLE, MC_TITBG, S);
    menu_str(0, 2 * CH, " Mag (mGauss)       ", COLS, MC_HINT,  MC_BG,   S);
    menu_str(0, 6 * CH, " Heading            ", COLS, MC_HINT,  MC_BG,   S);
    menu_str(0, 9 * CH, " Field strength     ", COLS, MC_HINT,  MC_BG,   S);

    int samples = 0;
    char line[40];

    while (true) {
        if (back_key_pressed()) break;

        dev_read(i2c1, 0x1E, 0x67, &status);
        if (!(status & 0x08)) {
            sleep_ms(10);
            continue;
        }

        uint8_t reg = 0x68;
        uint8_t raw[6] = {0};
        i2c_write_timeout_us(i2c1, 0x1E, &reg, 1, true, 50000);
        i2c_read_timeout_us(i2c1, 0x1E, raw, 6, false, 100000);

        int16_t mx = (int16_t)((raw[1]<<8)|raw[0]);
        int16_t my = (int16_t)((raw[3]<<8)|raw[2]);
        int16_t mz = (int16_t)((raw[5]<<8)|raw[4]);
        float fmx = mx * 1.5f;
        float fmy = my * 1.5f;
        float fmz = mz * 1.5f;

        // Heading from X/Y (assuming device flat, Z up)
        float heading = atan2f(-fmy, fmx) * 180.0f / (float)M_PI;
        if (heading < 0) heading += 360.0f;

        // Total field strength
        float total = sqrtf(fmx * fmx + fmy * fmy + fmz * fmz);

        // Update TFT — mag axes
        snprintf(line, sizeof(line), " X %+8.1f         ", fmx);
        menu_str(0, 3 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Y %+8.1f         ", fmy);
        menu_str(0, 4 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Z %+8.1f         ", fmz);
        menu_str(0, 5 * CH, line, COLS, MC_FG, MC_BG, S);

        // Heading
        snprintf(line, sizeof(line), " %6.1f deg         ", heading);
        menu_str(0, 7 * CH, line, COLS, MC_FG, MC_BG, S);

        // Field strength
        snprintf(line, sizeof(line), " %.0f mG             ", total);
        menu_str(0, 10 * CH, line, COLS, MC_FG, MC_BG, S);

        if (++samples % 5 == 0) {
            printf("  M: %+8.1f %+8.1f %+8.1f mG  hdg=%.1f  |B|=%.0f\n",
                   fmx, fmy, fmz, heading, total);
        }

        sleep_ms(50);  // ~20 Hz
    }

    // --- Cleanup ---
    dev_write(i2c1, 0x1E, 0x60, 0x02);  // power down (md=10)

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// LSM6DSV16X IMU (0x6A)
// ---------------------------------------------------------------------------

void imu_read(void) {
    // Register map (DS13448):
    //   CTRL1 (0x10): [6:4]=op_mode_xl [3:0]=odr_xl
    //   CTRL2 (0x11): [6:4]=op_mode_g  [3:0]=odr_g
    //   CTRL3 (0x12): [6]=bdu [2]=if_inc [0]=sw_reset
    //   CTRL6 (0x15): [3:0]=fs_g  (1=±250dps, 8.75mdps/LSB)
    //   CTRL8 (0x17): [1:0]=fs_xl (0=±2g, 0.061mg/LSB)
    //   STATUS (0x1E): [2]=TDA [1]=GDA [0]=XLDA
    //   Data: 0x20(TEMP_L)..0x2D(OUTZ_H_A) — 14 bytes burst
    printf("\n--- LSM6DSV16X IMU live display (BACK to exit) ---\n");

    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    dev_write(i2c1, 0x6A, 0x12, 0x01);  // SW reset
    for (int i = 0; i < 50; i++) {
        uint8_t c = 0xFF;
        dev_read(i2c1, 0x6A, 0x12, &c);
        if (!(c & 0x01)) break;
        sleep_ms(1);
    }
    dev_write(i2c1, 0x6A, 0x12, 0x44);  // BDU=1, IF_INC=1
    dev_write(i2c1, 0x6A, 0x17, 0x00);  // fs_xl=±2g
    dev_write(i2c1, 0x6A, 0x15, 0x01);  // fs_g=±250dps
    dev_write(i2c1, 0x6A, 0x10, 0x06);  // odr_xl=120Hz, HP mode
    dev_write(i2c1, 0x6A, 0x11, 0x06);  // odr_g=120Hz,  HP mode

    // Wait for first data ready
    uint8_t status = 0;
    for (int i = 0; i < 40; i++) {
        dev_read(i2c1, 0x6A, 0x1E, &status);
        if ((status & 0x03) == 0x03) break;
        sleep_ms(5);
    }

    // TFT rendering constants (scale=2: 12x16 px per char)
    const int S = 2;
    const int CW = 6 * S;   // 12 px char width
    const int CH = 8 * S;   // 16 px char height
    const int W = menu_tft_width();
    const int COLS = W / CW;

    // Draw static header
    menu_clear(MC_BG);
    menu_str(0, 0,      " IMU Live           ", COLS, MC_TITLE, MC_TITBG, S);
    menu_str(0, 2 * CH, " Accel (g)    +/-2g ", COLS, MC_HINT,  MC_BG,   S);
    menu_str(0, 6 * CH, " Gyro (dps) +/-250  ", COLS, MC_HINT,  MC_BG,   S);
    menu_str(0, 10* CH, " Temp               ", COLS, MC_HINT,  MC_BG,   S);

    int samples = 0;
    char line[40];

    // --- Main loop: read + draw at ~20 Hz ---
    while (true) {
        if (back_key_pressed()) break;

        // Wait for data ready (both XL + G)
        dev_read(i2c1, 0x6A, 0x1E, &status);
        if ((status & 0x03) != 0x03) {
            sleep_ms(5);
            continue;
        }

        // Burst read 14 bytes: temp(2) + gyro(6) + accel(6)
        uint8_t reg = 0x20;
        uint8_t raw[14] = {0};
        i2c_write_timeout_us(i2c1, 0x6A, &reg, 1, true, 50000);
        i2c_read_timeout_us(i2c1, 0x6A, raw, 14, false, 100000);

        int16_t t_raw = (int16_t)((raw[1]<<8)|raw[0]);
        int16_t gx = (int16_t)((raw[3]<<8)|raw[2]);
        int16_t gy = (int16_t)((raw[5]<<8)|raw[4]);
        int16_t gz = (int16_t)((raw[7]<<8)|raw[6]);
        int16_t ax = (int16_t)((raw[9]<<8)|raw[8]);
        int16_t ay = (int16_t)((raw[11]<<8)|raw[10]);
        int16_t az = (int16_t)((raw[13]<<8)|raw[12]);

        float fax = ax * 0.000061f;
        float fay = ay * 0.000061f;
        float faz = az * 0.000061f;
        float fgx = gx * 0.00875f;
        float fgy = gy * 0.00875f;
        float fgz = gz * 0.00875f;
        float temp = 25.0f + t_raw / 256.0f;

        // Update TFT — accel
        snprintf(line, sizeof(line), " X %+7.3f          ", fax);
        menu_str(0, 3 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Y %+7.3f          ", fay);
        menu_str(0, 4 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Z %+7.3f          ", faz);
        menu_str(0, 5 * CH, line, COLS, MC_FG, MC_BG, S);

        // Update TFT — gyro
        snprintf(line, sizeof(line), " X %+8.2f         ", fgx);
        menu_str(0, 7 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Y %+8.2f         ", fgy);
        menu_str(0, 8 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Z %+8.2f         ", fgz);
        menu_str(0, 9 * CH, line, COLS, MC_FG, MC_BG, S);

        // Update TFT — temp
        snprintf(line, sizeof(line), " %+.1f C             ", temp);
        menu_str(0, 11 * CH, line, COLS, MC_FG, MC_BG, S);

        // Serial output (throttled: every 10th sample)
        if (++samples % 10 == 0) {
            printf("  A: %+6.3f %+6.3f %+6.3f g  "
                   "G: %+7.2f %+7.2f %+7.2f dps  "
                   "T: %.1f C\n",
                   fax, fay, faz, fgx, fgy, fgz, temp);
        }

        sleep_ms(50);  // ~20 Hz display update
    }

    // --- Cleanup ---
    dev_write(i2c1, 0x6A, 0x10, 0x00);  // power down XL
    dev_write(i2c1, 0x6A, 0x11, 0x00);  // power down G

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// Teseo-LIV3FL GNSS (0x3A)
// ---------------------------------------------------------------------------

// Print NMEA bytes as readable text; returns number of complete sentences seen.
static int gnss_print_buf(const uint8_t *buf, int len) {
    int sentences = 0;
    printf("    ");
    for (int i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '$') sentences++;
        if      (c >= 0x20 && c < 0x7F) printf("%c", c);
        else if (c == '\r') {}
        else if (c == '\n') printf("\n    ");
        else if (c != 0xFF) printf("\\x%02X", (uint8_t)c);
    }
    printf("\n");
    return sentences;
}

static void dump_gnss(i2c_inst_t *i2c) {
    printf("\n  [0x3A] Teseo-LIV3FL GNSS (streaming, read 64 bytes)\n");
    uint8_t buf[64];
    int r = i2c_read_timeout_us(i2c, 0x3A, buf, 64, false, 200000);
    if (r < 0) { printf("    read error (%d)\n", r); return; }
    gnss_print_buf(buf, r);
}

// NMEA sentence types to track on fixed display rows
// Each type gets one TFT row showing the latest sentence content.
#define NMEA_SLOT_COUNT  14
static const char *nmea_slot_prefix[NMEA_SLOT_COUNT] = {
    "GNGGA", "GNRMC", "GNGLL", "GNVTG",
    "GPGGA", "GPRMC",
    "GPGSV", "GLGSV", "GAGSV", "GBGSV", "GNGSV",
    "GNGSA",
    "PSTM",  "GPGSA",
};

void gnss_info(void) {
    printf("\n--- NMEA Polling (BACK to exit) ---\n");

    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    // TFT setup
    const int S = 2;
    const int CH = 8 * S;     // 16 px row height
    const int W = menu_tft_width();
    const int H = menu_tft_height();
    const int COLS = W / (6 * S);
    const int max_rows = H / CH - 1;  // minus header row
    int slots = NMEA_SLOT_COUNT;
    if (slots > max_rows) slots = max_rows;

    menu_clear(MC_BG);
    menu_str(0, 0, " NMEA Polling", COLS, MC_TITLE, MC_TITBG, S);

    // Draw initial slot labels
    for (int i = 0; i < slots; i++) {
        char lbl[40];
        snprintf(lbl, sizeof(lbl), "%-6s ---", nmea_slot_prefix[i]);
        menu_str(0, (1 + i) * CH, lbl, COLS, MC_HINT, MC_BG, S);
    }

    // Simple NMEA accumulator (no dependency on gnss_tft module)
    char nbuf[256];
    int nlen = 0;

    uint32_t pkt_count = 0;

    while (true) {
        if (back_key_pressed()) break;

        // Read raw bytes from Teseo-LIV3FL
        uint8_t raw[128];
        int r = i2c_read_timeout_us(i2c1, 0x3A, raw, sizeof(raw), false, 200000);
        if (r <= 0) { sleep_ms(20); continue; }

        // Accumulate into sentence buffer
        for (int i = 0; i < r; i++) {
            char c = (char)raw[i];
            if (c == (char)0xFF) continue;  // idle filler
            if (c == '$') nlen = 0;         // start new sentence
            if (nlen < (int)sizeof(nbuf) - 1) {
                nbuf[nlen++] = c;
                nbuf[nlen]   = '\0';
            }
            if (c != '\n') continue;

            // Complete sentence: nbuf starts with '$', ends with \r\n
            // Strip leading '$' and trailing \r\n for matching
            if (nlen < 6 || nbuf[0] != '$') { nlen = 0; continue; }
            char *body = nbuf + 1;  // skip '$'
            // Trim trailing \r\n
            for (int k = nlen - 1; k > 0 && (nbuf[k]=='\r'||nbuf[k]=='\n'); k--)
                nbuf[k] = '\0';

            // Match against slot prefixes
            for (int s = 0; s < slots; s++) {
                int plen = (int)strlen(nmea_slot_prefix[s]);
                if (strncmp(body, nmea_slot_prefix[s], plen) == 0) {
                    // Truncate to fit screen width, show on this slot's row
                    char line[40];
                    snprintf(line, sizeof(line), "%.*s", COLS, body);
                    menu_str(0, (1 + s) * CH, line, COLS, MC_FG, MC_BG, S);
                    pkt_count++;
                    break;
                }
            }

            // Also print to serial
            printf("  $%s\n", body);
            nlen = 0;
        }

        sleep_ms(10);
    }

    printf("  Total sentences: %lu\n", (unsigned long)pkt_count);

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// gnss_probe — Read-only Teseo-LIV3FL state dump via $PSTMGETPAR
// Reads CDB-ID 200 / 227 / 231 / 232 so we can see the actual NMEA I2C mask
// and feature flags without writing anything. Issue 11 diagnostic.
// ---------------------------------------------------------------------------

static uint8_t gnss_nmea_cs(const char *body) {
    uint8_t cs = 0;
    while (*body) cs ^= (uint8_t)*body++;
    return cs;
}

static int gnss_send_nmea(const char *body) {
    char buf[96];
    uint8_t cs = gnss_nmea_cs(body);
    int n = snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, cs);
    return i2c_write_timeout_us(i2c1, 0x3A, (const uint8_t *)buf, n, false, 200000);
}

// Drain the I2C NMEA stream for up to wait_ms, printing any sentence whose
// body starts with match_prefix (skip NULL to print everything).
static void gnss_drain(int wait_ms, const char *match_prefix) {
    char sbuf[256];
    int slen = 0;
    absolute_time_t end = make_timeout_time_ms(wait_ms);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
        uint8_t raw[128];
        int r = i2c_read_timeout_us(i2c1, 0x3A, raw, sizeof(raw), false, 200000);
        if (r <= 0) { sleep_ms(10); continue; }
        for (int i = 0; i < r; i++) {
            char c = (char)raw[i];
            if (c == (char)0xFF) continue;
            if (c == '$') slen = 0;
            if (slen < (int)sizeof(sbuf) - 1) {
                sbuf[slen++] = c;
                sbuf[slen]   = '\0';
            }
            if (c != '\n') continue;
            if (slen >= 6 && sbuf[0] == '$') {
                for (int k = slen - 1; k > 0 && (sbuf[k] == '\r' || sbuf[k] == '\n'); k--)
                    sbuf[k] = '\0';
                const char *body = sbuf + 1;
                if (match_prefix == NULL || strncmp(body, match_prefix, strlen(match_prefix)) == 0) {
                    printf("  $%s\n", body);
                }
            }
            slen = 0;
        }
    }
}

static void gnss_query(const char *cmd_body, const char *match_prefix, int wait_ms) {
    printf("\n  >> $%s\n", cmd_body);
    int w = gnss_send_nmea(cmd_body);
    if (w < 0) {
        printf("    (i2c write failed: %d)\n", w);
        return;
    }
    gnss_drain(wait_ms, match_prefix);
}

// CDB-ID 231 NMEA I2C message list LOW 32 bits — bit definitions from
// UM2229 Table §CDB-ID 201/228/231 (same layout as UART list).
// Default value on Teseo-LIV3FL is 0x00980056.
static void gnss_decode_cdb231(uint32_t v) {
    static const struct { uint8_t bit; const char *name; } bits[] = {
        { 1,  "GGA"      },
        { 2,  "GSA"      },
        { 3,  "GST"      },
        { 4,  "VTG"      },
        { 6,  "RMC"      },
        { 19, "GSV"      },
        { 20, "GLL"      },
        { 23, "PSTMCPU"  },
        { 25, "ZDA"      },
    };
    printf("    CDB-231 decode (NMEA I2C list, 0x%08lX):\n", (unsigned long)v);
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        int on = (v >> bits[i].bit) & 1u;
        printf("      bit %2u %-8s = %d\n", bits[i].bit, bits[i].name, on);
    }
}

void gnss_probe(void) {
    printf("\n--- GNSS Probe (read-only CDB dump) ---\n");

    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    // Drain whatever is currently in the output queue so the $PSTMGETPAR
    // responses are not mixed with periodic NMEA output.
    printf("\n  [drain 500 ms pre-probe]\n");
    gnss_drain(500, NULL);

    // Software version — confirms the module is responsive.
    gnss_query("PSTMGETSWVER,6", "PSTM", 800);

    // CDB-ID 200 — Application ON/OFF 1 (GPS/GLONASS/SBAS/PPS/etc).
    gnss_query("PSTMGETPAR,1200", "PSTMSETPAR", 800);

    // CDB-ID 227 — Application ON/OFF 2 (Galileo/BEIDOU/RTC).
    gnss_query("PSTMGETPAR,1227", "PSTMSETPAR", 800);

    // CDB-ID 231 — NMEA I2C message list LOW 32 bits. Default 0x00980056.
    gnss_query("PSTMGETPAR,1231", "PSTMSETPAR", 800);

    // CDB-ID 232 — NMEA I2C message list HIGH 32 bits. Default 0x0.
    gnss_query("PSTMGETPAR,1232", "PSTMSETPAR", 800);

    printf("\n  Note: $PSTMSETPAR response format is\n");
    printf("    $PSTMSETPAR,<CDB-ID>,<value>*<cs>\n");
    printf("    where <value> is the parameter's current value (hex).\n");
    printf("  CDB-231 default is 0x00980056 → GGA,GSA,VTG,RMC,GSV,GLL,PSTMCPU\n");

    gnss_decode_cdb231(0x00980056u);

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
    printf("\nDone\n");
}

// ---------------------------------------------------------------------------
// gnss_rfdiag — Poll $PSTMNOISE / $PSTMRF via $PSTMNMEAREQUEST (UM2229
// §10.2.38), and keep two negative-result probes for high-word messages as
// a permanent regression guard.
//
// **Firmware quirk (BINIMG_4.6.15.1_CP_LIV3FL_ARM, confirmed 2026-04-11):**
// `$PSTMNMEAREQUEST` only honors the low-word (`msglist_l`) argument. The
// high-word (`msglist_h`) argument is silently ignored, so any message at
// overall bit ≥ 32 is unreachable via this command. Confirmed by isolation
// probes for both `$PSTMLOWPOWERDATA` (high bit 0) and `$PSTMNOTCHSTATUS`
// (high bit 1): the command is ack'd but no payload follows. The only path
// to enable high-word messages is persistent CDB-1232 write + SAVEPAR + SRR,
// which is destructive and avoided here. See rev-a-bringup-log.md Step 14a.
//
// Message bit map (UM2229 Table 208, low word):
//   bit 5 (0x20) = $PSTMNOISE
//   bit 7 (0x80) = $PSTMRF    → low = 0xA0 polls both
// ---------------------------------------------------------------------------

void gnss_rfdiag(void) {
    printf("\n--- GNSS RF Diagnostic ---\n");

    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    gnss_drain(300, NULL);

    uint32_t n_noise = 0, n_notch = 0, n_rf = 0, n_lpd = 0;

    // Probe sequence. (1)(2) are permanent regression guards — if they ever
    // start returning payload on a newer Teseo firmware, delete this comment
    // and re-evaluate the high-word policy. (3)-(6) are the actual working
    // NOISE + RF polls.
    const char *const requests[] = {
        "PSTMNMEAREQUEST,0,1",    // regression guard: high bit 0 (LOWPOWERDATA)
        "PSTMNMEAREQUEST,0,2",    // regression guard: high bit 1 (NOTCHSTATUS)
        "PSTMNMEAREQUEST,A0,0",   // NOISE + RF
        "PSTMNMEAREQUEST,A0,0",
        "PSTMNMEAREQUEST,A0,0",
        "PSTMNMEAREQUEST,A0,0",
    };

    int n_polls = (int)(sizeof(requests) / sizeof(requests[0]));
    for (int poll = 0; poll < n_polls; poll++) {
        printf("\n  [poll %d/%d] %s\n", poll + 1, n_polls, requests[poll]);
        gnss_send_nmea(requests[poll]);

        // Give the chip ~800 ms to emit the burst; capture everything that
        // starts with PSTMNOISE / PSTMNOTCHSTATUS / PSTMRF, tally and print.
        char sbuf[256];
        int  slen = 0;
        absolute_time_t end = make_timeout_time_ms(800);
        while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
            uint8_t raw[128];
            int r = i2c_read_timeout_us(i2c1, 0x3A, raw, sizeof(raw), false, 200000);
            if (r <= 0) { sleep_ms(10); continue; }
            for (int i = 0; i < r; i++) {
                char c = (char)raw[i];
                if (c == (char)0xFF) continue;
                if (c == '$') slen = 0;
                if (slen < (int)sizeof(sbuf) - 1) {
                    sbuf[slen++] = c;
                    sbuf[slen]   = '\0';
                }
                if (c != '\n') continue;
                if (slen >= 6 && sbuf[0] == '$') {
                    for (int k = slen - 1; k > 0 && (sbuf[k] == '\r' || sbuf[k] == '\n'); k--)
                        sbuf[k] = '\0';
                    const char *body = sbuf + 1;
                    if      (strncmp(body, "PSTMNOISE,",         10) == 0) { n_noise++; printf("    $%s\n", body); }
                    else if (strncmp(body, "PSTMNOTCHSTATUS,",   16) == 0) { n_notch++; printf("    $%s\n", body); }
                    else if (strncmp(body, "PSTMRF,",             7) == 0) { n_rf++;    printf("    $%s\n", body); }
                    else if (strncmp(body, "PSTMLOWPOWERDATA,",  17) == 0) { n_lpd++;   printf("    $%s\n", body); }
                    else if (strncmp(body, "PSTMNMEAREQUEST",    15) == 0) {             printf("    $%s\n", body); }
                }
                slen = 0;
            }
        }
    }

    printf("\nCapture summary:\n");
    printf("  $PSTMNOISE        : %lu\n", (unsigned long)n_noise);
    printf("  $PSTMRF           : %lu\n", (unsigned long)n_rf);
    printf("  $PSTMLOWPOWERDATA : %lu   (regression guard, expect 0 on LIV3FL 4.6.15.1)\n", (unsigned long)n_lpd);
    printf("  $PSTMNOTCHSTATUS  : %lu   (regression guard, expect 0 on LIV3FL 4.6.15.1)\n", (unsigned long)n_notch);
    printf("\nInterpretation:\n");
    printf("  NOISE ~12500 baseline on quiet bench; sharp rise -> in-band RFI\n");
    printf("  PSTMRF n_sat==0              -> no satellite tracked (Issue 11)\n");
    printf("  LPD or NOTCH > 0             -> firmware upgraded; revisit Step 14a\n");

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
    printf("\nDone\n");
}

// ---------------------------------------------------------------------------
// Bus A register dump (IMU + Mag + Baro + GNSS)
// ---------------------------------------------------------------------------

static void dump_lsm6dsv16x(i2c_inst_t *i2c) {
    printf("\n  [0x6A] LSM6DSV16X IMU\n");
    uint8_t v;
    dev_read(i2c, 0x6A, 0x0F, &v);
    printf("    WHO_AM_I  : 0x%02X %s\n", v, v==0x70?"(OK)":"(UNEXPECTED - expected 0x70)");
    dev_read(i2c, 0x6A, 0x10, &v); printf("    CTRL1(XL) : 0x%02X  ODR_XL=%d FS_XL=%d\n", v, (v>>4)&0xF, (v>>2)&0x3);
    dev_read(i2c, 0x6A, 0x11, &v); printf("    CTRL2(G)  : 0x%02X  ODR_G=%d  FS_G=%d\n",  v, (v>>4)&0xF, (v>>2)&0x3);
    dev_read(i2c, 0x6A, 0x12, &v); printf("    CTRL3     : 0x%02X  BDU=%d SW_RST=%d\n", v, (v>>6)&1, v&1);
    dev_read(i2c, 0x6A, 0x1E, &v); printf("    STATUS    : 0x%02X  TDA=%d GDA=%d XLDA=%d\n", v, (v>>2)&1,(v>>1)&1,v&1);
    uint8_t lo, hi;
    dev_read(i2c, 0x6A, 0x28, &lo); dev_read(i2c, 0x6A, 0x29, &hi);
    printf("    ACCEL X   : 0x%04X (raw; ODR=0 at POR → power-down, enable via CTRL1)\n", (uint16_t)(hi<<8|lo));
}

static void dump_lis2mdl(i2c_inst_t *i2c) {
    printf("\n  [0x1E] LIS2MDL Magnetometer\n");
    uint8_t v;
    dev_read(i2c, 0x1E, 0x4F, &v);
    printf("    WHO_AM_I  : 0x%02X %s\n", v, v==0x40?"(OK)":"(UNEXPECTED - expected 0x40)");
    dev_read(i2c, 0x1E, 0x60, &v); printf("    CFG_REG_A : 0x%02X  COMP_TEMP=%d ODR=%d MD=%d\n", v, (v>>7)&1,(v>>2)&3,v&3);
    dev_read(i2c, 0x1E, 0x61, &v); printf("    CFG_REG_B : 0x%02X\n", v);
    dev_read(i2c, 0x1E, 0x62, &v); printf("    CFG_REG_C : 0x%02X  BDU=%d\n", v, (v>>4)&1);
    dev_read(i2c, 0x1E, 0x67, &v); printf("    STATUS    : 0x%02X  ZYXDA=%d\n", v, (v>>3)&1);
    uint8_t lo, hi;
    dev_read(i2c, 0x1E, 0x68, &lo); dev_read(i2c, 0x1E, 0x69, &hi);
    printf("    MAG X     : 0x%04X (raw; MD=11 at POR → idle, set MD=00 for continuous)\n", (uint16_t)(hi<<8|lo));
}

static void dump_lps22hh(i2c_inst_t *i2c) {
    printf("\n  [0x5D] LPS22HH Barometer\n");
    uint8_t v;
    dev_read(i2c, 0x5D, 0x0F, &v);
    printf("    WHO_AM_I  : 0x%02X %s\n", v, v==0xB3?"(OK)":"(UNEXPECTED - expected 0xB3)");
    dev_read(i2c, 0x5D, 0x10, &v); printf("    CTRL_REG1 : 0x%02X  ODR=%d BDU=%d\n", v, (v>>4)&7,(v>>1)&1);
    dev_read(i2c, 0x5D, 0x11, &v); printf("    CTRL_REG2 : 0x%02X  BOOT=%d SWRESET=%d ONE_SHOT=%d\n", v,(v>>7)&1,(v>>2)&1,v&1);
    dev_read(i2c, 0x5D, 0x27, &v); printf("    STATUS    : 0x%02X  T_DA=%d P_DA=%d\n", v,(v>>1)&1,v&1);
    uint8_t xl, lo, hi;
    dev_read(i2c, 0x5D, 0x28, &xl); dev_read(i2c, 0x5D, 0x29, &lo); dev_read(i2c, 0x5D, 0x2A, &hi);
    uint32_t p_raw = ((uint32_t)hi<<16)|((uint32_t)lo<<8)|xl;
    printf("    PRESS_RAW : 0x%06X = %.2f hPa (valid if ODR!=0)\n",
           (unsigned)p_raw, (float)(int32_t)(p_raw<<8>>8)/4096.0f);
    dev_read(i2c, 0x5D, 0x2B, &lo); dev_read(i2c, 0x5D, 0x2C, &hi);
    int16_t t_raw = (int16_t)(hi<<8|lo);
    printf("    TEMP_RAW  : 0x%04X = %.2f °C (valid if ODR!=0)\n", (uint16_t)t_raw, t_raw/100.0f);
}

void scan_bus_a(void) {
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / CW;

    // --- TFT header ---
    menu_clear(MC_BG);
    menu_str(0, 0, " Bus A Diagnostic   ", COLS, MC_TITLE, MC_TITBG, S);

    // --- I2C address scan (serial grid + collect found set) ---
    printf("\n--- Bus A scan+dump (Sensors, GPIO 34/35) ---\n");
    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    bool found[128] = {false};
    printf("   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int addr = 0; addr < 128; ++addr) {
        if (addr % 16 == 0) printf("%02x ", addr);
        int ret;
        uint8_t rxdata;
        if ((addr & 0x78) == 0 || (addr & 0x78) == 0x78)
            ret = PICO_ERROR_GENERIC;
        else
            ret = i2c_read_timeout_us(i2c1, addr, &rxdata, 1, false, 50000);
        if (ret >= 0) found[addr] = true;
        printf(ret < 0 ? "." : "@");
        printf(addr % 16 == 15 ? "\n" : "  ");
    }

    // --- Per-device probe: WHO_AM_I + register dump + TFT line ---
    typedef struct {
        uint8_t addr;
        const char *name;
        uint8_t who_reg;
        uint8_t who_expect;
    } dev_entry_t;
    static const dev_entry_t devs[] = {
        {0x6A, "IMU  6A", 0x0F, 0x70},
        {0x1E, "Mag  1E", 0x4F, 0x40},
        {0x5D, "Baro 5D", 0x0F, 0xB3},
        {0x3A, "GNSS 3A", 0x00, 0x00},  // no WHO_AM_I
    };
    int pass = 0;
    for (int i = 0; i < 4; i++) {
        const dev_entry_t *d = &devs[i];
        int row = 2 + i;
        char line[24];

        if (!found[d->addr]) {
            snprintf(line, sizeof(line), " %s  MISSING", d->name);
            menu_str(0, row * CH, line, COLS, MC_ERR, MC_BG, S);
            printf("\n  [0x%02X] %s — NOT FOUND\n", d->addr, d->name);
            continue;
        }

        if (d->addr == 0x3A) {
            // GNSS has no WHO_AM_I — ACK is sufficient
            snprintf(line, sizeof(line), " %s  ACK OK", d->name);
            menu_str(0, row * CH, line, COLS, MC_OK, MC_BG, S);
            pass++;
            dump_gnss(i2c1);
        } else {
            uint8_t v;
            dev_read(i2c1, d->addr, d->who_reg, &v);
            bool ok = (v == d->who_expect);
            snprintf(line, sizeof(line), " %s  ID:%02X %s",
                     d->name, v, ok ? "OK" : "BAD");
            menu_str(0, row * CH, line, COLS,
                     ok ? MC_OK : MC_ERR, MC_BG, S);
            if (ok) pass++;
        }

        // Full register dump to serial
        if (d->addr == 0x6A) dump_lsm6dsv16x(i2c1);
        else if (d->addr == 0x1E) dump_lis2mdl(i2c1);
        else if (d->addr == 0x5D) dump_lps22hh(i2c1);
    }

    // --- Summary row on TFT ---
    char summary[24];
    snprintf(summary, sizeof(summary), " Result: %d/4 pass", pass);
    menu_str(0, 7 * CH, summary, COLS,
             pass == 4 ? MC_OK : MC_ERR, MC_BG, S);

    menu_str(0, 9 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);

    printf("\n  === %d/4 devices OK ===\n\n", pass);

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// I2C bus scan
// ---------------------------------------------------------------------------

void perform_scan(i2c_inst_t *i2c, uint sda, uint scl, const char *bus_name) {
    printf("\n--- Scanning %s (SDA:%d, SCL:%d) ---\n", bus_name, sda, scl);

    i2c_init(i2c, 100 * 1000);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_pull_up(sda);
    gpio_pull_up(scl);

    printf("   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int addr = 0; addr < (1 << 7); ++addr) {
        if (addr % 16 == 0) printf("%02x ", addr);
        int ret;
        uint8_t rxdata;
        if ((addr & 0x78) == 0 || (addr & 0x78) == 0x78)
            ret = PICO_ERROR_GENERIC;
        else
            ret = i2c_read_timeout_us(i2c, addr, &rxdata, 1, false, 50000);
        printf(ret < 0 ? "." : "@");
        printf(addr % 16 == 15 ? "\n" : "  ");
    }

    i2c_deinit(i2c);
    gpio_set_function(sda, GPIO_FUNC_NULL);
    gpio_set_function(scl, GPIO_FUNC_NULL);
}
