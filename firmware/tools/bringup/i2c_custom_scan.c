#include "bringup.h"

// ---------------------------------------------------------------------------
// Keypad scan (GPIO polling, bringup only — not the PIO+DMA production scan)
// ---------------------------------------------------------------------------

// Primary key label per [row][col], matching hardware-requirements.md matrix.
// key_names[row][col]: ROW = GPIO42+r driven LOW, COL = GPIO36+c read.
// SW numbering is column-major (SW1-6 = col0 top-to-bottom, etc.).
static const char *const key_names[KEY_ROWS][KEY_COLS] = {
    {"FUNC",  "BACK",  "LEFT",  "DEL",   "VOL-",  "UP"  },
    {"1/2",   "3/4",   "5/6",   "7/8",   "9/0",   "OK"  },
    {"Q/W",   "E/R",   "T/Y",   "U/I",   "O/P",   "DOWN"},
    {"A/S",   "D/F",   "G/H",   "J/K",   "L",     "RIGHT"},
    {"Z/X",   "C/V",   "B/N",   "M",     "ㄡㄥ",  "SET" },
    {"MODE",  "TAB",   "SPACE", "SYM",   "。.？", "VOL+"},
};

// Diode orientation: Anode=COL, Cathode=ROW.
// ROW = OUTPUT (drive LOW to select), COL = INPUT pull-up (read LOW when pressed).
static void key_gpio_init(void) {
    for (int c = 0; c < KEY_COLS; c++) {
        gpio_init(KEY_COL_BASE + c);
        gpio_set_dir(KEY_COL_BASE + c, GPIO_IN);
        gpio_pull_up(KEY_COL_BASE + c);
    }
    for (int r = 0; r < KEY_ROWS; r++) {
        gpio_init(KEY_ROW_BASE + r);
        gpio_set_dir(KEY_ROW_BASE + r, GPIO_OUT);
        gpio_put(KEY_ROW_BASE + r, 1);
    }
}

static void key_gpio_deinit(void) {
    for (int c = 0; c < KEY_COLS; c++)
        gpio_disable_pulls(KEY_COL_BASE + c);
    for (int r = 0; r < KEY_ROWS; r++) {
        gpio_put(KEY_ROW_BASE + r, 1);
        gpio_set_dir(KEY_ROW_BASE + r, GPIO_IN);
        gpio_disable_pulls(KEY_ROW_BASE + r);
    }
}

// Drive each ROW LOW; read which COLs are pulled low through diode (Vf ~0.3V < VIL 0.54V).
static void key_scan_matrix(uint8_t pressed[KEY_ROWS]) {
    for (int r = 0; r < KEY_ROWS; r++) pressed[r] = 0;
    for (int r = 0; r < KEY_ROWS; r++) {
        gpio_put(KEY_ROW_BASE + r, 0);
        sleep_us(10);
        for (int c = 0; c < KEY_COLS; c++) {
            if (!gpio_get(KEY_COL_BASE + c))
                pressed[r] |= (uint8_t)(1u << c);
        }
        gpio_put(KEY_ROW_BASE + r, 1);
    }
}

