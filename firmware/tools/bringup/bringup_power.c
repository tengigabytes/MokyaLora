#include "bringup.h"
#include "bringup_menu.h"

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
        if (back_key_pressed()) break;
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

void dump_bus_b(void) {
    printf("\n=== Bus B register dump (Power, GPIO 6/7) ===\n");
    bus_b_init();
    bq25622_dump_regs();
    bq27441_dump_regs();
    lm27965_dump_regs();
    bus_b_deinit();
    printf("\n");
}

// ---------------------------------------------------------------------------
// Bus B Diagnostic — scan + probe + TFT display (mirrors scan_bus_a pattern)
// ---------------------------------------------------------------------------

void scan_bus_b(void) {
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / CW;

    // --- TFT header ---
    menu_clear(MC_BG);
    menu_str(0, 0, " Bus B Diagnostic   ", COLS, MC_TITLE, MC_TITBG, S);

    // --- I2C scan (serial grid) ---
    printf("\n--- Bus B scan+dump (Power, GPIO 6/7) ---\n");
    bus_b_init();

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

    // --- Per-device probe + TFT row ---
    typedef struct {
        uint8_t addr;
        const char *name;
    } bdev_t;
    static const bdev_t devs[] = {
        {BQ25622_ADDR, "Chg "},   // 0x6B
        {BQ27441_ADDR, "Fuel"},   // 0x55
        {LM27965_ADDR, "LED "},   // 0x36
    };
    int pass = 0;

    for (int i = 0; i < 3; i++) {
        const bdev_t *d = &devs[i];
        int row = 2 + i;
        char line[24];

        if (!found[d->addr]) {
            snprintf(line, sizeof(line), " %s %02X  MISSING    ", d->name, d->addr);
            menu_str(0, row * CH, line, COLS, MC_ERR, MC_BG, S);
            printf("\n  [0x%02X] %s -- NOT FOUND\n", d->addr, d->name);
            continue;
        }

        if (d->addr == BQ25622_ADDR) {
            uint8_t v;
            bq25622_reg_read(REG_PART_INFO, &v);
            uint8_t pn = (v >> 3) & 0x7;
            bool ok = (pn == 1);  // 1 = BQ25622
            snprintf(line, sizeof(line), " %s %02X  PN:%s  ",
                     d->name, d->addr, ok ? "25622" : "???  ");
            menu_str(0, row * CH, line, COLS, ok ? MC_OK : MC_ERR, MC_BG, S);
            if (ok) pass++;
            bq25622_dump_regs();
        } else if (d->addr == BQ27441_ADDR) {
            uint16_t dt;
            bool ok = (fg_ctrl_read(BQ27441_CTRL_DEVTYPE, &dt) >= 0 && dt == 0x0421);
            snprintf(line, sizeof(line), " %s %02X  DT:%s  ",
                     d->name, d->addr, ok ? "0421 " : "NACK ");
            menu_str(0, row * CH, line, COLS, ok ? MC_OK : MC_ERR, MC_BG, S);
            if (ok) pass++;
            bq27441_dump_regs();
        } else if (d->addr == LM27965_ADDR) {
            uint8_t v;
            bool ok = (lm_read(LM27965_GP, &v) >= 0);
            snprintf(line, sizeof(line), " %s %02X  GP:%s    ",
                     d->name, d->addr, ok ? "OK  " : "NACK");
            menu_str(0, row * CH, line, COLS, ok ? MC_OK : MC_ERR, MC_BG, S);
            if (ok) pass++;
            lm27965_dump_regs();
        }
    }

    // --- Summary ---
    char summary[24];
    snprintf(summary, sizeof(summary), " Result: %d/3 pass  ", pass);
    menu_str(0, 6 * CH, summary, COLS, pass == 3 ? MC_OK : MC_ERR, MC_BG, S);
    menu_str(0, 8 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);

    printf("\n  === %d/3 devices OK ===\n\n", pass);
    bus_b_deinit();
}
