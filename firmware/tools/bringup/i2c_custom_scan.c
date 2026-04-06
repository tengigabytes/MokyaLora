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
    } else if (strcmp(cmd, "adc") == 0) {
        bus_b_init();
        bq25622_read_adc();
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
#if BRINGUP_WAV
    } else if (strcmp(cmd, "voice") == 0) {
        amp_voice();
    } else if (strcmp(cmd, "gc") == 0) {
        amp_gc();
    } else if (strcmp(cmd, "test01_8k") == 0) {
        amp_test01_8k();
    } else if (strcmp(cmd, "test01_16k") == 0) {
        amp_test01_16k();
    } else if (strcmp(cmd, "test01_44k") == 0) {
        amp_test01_44k();
    } else if (strcmp(cmd, "test01_48k") == 0) {
        amp_test01_48k();
    } else if (strcmp(cmd, "test01") == 0) {
        amp_test01_all();
#endif
    } else if (strcmp(cmd, "mic") == 0) {
        mic_test();
    } else if (strcmp(cmd, "mic_raw") == 0) {
        mic_raw();
    } else if (strcmp(cmd, "mic_loop") == 0) {
        mic_loopback();
    } else if (strcmp(cmd, "mic_rec") == 0) {
        mic_rec();
    } else if (strcmp(cmd, "mic_dump") == 0) {
        mic_dump();
    } else if (strcmp(cmd, "lora") == 0) {
        lora_test();
    } else if (strcmp(cmd, "lora_rx") == 0) {
        // TW LONG_FAST default: hash("LongFast")%20=15 → 920.0+0.125+15*0.25 = 923.875 MHz, SF11, BW250k
        lora_rx(923875000UL, 11, 0x08, 0x01, 30);
    } else if (strcmp(cmd, "lora_rx_mf") == 0) {
        // TW MEDIUM_FAST default: hash("MediumFast")%20=8 → 920.0+0.125+8*0.25 = 922.125 MHz, SF9, BW250k
        lora_rx(922125000UL, 9, 0x08, 0x01, 0);
    } else if (strcmp(cmd, "lora_rx_mf1") == 0) {
        // TW MEDIUM_FAST channel_num=1 (URL config): slot0 → 920.0+0.125 = 920.125 MHz, SF9, BW250k
        lora_rx(920125000UL, 9, 0x08, 0x01, 0);
    } else if (strcmp(cmd, "lora_dump") == 0) {
        lora_dump();
    } else if (strcmp(cmd, "sram") == 0) {
        sram_test();
    } else if (strcmp(cmd, "flash") == 0) {
        flash_test();
    } else if (strcmp(cmd, "psram") == 0) {
        psram_test();
    } else if (strcmp(cmd, "psram_full") == 0) {
        psram_full_test();
    } else if (strcmp(cmd, "psram_sweep") == 0) {
        psram_speed_test();
    } else if (strcmp(cmd, "flash_sweep") == 0) {
        flash_speed_test();
    } else if (strcmp(cmd, "psram_jlink") == 0) {
        psram_jlink_prep();
    } else if (strcmp(cmd, "psram_diag") == 0) {
        psram_diag_test();
    } else if (strcmp(cmd, "psram_probe") == 0) {
        psram_probe();
    } else if (strcmp(cmd, "key") == 0) {
        key_monitor();
    } else if (strcmp(cmd, "tft") == 0) {
        tft_test();
    } else if (strcmp(cmd, "tft_fast") == 0) {
        tft_fast_test();
    } else if (strcmp(cmd, "gnss_tft") == 0) {
        gnss_tft_test();
    } else if (strcmp(cmd, "charge_on") == 0) {
        bus_b_init();
        bq25622_enable_charge();
        bus_b_deinit();
    } else if (strcmp(cmd, "charge_scan") == 0) {
        bus_b_init();
        bq25622_enable_charge();
        sleep_ms(500);  // allow charging current to flow and wake BQ27441
        bus_b_deinit();
        perform_scan(i2c1, BUS_B_SDA, BUS_B_SCL, "Bus B (Power, i2c1) — post charge_on");
        printf("Expected: 0x6B(Charger)  0x55(FuelGauge — only if awake)  0x36(LED)\n");
    } else if (strcmp(cmd, "charge_off") == 0) {
        bus_b_init();
        bq25622_disable_charge();
        bus_b_deinit();
    } else if (strcmp(cmd, "bq27441") == 0) {
        bq27441_read();
    } else if (strcmp(cmd, "core1") == 0) {
        core1_test();
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
        printf("  adc         -- BQ25622 ADC: IBUS/IBAT/VBUS/VPMID/VBAT/VSYS\n");
        printf("  led         -- LED cycle (keyboard/red/green)\n");
        printf("  motor       -- vibration motor breathe x5\n");
        printf("  amp_test    -- NAU8315 constant tone 5 s at 80%% (hardware check)\n");
        printf("  amp         -- NAU8315 speaker breathe tone x5 (~444 Hz)\n");
        printf("  bee         -- Xiao Mi Feng melody at 40%% amp\n");
#if BRINGUP_WAV
        printf("  voice       -- play voice.wav (4.95 s, embedded in flash)\n");
        printf("  gc          -- play gc.wav (26.01 s, embedded in flash)\n");
        printf("  test01_8k   -- play test01 resampled from  8000 Hz\n");
        printf("  test01_16k  -- play test01 resampled from 16000 Hz\n");
        printf("  test01_44k  -- play test01 resampled from 44100 Hz\n");
        printf("  test01_48k  -- play test01 resampled from 48000 Hz\n");
        printf("  test01      -- play all four in sequence (A/B/C/D comparison)\n");
#endif
        printf("  mic         -- IM69D130 PDM mic: capture 32768 bits, check 1-density\n");
        printf("  mic_raw     -- PDM density monitor 10 s, no amp (speak to see shift)\n");
        printf("  mic_loop    -- mic -> speaker loopback 10 s (Enter to stop)\n");
        printf("  mic_rec     -- record 3 s into SRAM then play back (no loopback CLK hazard)\n");
        printf("  mic_dump    -- record 1 s into SRAM then dump raw PCM over serial (use recv_pcm_dump.py)\n");
        printf("  lora        -- SX1262 reset + GetStatus + ReadRegister (SyncWord check)\n");
        printf("  lora_rx     -- SX1262 RX 30 s    (923.875 MHz, SF11, TW LONG_FAST default)\n");
        printf("  lora_rx_mf  -- SX1262 RX cont.  (922.125 MHz, SF9,  TW MEDIUM_FAST default; 'exit' to stop)\n");
        printf("  lora_rx_mf1 -- SX1262 RX cont.  (920.125 MHz, SF9,  TW MEDIUM_FAST ch_num=1; 'exit' to stop)\n");
        printf("  lora_dump   -- SX1262 full status: errors, SyncWord, OCP, RxGain, RSSI, stats\n");
        printf("  sram        -- RP2350B internal SRAM 16 KB pattern test (5 patterns)\n");
        printf("  flash       -- read Flash JEDEC ID + unique ID (W25Q128JW)\n");
        printf("  psram       -- init + 4 KB pattern test (APS6404L, CS=GPIO0)\n");
        printf("  psram_full  -- full 8 MB two-pass write+verify test (address pattern)\n");
        printf("  psram_sweep -- PSRAM speed sweep: CLKDIV x RXDELAY, 256 KB per combo\n");
        printf("  flash_sweep -- Flash speed sweep: CLKDIV x RXDELAY from RAM (safe)\n");
        printf("  psram_jlink -- write sentinel to PSRAM[0..3], print J-Link mem32 command\n");
        printf("  psram_diag  -- CLKDIV=1 error analysis: address pattern, timing tuning\n");
        printf("  psram_probe -- QMI direct mode SPI probe: reset + read 8 bytes from CS1\n");
        printf("  tft         -- ST7789VI LCD: init + fill Red/Green/Blue/White/Black (1.5s each)\n");
        printf("  tft_fast    -- Step 13: TE freq, baseline FPS, DMA FPS, clkdiv=3 test, TE-gated fill\n");
        printf("  gnss_tft    -- Step 14: GNSS outdoor test; streams NMEA + live display on TFT (Enter to stop)\n");
        printf("  key         -- keyboard monitor (prints key name on press; Enter to exit)\n");
        printf("  charge_on   -- set VREG=4100mV IINDPM=100mA, enable BQ25622 charging\n");
        printf("  charge_off  -- disable BQ25622 charging\n");
        printf("  charge_scan -- charge_on + 500ms + scan Bus B (checks if BQ27441 wakes up)\n");
        printf("  bq27441     -- Step 12: BQ27441 fuel gauge: CTRL_STATUS, FLAGS, V/I/SOC/SOH\n");
        printf("  core1       -- Step 16 Stage A: Core 1 boot/FIFO/SRAM/GPIO test\n");
    } else if (cmd[0] != '\0') {
        printf("Unknown command: '%s'  (type 'help' for list)\n", cmd);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

/* bringup_repl_init — must be called from Core 0.
 * Initialises USB CDC (stdio_init_all registers the USB interrupt on Core 0's
 * NVIC), waits for rails to settle, and sets safe peripheral defaults.
 * Safe to call before multicore_launch_core1(). */
// I2C bus recovery for Bus B (GPIO 6 SCL, GPIO 7 SDA).
// BQ27441 has no I2C bus timeout (pure I2C, not SMBus). If the gauge sees
// glitches on SDA/SCL during cold boot (before pull-ups are active), its I2C
// state machine can lock up permanently until power-cycled.
// Fix: send 9 SCL clocks + STOP condition per I2C spec UM10204 §3.1.16
// before any peripheral touches these pins.
static void bus_b_i2c_recovery(void) {
    const uint scl = BUS_B_SCL;  // GPIO 6
    const uint sda = BUS_B_SDA;  // GPIO 7

    // Configure SCL as output HIGH, SDA as input with pull-up
    gpio_init(scl);
    gpio_set_dir(scl, GPIO_OUT);
    gpio_put(scl, 1);

    gpio_init(sda);
    gpio_set_dir(sda, GPIO_IN);
    gpio_pull_up(sda);

    sleep_us(10);

    // 9 clock pulses — if slave holds SDA low, each clock gives it a chance
    // to release.  If SDA goes HIGH before 9 clocks, slave has released.
    for (int i = 0; i < 9; i++) {
        gpio_put(scl, 0);
        sleep_us(5);
        gpio_put(scl, 1);
        sleep_us(5);
        if (gpio_get(sda)) break;  // SDA released — bus is free
    }

    // Generate STOP condition: SDA LOW→HIGH while SCL is HIGH
    gpio_set_dir(sda, GPIO_OUT);
    gpio_put(sda, 0);
    sleep_us(5);
    gpio_put(scl, 1);
    sleep_us(5);
    gpio_put(sda, 1);  // SDA rising edge while SCL HIGH = STOP
    sleep_us(10);

    // Release pins back to default (will be reconfigured by bus_b_init later)
    gpio_set_dir(scl, GPIO_IN);
    gpio_set_dir(sda, GPIO_IN);
    gpio_disable_pulls(scl);
    gpio_disable_pulls(sda);
}

void bringup_repl_init(void) {
    // PSRAM init first — GPIO0 boots with pull-down (CE# LOW = bus contention).
    // Must fix GPIO0 before any other peripheral touches the QSPI bus.
    psram_init();

    // Immediately clear any I2C bus lockup on Bus B before anything else.
    // BQ27441 may have locked up if SDA/SCL glitched during cold boot
    // (1.8V pull-up rail not yet stable when gauge powered from battery).
    bus_b_i2c_recovery();

    stdio_init_all();
    sleep_ms(2000);  // wait for USB enumeration and power rails

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
}

/* bringup_repl_loop — the interactive REPL loop.
 * Can be called from Core 0 or Core 1 after bringup_repl_init() has run on
 * Core 0 (USB CDC is already live at this point). */
void bringup_repl_loop(void) {
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

/* bringup_repl_run — convenience wrapper for the Core 0 binary. */
void bringup_repl_run(void) {
    bringup_repl_init();
    bringup_repl_loop();
}
