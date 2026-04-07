#include "bringup.h"
#include "bringup_menu.h"

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
void key_gpio_init(void) {
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

void key_gpio_deinit(void) {
    for (int c = 0; c < KEY_COLS; c++)
        gpio_disable_pulls(KEY_COL_BASE + c);
    for (int r = 0; r < KEY_ROWS; r++) {
        gpio_put(KEY_ROW_BASE + r, 1);
        gpio_set_dir(KEY_ROW_BASE + r, GPIO_IN);
        gpio_disable_pulls(KEY_ROW_BASE + r);
    }
}

// Drive each ROW LOW; read which COLs are pulled low through diode (Vf ~0.3V < VIL 0.54V).
void key_scan_matrix(uint8_t pressed[KEY_ROWS]) {
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

// ---------------------------------------------------------------------------
// Key TFT test — visual keyboard layout matching physical device
// ---------------------------------------------------------------------------
//
// Layout: top section = nav cluster (FUNC/BACK, D-pad, SET/DEL/VOL+/-)
//         bottom section = 5x5 text key grid (rows 1-5, cols 0-4)
//
// Screen: 320x240 landscape.  Nav area: y 0-66.  Text area: y 70-240.

// ASCII-safe labels [matrix_row][matrix_col]
static const char *const key_tft_labels[KEY_ROWS][KEY_COLS] = {
    {"FUNC", "BACK", "LEFT", "DEL",  "VOL-", " UP" },
    {"1  2", "3  4", "5  6", "7  8", "9  0", " OK" },
    {"Q  W", "E  R", "T  Y", "U  I", "O  P", "DOWN"},
    {"A  S", "D  F", "G  H", "J  K", "  L ", "RGHT"},
    {"Z  X", "C  V", "B  N", "  M ", "Bpmf", "SET" },
    {"MODE", " TAB", "SPC ", " SYM", "Punc", "VOL+"},
};

// Calculator-mode labels (line 2) for text keys; NULL = no line 2
static const char *const kt_calc[KEY_ROWS][KEY_COLS] = {
    {NULL,   NULL,  NULL,  NULL,  NULL,  NULL },
    {"[ANS]","[7]", "[8]", "[9]", "[/]", NULL },
    {"[(]",  "[4]", "[5]", "[6]", "[x]", NULL },
    {"[)]",  "[1]", "[2]", "[3]", "[-]", NULL },
    {"[AC]", "[0]", "[.]", "[Ex]","[+]", NULL },
    {NULL,   NULL,  NULL,  NULL,  NULL,  NULL },
};

// --- Nav key pixel positions (row 0 keys + col 5 keys) ---
// Indexed: [0..5] = row 0 cols 0-5, [6..10] = rows 1-5 col 5
static const struct { int16_t x, y, w, h; } kt_nav[] = {
    {  2,  2, 56, 20 },  // [0] FUNC   (0,0)
    {  2, 44, 56, 20 },  // [1] BACK   (0,1) — exit key
    { 94, 23, 44, 20 },  // [2] LEFT   (0,2)
    {226, 44, 42, 20 },  // [3] DEL    (0,3)
    {274, 44, 44, 20 },  // [4] VOL-   (0,4)
    {138,  2, 44, 20 },  // [5] UP     (0,5)
    {138, 23, 44, 20 },  // [6] OK     (1,5)
    {138, 44, 44, 20 },  // [7] DOWN   (2,5)
    {182, 23, 44, 20 },  // [8] RIGHT  (3,5)
    {226,  2, 42, 20 },  // [9] SET    (4,5)
    {274,  2, 44, 20 },  // [10] VOL+  (5,5)
};

// --- Text key grid constants ---
#define KT_TXT_Y   70
#define KT_TXT_W   64    // 320 / 5
#define KT_TXT_H   34    // (240 - 70) / 5

// --- Colours ---
#define KT_NAV_BG   0x2104   // dark gray (nav unpressed)
#define KT_TXT_BG   0x18E3   // slightly lighter (text unpressed)
#define KT_PRESS_BG 0x001F   // blue (pressed)
#define KT_PRESS_FG 0xFFFF   // white (pressed text)
#define KT_EXIT_BG  0x4000   // dark red (BACK key)
#define KT_BORDER   0x4208   // key border
#define KT_DIM      0x7BEF   // dim text (calc labels)

static bool kt_is_nav(int r, int c) { return r == 0 || c == 5; }

static void kt_get_rect(int r, int c, int *ox, int *oy, int *ow, int *oh) {
    if (r == 0) {
        *ox = kt_nav[c].x;   *oy = kt_nav[c].y;
        *ow = kt_nav[c].w;   *oh = kt_nav[c].h;
    } else if (c == 5) {
        int i = 5 + r;       // rows 1-5 → indices 6-10
        *ox = kt_nav[i].x;   *oy = kt_nav[i].y;
        *ow = kt_nav[i].w;   *oh = kt_nav[i].h;
    } else {
        *ox = c * KT_TXT_W;
        *oy = KT_TXT_Y + (r - 1) * KT_TXT_H;
        *ow = KT_TXT_W;
        *oh = KT_TXT_H;
    }
}

static void kt_str(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
    for (int i = 0; s[i]; i++)
        menu_char(x + i * 6, y, s[i], fg, bg, 1);
}

static void kt_draw_key(int r, int c, bool pressed) {
    int x, y, w, h;
    kt_get_rect(r, c, &x, &y, &w, &h);

    bool is_back = (r == 0 && c == 1);
    bool nav = kt_is_nav(r, c);

    uint16_t bg, fg;
    if (is_back)        { bg = KT_EXIT_BG;  fg = MC_ERR; }
    else if (pressed)   { bg = KT_PRESS_BG;  fg = KT_PRESS_FG; }
    else                { bg = nav ? KT_NAV_BG : KT_TXT_BG; fg = MC_FG; }

    // Border (1 px) + fill
    menu_rect(x, y, w, 1, KT_BORDER);
    menu_rect(x, y + h - 1, w, 1, KT_BORDER);
    menu_rect(x, y, 1, h, KT_BORDER);
    menu_rect(x + w - 1, y, 1, h, KT_BORDER);
    menu_rect(x + 1, y + 1, w - 2, h - 2, bg);

    // Label line 1 (centred)
    const char *l1 = key_tft_labels[r][c];
    int len1 = (int)strlen(l1);
    int tx = x + (w - len1 * 6) / 2;

    if (nav) {
        kt_str(tx, y + (h - 8) / 2, l1, fg, bg);
    } else {
        kt_str(tx, y + 4, l1, fg, bg);
        // Line 2: calculator symbol (dimmed)
        const char *l2 = kt_calc[r][c];
        if (l2) {
            int len2 = (int)strlen(l2);
            int tx2 = x + (w - len2 * 6) / 2;
            kt_str(tx2, y + h - 12, l2, pressed ? KT_PRESS_FG : KT_DIM, bg);
        }
    }
}

void key_tft_test(void) {
    printf("\n--- Key TFT test (press keys; BACK to exit) ---\n");
    key_gpio_init();
    sleep_ms(10);

    menu_clear(MC_BG);

    // Separator between nav and text sections
    menu_rect(0, 67, 320, 2, MC_SEP);

    // Draw all keys unpressed
    for (int r = 0; r < KEY_ROWS; r++)
        for (int c = 0; c < KEY_COLS; c++)
            kt_draw_key(r, c, false);

    // Drain residual serial bytes
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}

    uint8_t prev[KEY_ROWS] = {0};
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + 120000;

    while (to_ms_since_boot(get_absolute_time()) < deadline) {
        int ch = getchar_timeout_us(0);
        if (ch == '\r' || ch == '\n') break;

        uint8_t cur[KEY_ROWS];
        key_scan_matrix(cur);

        // BACK key (R0C1) → exit
        if (cur[0] & (1u << 1)) break;

        // Redraw changed keys (skip BACK)
        for (int r = 0; r < KEY_ROWS; r++) {
            uint8_t changed = (uint8_t)(cur[r] ^ prev[r]);
            if (!changed) continue;
            for (int c = 0; c < KEY_COLS; c++) {
                if (!(changed & (1u << c))) continue;
                if (r == 0 && c == 1) continue;
                bool p = (cur[r] & (1u << c)) != 0;
                kt_draw_key(r, c, p);
                if (p) printf("  %s  (R%dC%d)\n", key_names[r][c], r, c);
            }
        }
        memcpy(prev, cur, KEY_ROWS);
        sleep_ms(20);
    }

    printf("Done\n");
}

// Monitor key presses for up to 60 s; exit on Enter key from serial.
void key_monitor(void) {
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
        if (back_key_pressed()) break;

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

void handle_command(const char *cmd) {
    if (strcmp(cmd, "baro") == 0) {
        baro_read();
    } else if (strcmp(cmd, "mag") == 0) {
        mag_read();
    } else if (strcmp(cmd, "imu") == 0) {
        imu_read();
    } else if (strcmp(cmd, "gnss_info") == 0) {
        gnss_info();
    } else if (strcmp(cmd, "dump_b") == 0) {
        dump_bus_b();
    } else if (strcmp(cmd, "scan_a") == 0) {
        scan_bus_a();
    } else if (strcmp(cmd, "scan_b") == 0) {
        scan_bus_b();
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
        led_control();
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
    } else if (strcmp(cmd, "mem_diag") == 0) {
        cmd_memory_diag();
    } else if (strcmp(cmd, "psram_full_tft") == 0) {
        cmd_psram_full_tft();
    } else if (strcmp(cmd, "psram_tuning") == 0) {
        cmd_psram_tuning();
    } else if (strcmp(cmd, "psram_debug") == 0) {
        cmd_psram_debug();
    } else if (strcmp(cmd, "psram_dma") == 0) {
        cmd_psram_dma_test();
    } else if (strcmp(cmd, "psram_rd_diag") == 0) {
        psram_rd_diag();
    } else if (strcmp(cmd, "key") == 0) {
        key_monitor();
    } else if (strcmp(cmd, "key_tft") == 0) {
        key_tft_test();
    } else if (strcmp(cmd, "tft") == 0) {
        tft_test();
    } else if (strcmp(cmd, "tft_fast") == 0) {
        tft_fast_test();
    } else if (strcmp(cmd, "rotate") == 0) {
        menu_tft_rotate();
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
        printf("  scan_a      -- Bus A diagnostic: scan + WHO_AM_I + reg dump (TFT)\n");
        printf("  dump_b      -- dump Bus B device registers (Charger/LED)\n");
        printf("  scan_b      -- Bus B diagnostic: scan + probe + reg dump (TFT)\n");
        printf("  status      -- BQ25622 charger status\n");
        printf("  adc         -- BQ25622 ADC: IBUS/IBAT/VBUS/VPMID/VBAT/VSYS\n");
        printf("  led         -- LED interactive control: per-bank on/off + duty (TFT)\n");
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
        printf("  rotate      -- Cycle TFT rotation: 0/90/180/270 degrees\n");
        printf("  gnss_tft    -- Step 14: GNSS outdoor test; streams NMEA + live display on TFT (Enter to stop)\n");
        printf("  key         -- keyboard monitor (prints key name on press; Enter to exit)\n");
        printf("  key_tft     -- visual keyboard test on TFT (keys light up; BACK to exit)\n");
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

// Global menu context — shared between init and loop
static menu_ctx_t g_menu;

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

    // Initialize LCD menu system (TFT + backlight + keyboard)
    menu_init(&g_menu);
}

/* bringup_repl_loop — main loop with LCD menu + serial REPL.
 * Polls both USB CDC serial and keypad at ~50 Hz.
 * Serial commands always take priority (for bringup_run.ps1 compatibility).
 * When serial is idle >2 s, the LCD menu becomes active. */
void bringup_repl_loop(void) {
    char line[32];
    int pos = 0;

    // Draw initial menu
    if (g_menu.lcd_dirty) {
        menu_draw(&g_menu);
        g_menu.lcd_dirty = false;
    }

    while (true) {
        // --- 1. Poll serial (highest priority, non-blocking) ---
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            g_menu.serial_last_ms = to_ms_since_boot(get_absolute_time());

            if (g_menu.state != MS_SERIAL_ACTIVE) {
                g_menu.state = MS_SERIAL_ACTIVE;
                menu_show_serial_banner(&g_menu);
            }

            // Normal REPL character accumulation
            if (c == '\r' || c == '\n') {
                line[pos] = '\0';
                pos = 0;
                printf("\n");
                handle_command(line);
                printf("> ");
                g_menu.serial_last_ms = to_ms_since_boot(get_absolute_time());
            } else if (c == '\b' || c == 127) {
                if (pos > 0) { pos--; printf("\b \b"); }
            } else if (pos < (int)(sizeof(line) - 1)) {
                line[pos++] = (char)c;
                printf("%c", c);
            }
        }

        // --- 2. Serial timeout → return to menu ---
        if (g_menu.state == MS_SERIAL_ACTIVE) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - g_menu.serial_last_ms > 2000) {
                g_menu.state = MS_MENU;
                // Only force TFT reinit if PIO was released (by tft/tft_fast/gnss_tft).
                // Normal serial commands don't touch TFT — just redraw the menu.
                g_menu.lcd_dirty = true;
            }
        }

        // --- 3. Poll keyboard (only in MENU state) ---
        if (g_menu.state == MS_MENU) {
            menu_key_t key = menu_scan_key();
            if (key != KEY_NONE) {
                menu_handle_key(&g_menu, key);
            }
        }

        // --- 4. Redraw LCD if dirty ---
        if (g_menu.lcd_dirty) {
            menu_draw(&g_menu);
            g_menu.lcd_dirty = false;
        }

        // --- 5. Yield (~50 Hz loop) ---
        sleep_ms(20);
    }
}

/* bringup_repl_run — convenience wrapper for the Core 0 binary. */
void bringup_repl_run(void) {
    bringup_repl_init();
    bringup_repl_loop();
}