// Monitor key presses for up to 60 s; exit on Enter key from serial.
static void key_monitor(void) {
    printf("\n--- Keyboard monitor (press keys; Enter to exit) ---\n");
    key_gpio_init();
    sleep_ms(10);

    // Drain any residual bytes (e.g. '\n' left after command dispatch on '\r')
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}

    uint8_t prev[KEY_ROWS] = {0};
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 60000;

    while (to_ms_since_boot(get_absolute_time()) < deadline) {
        int ch = getchar_timeout_us(0);
        if (ch == '\r' || ch == '\n') break;

        uint8_t cur[KEY_ROWS];
        key_scan_matrix(cur);

        for (int r = 0; r < KEY_ROWS; r++) {
            uint8_t newly = (uint8_t)(cur[r] & ~prev[r]);
            for (int c = 0; c < KEY_COLS; c++) {
                if (newly & (1u << c))
                    printf("  %s  (R%dC%d)\n", key_names[r][c], r, c);
            }
        }
        memcpy(prev, cur, KEY_ROWS);
        sleep_ms(20);   // ~50 Hz scan
    }

    key_gpio_deinit();
    printf("Done\n");
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static void handle_command(const char *cmd) {
    if (strcmp(cmd, "baro") == 0) {
        baro_read();
    } else if (strcmp(cmd, "mag") == 0) {
        mag_read();
    } else if (strcmp(cmd, "imu") == 0) {
        imu_read();
    } else if (strcmp(cmd, "gnss_info") == 0) {
        gnss_info();
    } else if (strcmp(cmd, "dump_a") == 0) {
        dump_bus_a();
    } else if (strcmp(cmd, "dump_b") == 0) {
        dump_bus_b();
    } else if (strcmp(cmd, "scan_a") == 0) {
        perform_scan(i2c1, BUS_A_SDA, BUS_A_SCL, "Bus A (Sensors, i2c1)");
        printf("Expected: 0x6A(IMU)  0x1E(Mag)  0x5D(Baro)  0x3A(GNSS)\n");
    } else if (strcmp(cmd, "scan_b") == 0) {
        perform_scan(i2c1, BUS_B_SDA, BUS_B_SCL, "Bus B (Power, i2c1)");
        printf("Expected: 0x6B(Charger)  0x55(FuelGauge)  0x36(LED)\n");
    } else if (strcmp(cmd, "status") == 0) {
        bus_b_init();
        bq25622_print_status();
        bus_b_deinit();
    } else if (strcmp(cmd, "led") == 0) {
        bus_b_init();
        lm27965_cycle();
        bus_b_deinit();
    } else if (strcmp(cmd, "motor") == 0) {
        motor_breathe();
    } else if (strcmp(cmd, "amp_test") == 0) {
        amp_test();
    } else if (strcmp(cmd, "amp") == 0) {
        amp_breathe();
    } else if (strcmp(cmd, "bee") == 0) {
        amp_bee();
    } else if (strcmp(cmd, "mic") == 0) {
        mic_test();
    } else if (strcmp(cmd, "mic_loop") == 0) {
        mic_loopback();
    } else if (strcmp(cmd, "lora") == 0) {
        lora_test();
    } else if (strcmp(cmd, "lora_rx") == 0) {
        // AS923 Meshtastic Long_Fast: 923.125 MHz, SF11, BW250kHz, CR4/5, 30 s
        lora_rx(923125000UL, 11, 0x08, 0x01, 30);
    } else if (strcmp(cmd, "lora_dump") == 0) {
        lora_dump();
    } else if (strcmp(cmd, "flash") == 0) {
        flash_test();
    } else if (strcmp(cmd, "psram") == 0) {
        psram_test();
    } else if (strcmp(cmd, "key") == 0) {
        key_monitor();
    } else if (strcmp(cmd, "tft") == 0) {
        tft_test();
    } else if (strcmp(cmd, "charge_on") == 0) {
        bus_b_init();
        bq25622_enable_charge();
        bus_b_deinit();
    } else if (strcmp(cmd, "charge_off") == 0) {
        bus_b_init();
        bq25622_disable_charge();
        bus_b_deinit();
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        printf("Commands:\n");
        printf("  imu         -- LSM6DSV16X accel+gyro+temp one-shot read\n");
        printf("  baro        -- LPS22HH pressure+temp one-shot read\n");
        printf("  mag         -- LIS2MDL magnetometer one-shot read\n");
        printf("  gnss_info   -- Teseo-LIV3FL: send $PSTMGETSWVER + read 300-byte NMEA stream\n");
        printf("  dump_a      -- dump Bus A device registers (IMU/Mag/Baro/GNSS)\n");
        printf("  dump_b      -- dump Bus B device registers (Charger/LED)\n");
        printf("  scan_a      -- scan Bus A (sensors, GPIO 34/35)\n");
        printf("  scan_b      -- scan Bus B (power, GPIO 6/7)\n");
        printf("  status      -- BQ25622 charger status\n");
        printf("  led         -- LED cycle (keyboard/red/green)\n");
        printf("  motor       -- vibration motor breathe x5\n");
        printf("  amp_test    -- NAU8315 constant tone 5 s at 80%% (hardware check)\n");
        printf("  amp         -- NAU8315 speaker breathe tone x5 (~444 Hz)\n");
        printf("  bee         -- Xiao Mi Feng melody at 40%% amp\n");
        printf("  mic         -- IM69D130 PDM mic: capture 32768 bits, check 1-density\n");
        printf("  mic_loop    -- mic -> speaker loopback 10 s (Enter to stop)\n");
        printf("  lora        -- SX1262 reset + GetStatus + ReadRegister (SyncWord check)\n");
        printf("  lora_rx     -- SX1262 RX sniff 30 s (923.125 MHz, SF11, BW250k, AS923 Meshtastic)\n");
        printf("  lora_dump   -- SX1262 full status: errors, SyncWord, OCP, RxGain, RSSI, stats\n");
        printf("  flash       -- read Flash JEDEC ID + unique ID (W25Q128JW)\n");
        printf("  psram       -- init + 4 KB pattern test (APS6404L, CS=GPIO0)\n");
        printf("  tft         -- ST7789VI LCD: init + fill Red/Green/Blue/White/Black (1.5s each)\n");
        printf("  key         -- keyboard monitor (prints key name on press; Enter to exit)\n");
        printf("  charge_on   -- enable BQ25622 charging\n");
        printf("  charge_off  -- disable BQ25622 charging\n");
    } else if (cmd[0] != '\0') {
        printf("Unknown command: '%s'  (type 'help' for list)\n", cmd);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    stdio_init_all();
    sleep_ms(2000);  // wait for power rails to settle

    // Pre-compute unit sine table using hardware FPU sinf()
    precompute_sine();

    // Safe defaults: charge off, LEDs off
    bus_b_init();
    bq25622_disable_charge();
    lm_write(LM27965_GP, 0x20);  // all outputs off
    bus_b_deinit();

    printf("\n\n***************************************\n");
    printf("* MokyaLora RP2350 Bring-Up Shell     *\n");
    printf("***************************************\n");
    printf("Type 'help' for commands.\n> ");

    char line[32];
    int pos = 0;

    while (true) {
        int c = getchar_timeout_us(10000);
        if (c == PICO_ERROR_TIMEOUT) continue;

        if (c == '\r' || c == '\n') {
            line[pos] = '\0';
            pos = 0;
            printf("\n");
            handle_command(line);
            printf("> ");
        } else if (c == '\b' || c == 127) {
            if (pos > 0) { pos--; printf("\b \b"); }
        } else if (pos < (int)(sizeof(line) - 1)) {
            line[pos++] = (char)c;
            printf("%c", c);
        }
    }
}
