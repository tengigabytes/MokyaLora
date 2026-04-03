#include "bringup.h"

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
    printf("\n--- LPS22HH Barometer one-shot read ---\n");

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
    dev_write(i2c1, 0x5D, 0x11, 0x10);  // IF_ADD_INC=1
    dev_write(i2c1, 0x5D, 0x10, 0x02);  // BDU=1, ODR=power-down (one-shot)
    dev_write(i2c1, 0x5D, 0x11, 0x11);  // ONE_SHOT trigger

    uint8_t status = 0;
    for (int i = 0; i < 100; i++) {
        dev_read(i2c1, 0x5D, 0x27, &status);
        if ((status & 0x03) == 0x03) break;
        sleep_ms(5);
    }
    printf("  STATUS   : 0x%02X  P_DA=%d T_DA=%d\n", status, status&1, (status>>1)&1);

    uint8_t reg = 0x28;
    uint8_t raw[5] = {0};
    i2c_write_timeout_us(i2c1, 0x5D, &reg, 1, true, 50000);
    i2c_read_timeout_us(i2c1, 0x5D, raw, 5, false, 100000);

    uint32_t p_raw = (uint32_t)raw[0] | ((uint32_t)raw[1]<<8) | ((uint32_t)raw[2]<<16);
    int16_t  t_raw = (int16_t)((uint16_t)raw[3] | ((uint16_t)raw[4]<<8));
    printf("  Pressure : %.2f hPa  (raw=0x%06X)\n", p_raw/4096.0f, (unsigned)p_raw);
    printf("  Temp     : %.2f °C   (raw=0x%04X)\n", t_raw/100.0f, (uint16_t)t_raw);

    dev_write(i2c1, 0x5D, 0x10, 0x00);  // power down

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
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
    printf("\n--- LIS2MDL Magnetometer one-shot read ---\n");

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
    dev_write(i2c1, 0x1E, 0x60, 0x80);  // comp_temp_en=1, odr=10Hz, continuous
    dev_write(i2c1, 0x1E, 0x61, 0x02);  // OFF_CANC every ODR
    dev_write(i2c1, 0x1E, 0x62, 0x10);  // BDU=1

    uint8_t status = 0;
    for (int i = 0; i < 40; i++) {
        dev_read(i2c1, 0x1E, 0x67, &status);
        if (status & 0x08) break;
        sleep_ms(5);
    }
    printf("  STATUS   : 0x%02X  ZYXDA=%d\n", status, (status>>3)&1);

    uint8_t reg = 0x68;
    uint8_t raw[6] = {0};
    i2c_write_timeout_us(i2c1, 0x1E, &reg, 1, true, 50000);
    i2c_read_timeout_us(i2c1, 0x1E, raw, 6, false, 100000);

    int16_t mx=(int16_t)((raw[1]<<8)|raw[0]);
    int16_t my=(int16_t)((raw[3]<<8)|raw[2]);
    int16_t mz=(int16_t)((raw[5]<<8)|raw[4]);
    printf("  Mag      : X=%+6d  Y=%+6d  Z=%+6d  raw\n", mx, my, mz);
    printf("           → X=%+8.1f  Y=%+8.1f  Z=%+8.1f  mGauss (1.5mG/LSB)\n",
           mx*1.5f, my*1.5f, mz*1.5f);

    dev_write(i2c1, 0x1E, 0x60, 0x02);  // power down (md=10)

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
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
    printf("\n--- LSM6DSV16X IMU one-shot read ---\n");
    printf("  (No UUID; WHO_AM_I=0x70 is the only chip identifier)\n");

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

    uint8_t status = 0;
    for (int i = 0; i < 40; i++) {
        dev_read(i2c1, 0x6A, 0x1E, &status);
        if ((status & 0x03) == 0x03) break;
        sleep_ms(5);
    }
    printf("  STATUS   : 0x%02X  XLDA=%d GDA=%d TDA=%d\n",
           status, status&1, (status>>1)&1, (status>>2)&1);

    uint8_t reg = 0x20;
    uint8_t raw[14] = {0};
    i2c_write_timeout_us(i2c1, 0x6A, &reg, 1, true, 50000);
    i2c_read_timeout_us(i2c1, 0x6A, raw, 14, false, 100000);

    int16_t t_raw = (int16_t)((raw[1]<<8)|raw[0]);
    printf("  Temp     : %+.1f °C\n", 25.0f + t_raw/256.0f);

    int16_t gx=(int16_t)((raw[3]<<8)|raw[2]);
    int16_t gy=(int16_t)((raw[5]<<8)|raw[4]);
    int16_t gz=(int16_t)((raw[7]<<8)|raw[6]);
    printf("  Gyro     : X=%+6d  Y=%+6d  Z=%+6d  raw\n", gx, gy, gz);
    printf("           → X=%+7.2f  Y=%+7.2f  Z=%+7.2f  dps (±250dps)\n",
           gx*0.00875f, gy*0.00875f, gz*0.00875f);

    int16_t ax=(int16_t)((raw[9]<<8)|raw[8]);
    int16_t ay=(int16_t)((raw[11]<<8)|raw[10]);
    int16_t az=(int16_t)((raw[13]<<8)|raw[12]);
    printf("  Accel    : X=%+6d  Y=%+6d  Z=%+6d  raw\n", ax, ay, az);
    printf("           → X=%+6.3f  Y=%+6.3f  Z=%+6.3f  g (±2g, expect ~1g on one axis)\n",
           ax*0.000061f, ay*0.000061f, az*0.000061f);

    dev_write(i2c1, 0x6A, 0x10, 0x00);  // power down XL
    dev_write(i2c1, 0x6A, 0x11, 0x00);  // power down G

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
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

void gnss_info(void) {
    printf("\n--- Teseo-LIV3FL GNSS Info ---\n");

    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    // Send $PSTMGETSWVER — checksum 0x28 (XOR of chars between $ and *)
    const char *ver_cmd = "$PSTMGETSWVER*28\r\n";
    int wr = i2c_write_timeout_us(i2c1, 0x3A,
                                   (const uint8_t *)ver_cmd,
                                   (int)strlen(ver_cmd), false, 100000);
    printf("  $PSTMGETSWVER write: %s\n",
           wr < 0 ? "NACK (device busy or not accepting cmds)" : "ACK");

    sleep_ms(1000);  // allow device to enqueue reply
    uint8_t buf[300];
    memset(buf, 0xFF, sizeof(buf));
    int r = i2c_read_timeout_us(i2c1, 0x3A, buf, sizeof(buf), false, 500000);
    if (r < 0) {
        printf("  read error (%d)\n", r);
    } else {
        printf("  NMEA stream (%d bytes):\n", r);
        gnss_print_buf(buf, r);
    }

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
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

void dump_bus_a(void) {
    printf("\n=== Bus A register dump (Sensors, GPIO 34/35) ===\n");
    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);

    dump_lsm6dsv16x(i2c1);
    dump_lis2mdl(i2c1);
    dump_lps22hh(i2c1);
    dump_gnss(i2c1);

    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
    printf("\n");
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
