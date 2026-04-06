// bringup_menu.c
// Interactive LCD menu system for MokyaLora bringup firmware.
//
// Displays a categorised menu on the ST7789VI TFT, navigated with the
// physical keypad (UP/DOWN/OK/BACK).  Serial commands always take priority
// to preserve bringup_run.ps1 automation compatibility.

#include "bringup.h"
#include "bringup_menu.h"
#include "tft_8080.pio.h"

// ---------------------------------------------------------------------------
// 5x8 bitmap font — column-major, LSB = top row, chars 0x20-0x7E (95 glyphs)
// Extracted from bringup_gnss_tft.c (public domain Adafruit 5x7 font).
// ---------------------------------------------------------------------------

const uint8_t MENU_FONT5[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 !
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 $
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 %
    {0x36,0x49,0x55,0x22,0x50}, // 0x26 &
    {0x00,0x05,0x03,0x00,0x00}, // 0x27 '
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 (
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 )
    {0x14,0x08,0x3E,0x08,0x14}, // 0x2A *
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B +
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C ,
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D -
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E .
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30 0
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 1
    {0x42,0x61,0x51,0x49,0x46}, // 0x32 2
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33 3
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 4
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36 6
    {0x01,0x71,0x09,0x05,0x03}, // 0x37 7
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 8
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39 9
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A :
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B ;
    {0x08,0x14,0x22,0x41,0x00}, // 0x3C <
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D =
    {0x00,0x41,0x22,0x14,0x08}, // 0x3E >
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F ?
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41 A
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 B
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44 D
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 E
    {0x7F,0x09,0x09,0x09,0x01}, // 0x46 F
    {0x3E,0x41,0x49,0x49,0x7A}, // 0x47 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 H
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 I
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A J
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B K
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C L
    {0x7F,0x02,0x04,0x02,0x7F}, // 0x4D M
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E N
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F O
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 R
    {0x46,0x49,0x49,0x49,0x31}, // 0x53 S
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 V
    {0x3F,0x40,0x38,0x40,0x3F}, // 0x57 W
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 X
    {0x07,0x08,0x70,0x08,0x07}, // 0x59 Y
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A Z
    {0x00,0x7F,0x41,0x41,0x00}, // 0x5B [
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C backslash
    {0x00,0x41,0x41,0x7F,0x00}, // 0x5D ]
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E ^
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F _
    {0x00,0x01,0x02,0x04,0x00}, // 0x60 `
    {0x20,0x54,0x54,0x54,0x78}, // 0x61 a
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62 b
    {0x38,0x44,0x44,0x44,0x20}, // 0x63 c
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64 d
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 e
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66 f
    {0x0C,0x52,0x52,0x52,0x3E}, // 0x67 g
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 h
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 i
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A j
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B k
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C l
    {0x7C,0x04,0x18,0x04,0x78}, // 0x6D m
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E n
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F o
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70 p
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71 q
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 r
    {0x48,0x54,0x54,0x54,0x20}, // 0x73 s
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74 t
    {0x3C,0x40,0x40,0x20,0x7C}, // 0x75 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77 w
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 0x79 y
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A z
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B {
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C |
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D }
    {0x10,0x08,0x08,0x10,0x08}, // 0x7E ~
};

// ---------------------------------------------------------------------------
// Shared TFT PIO helpers
// ---------------------------------------------------------------------------

static uint m_sm, m_off;
static bool m_pio_active = false;

// Screen rotation state (0=0°, 1=90°, 2=180°, 3=270°)
static uint8_t m_rotation = 0;
static int m_width = 240, m_height = 320;
static void apply_rotation(void);  // forward decl

bool menu_tft_start(void) {
    int n = pio_claim_unused_sm(pio1, false);
    if (n < 0) { printf("MENU: no free PIO1 SM\n"); return false; }
    m_sm = (uint)n;
    if (!pio_can_add_program(pio1, &tft_8080_program)) {
        printf("MENU: PIO1 program full\n");
        pio_sm_unclaim(pio1, m_sm);
        return false;
    }
    m_off = pio_add_program(pio1, &tft_8080_program);
    tft_8080_program_init(pio1, m_sm, m_off, TFT_D0_PIN, TFT_nWR_PIN, 4.0f);
    pio_sm_set_enabled(pio1, m_sm, true);
    m_pio_active = true;
    return true;
}

void menu_tft_stop(void) {
    if (!m_pio_active) return;
    pio_sm_set_enabled(pio1, m_sm, false);
    pio_remove_program(pio1, &tft_8080_program, m_off);
    pio_sm_unclaim(pio1, m_sm);
    for (uint i = 0; i < 8; i++)
        gpio_set_function(TFT_D0_PIN + i, GPIO_FUNC_NULL);
    gpio_set_function(TFT_nWR_PIN, GPIO_FUNC_NULL);
    m_pio_active = false;
}

// ---------------------------------------------------------------------------
// Low-level write helpers
// ---------------------------------------------------------------------------

void menu_tft_flush(void) {
    while (!pio_sm_is_tx_fifo_empty(pio1, m_sm)) tight_loop_contents();
    sleep_us(1);
}

void menu_tft_w8(uint8_t b) {
    pio_sm_put_blocking(pio1, m_sm, (uint32_t)b);
}

void menu_tft_cmd(uint8_t c) {
    menu_tft_flush();
    gpio_put(TFT_DCX_PIN, 0);
    menu_tft_w8(c);
    menu_tft_flush();
    gpio_put(TFT_DCX_PIN, 1);
}

void menu_tft_dat(uint8_t d) {
    menu_tft_w8(d);
}

void menu_tft_win(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    menu_tft_cmd(0x2A);
    menu_tft_dat(x0 >> 8); menu_tft_dat(x0 & 0xFF);
    menu_tft_dat(x1 >> 8); menu_tft_dat(x1 & 0xFF);
    menu_tft_cmd(0x2B);
    menu_tft_dat(y0 >> 8); menu_tft_dat(y0 & 0xFF);
    menu_tft_dat(y1 >> 8); menu_tft_dat(y1 & 0xFF);
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void menu_rect(int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    menu_tft_win((uint16_t)x, (uint16_t)y,
                 (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    menu_tft_cmd(0x2C);
    uint8_t hi = c >> 8, lo = c & 0xFF;
    for (int i = 0; i < w * h; i++) { menu_tft_w8(hi); menu_tft_w8(lo); }
    menu_tft_flush();
}

void menu_clear(uint16_t color) {
    menu_rect(0, 0, m_width, m_height, color);
}

void menu_char(int x, int y, char c, uint16_t fg, uint16_t bg, int sc) {
    if (c < 0x20 || c > 0x7E) c = ' ';
    const uint8_t *g = MENU_FONT5[(uint8_t)c - 0x20];
    int cw = 6 * sc, ch = 8 * sc;
    menu_tft_win((uint16_t)x, (uint16_t)y,
                 (uint16_t)(x + cw - 1), (uint16_t)(y + ch - 1));
    menu_tft_cmd(0x2C);
    for (int row = 0; row < 8; row++) {
        for (int ys = 0; ys < sc; ys++) {
            for (int col = 0; col < 6; col++) {
                uint16_t px = (col < 5 && ((g[col] >> row) & 1)) ? fg : bg;
                for (int xs = 0; xs < sc; xs++) {
                    menu_tft_w8((uint8_t)(px >> 8));
                    menu_tft_w8((uint8_t)(px & 0xFF));
                }
            }
        }
    }
    menu_tft_flush();
}

void menu_str(int x, int y, const char *s, int width,
              uint16_t fg, uint16_t bg, int sc) {
    int cx = x, slen = (int)strlen(s);
    for (int i = 0; i < width; i++) {
        if (cx + 6 * sc > m_width) break;
        char c = (i < slen) ? s[i] : ' ';
        menu_char(cx, y, c, fg, bg, sc);
        cx += 6 * sc;
    }
}

// ---------------------------------------------------------------------------
// ST7789VI init sequence (same as bringup_tft.c / bringup_gnss_tft.c)
// ---------------------------------------------------------------------------

void menu_tft_hw_init(void) {
    menu_tft_cmd(0x01); sleep_ms(150);  // SWRESET
    menu_tft_cmd(0x11); sleep_ms(120);  // SLPOUT
    menu_tft_cmd(0x3A); menu_tft_dat(0x55);    // COLMOD: RGB565
    menu_tft_cmd(0x36); menu_tft_dat(0x00);    // MADCTL: portrait, RGB
    menu_tft_cmd(0xB2);
    menu_tft_dat(0x0C); menu_tft_dat(0x0C); menu_tft_dat(0x00);
    menu_tft_dat(0x33); menu_tft_dat(0x33);
    menu_tft_cmd(0xB7); menu_tft_dat(0x35);
    menu_tft_cmd(0xBB); menu_tft_dat(0x19);
    menu_tft_cmd(0xC0); menu_tft_dat(0x2C);
    menu_tft_cmd(0xC2); menu_tft_dat(0x01);
    menu_tft_cmd(0xC3); menu_tft_dat(0x12);
    menu_tft_cmd(0xC4); menu_tft_dat(0x20);
    menu_tft_cmd(0xC6); menu_tft_dat(0x0F);
    menu_tft_cmd(0xD0); menu_tft_dat(0xA4); menu_tft_dat(0xA1);
    menu_tft_cmd(0xE0);
    menu_tft_dat(0xD0); menu_tft_dat(0x04); menu_tft_dat(0x0D);
    menu_tft_dat(0x11); menu_tft_dat(0x13); menu_tft_dat(0x2B);
    menu_tft_dat(0x3F); menu_tft_dat(0x54); menu_tft_dat(0x4C);
    menu_tft_dat(0x18); menu_tft_dat(0x0D); menu_tft_dat(0x0B);
    menu_tft_dat(0x1F); menu_tft_dat(0x23);
    menu_tft_cmd(0xE1);
    menu_tft_dat(0xD0); menu_tft_dat(0x04); menu_tft_dat(0x0C);
    menu_tft_dat(0x11); menu_tft_dat(0x13); menu_tft_dat(0x2C);
    menu_tft_dat(0x3F); menu_tft_dat(0x44); menu_tft_dat(0x51);
    menu_tft_dat(0x2F); menu_tft_dat(0x1F); menu_tft_dat(0x1F);
    menu_tft_dat(0x20); menu_tft_dat(0x23);
    menu_tft_cmd(0x2A);
    menu_tft_dat(0x00); menu_tft_dat(0x00); menu_tft_dat(0x00); menu_tft_dat(0xEF);
    menu_tft_cmd(0x2B);
    menu_tft_dat(0x00); menu_tft_dat(0x00); menu_tft_dat(0x01); menu_tft_dat(0x3F);
    menu_tft_cmd(0x21);               // INVON
    menu_tft_cmd(0x29); sleep_ms(20); // DISPON
}

// ---------------------------------------------------------------------------
// Full TFT init (GPIO + PIO + ST7789 + backlight)
// ---------------------------------------------------------------------------

bool menu_tft_init(void) {
    // TFT GPIO init
    gpio_init(TFT_nCS_PIN);  gpio_set_dir(TFT_nCS_PIN,  GPIO_OUT); gpio_put(TFT_nCS_PIN,  1);
    gpio_init(TFT_DCX_PIN);  gpio_set_dir(TFT_DCX_PIN,  GPIO_OUT); gpio_put(TFT_DCX_PIN,  1);
    gpio_init(TFT_nRST_PIN); gpio_set_dir(TFT_nRST_PIN, GPIO_OUT); gpio_put(TFT_nRST_PIN, 1);

    if (!menu_tft_start()) return false;

    // Hardware reset
    gpio_put(TFT_nRST_PIN, 0); sleep_ms(10);
    gpio_put(TFT_nRST_PIN, 1); sleep_ms(120);
    gpio_put(TFT_nCS_PIN, 0);

    menu_tft_hw_init();

    // Reset rotation to portrait
    m_rotation = 0; m_width = 240; m_height = 320;

    // Backlight on via LM27965 Bank A
    bus_b_init();
    lm_write(LM27965_BANKA, 0x16);  // 40%
    lm_write(LM27965_GP,    0x21);  // ENA=1
    bus_b_deinit();

    return true;
}

bool menu_tft_reinit(void) {
    // Re-claim PIO after a test released it
    if (m_pio_active) return true;  // already active

    // Re-init all TFT GPIO (tests may have released them)
    gpio_init(TFT_nCS_PIN);  gpio_set_dir(TFT_nCS_PIN,  GPIO_OUT); gpio_put(TFT_nCS_PIN,  1);
    gpio_init(TFT_DCX_PIN);  gpio_set_dir(TFT_DCX_PIN,  GPIO_OUT); gpio_put(TFT_DCX_PIN,  1);
    gpio_init(TFT_nRST_PIN); gpio_set_dir(TFT_nRST_PIN, GPIO_OUT); gpio_put(TFT_nRST_PIN, 1);

    if (!menu_tft_start()) return false;

    // Hardware reset — required after tft_fast_test or gnss_tft_test
    // leaves the ST7789 controller in an unknown state.
    gpio_put(TFT_nRST_PIN, 0); sleep_ms(10);
    gpio_put(TFT_nRST_PIN, 1); sleep_ms(120);
    gpio_put(TFT_nCS_PIN,  0);

    // Full ST7789 re-init
    menu_tft_hw_init();

    // Restore current rotation (MADCTL was reset by SWRESET in hw_init)
    if (m_rotation != 0) apply_rotation();

    // Backlight on
    bus_b_init();
    lm_write(LM27965_BANKA, 0x16);
    lm_write(LM27965_GP,    0x21);
    bus_b_deinit();

    return true;
}

// ---------------------------------------------------------------------------
// Screen rotation
// ---------------------------------------------------------------------------

// MADCTL values for each rotation:
//   0° (portrait):         0x00  (240×320)
//   90° (landscape):       0x60  MV=1 MX=1  (320×240)
//   180° (portrait inv):   0xC0  MX=1 MY=1  (240×320)
//   270° (landscape inv):  0xA0  MV=1 MY=1  (320×240)
static const uint8_t madctl_table[4] = { 0x00, 0x60, 0xC0, 0xA0 };

static void apply_rotation(void) {
    menu_tft_cmd(0x36); menu_tft_dat(madctl_table[m_rotation]);
    if (m_rotation & 1) { // landscape
        m_width = 320; m_height = 240;
        menu_tft_cmd(0x2A);
        menu_tft_dat(0x00); menu_tft_dat(0x00);
        menu_tft_dat(0x01); menu_tft_dat(0x3F); // 0..319
        menu_tft_cmd(0x2B);
        menu_tft_dat(0x00); menu_tft_dat(0x00);
        menu_tft_dat(0x00); menu_tft_dat(0xEF); // 0..239
    } else { // portrait
        m_width = 240; m_height = 320;
        menu_tft_cmd(0x2A);
        menu_tft_dat(0x00); menu_tft_dat(0x00);
        menu_tft_dat(0x00); menu_tft_dat(0xEF); // 0..239
        menu_tft_cmd(0x2B);
        menu_tft_dat(0x00); menu_tft_dat(0x00);
        menu_tft_dat(0x01); menu_tft_dat(0x3F); // 0..319
    }
}

int menu_tft_rotate(void) {
    m_rotation = (m_rotation + 1) & 3;
    if (m_pio_active) apply_rotation();
    printf("TFT rotation: %d deg (%dx%d)\n", m_rotation * 90, m_width, m_height);
    return m_rotation * 90;
}

int menu_tft_width(void)  { return m_width; }
int menu_tft_height(void) { return m_height; }

// ---------------------------------------------------------------------------
// Menu page definitions
// ---------------------------------------------------------------------------

// Thin wrappers for commands that need bus_b_init/deinit bracketing
static void cmd_status(void)     { bus_b_init(); bq25622_print_status(); bus_b_deinit(); }
static void cmd_adc(void)        { bus_b_init(); bq25622_read_adc(); bus_b_deinit(); }
static void cmd_charge_on(void)  { bus_b_init(); bq25622_enable_charge(); bus_b_deinit(); }
static void cmd_charge_off(void) { bus_b_init(); bq25622_disable_charge(); bus_b_deinit(); }
static void cmd_charge_scan(void) {
    bus_b_init();
    bq25622_enable_charge();
    sleep_ms(500);
    bus_b_deinit();
    perform_scan(i2c1, BUS_B_SDA, BUS_B_SCL, "Bus B (Power, i2c1) -- post charge_on");
    printf("Expected: 0x6B(Charger)  0x55(FuelGauge -- only if awake)  0x36(LED)\n");
}
static void cmd_led(void)        { bus_b_init(); lm27965_cycle(); bus_b_deinit(); }
static void cmd_scan_a(void) {
    perform_scan(i2c1, BUS_A_SDA, BUS_A_SCL, "Bus A (Sensors, i2c1)");
    printf("Expected: 0x6A(IMU)  0x1E(Mag)  0x5D(Baro)  0x3A(GNSS)\n");
}
static void cmd_scan_b(void) {
    perform_scan(i2c1, BUS_B_SDA, BUS_B_SCL, "Bus B (Power, i2c1)");
    printf("Expected: 0x6B(Charger)  0x55(FuelGauge)  0x36(LED)\n");
}
static void cmd_lora_rx_lf(void) {
    lora_rx(923875000UL, 11, 0x08, 0x01, 30);
}
static void cmd_lora_rx_mf(void) {
    lora_rx(922125000UL, 9, 0x08, 0x01, 0);
}
static void cmd_lora_rx_mf1(void) {
    lora_rx(920125000UL, 9, 0x08, 0x01, 0);
}

// --- Sensors page ---
static const menu_item_t page_sensors_items[] = {
    {"IMU read",       imu_read,     NULL},
    {"Baro read",      baro_read,    NULL},
    {"Mag read",       mag_read,     NULL},
    {"GNSS info",      gnss_info,    NULL},
    {"GNSS TFT live",  gnss_tft_test, NULL},
    {"Scan Bus A",     cmd_scan_a,   NULL},
    {"Dump Bus A",     dump_bus_a,   NULL},
};
static const menu_page_t page_sensors = {
    "Sensors", page_sensors_items, sizeof(page_sensors_items) / sizeof(page_sensors_items[0])
};

// --- Power page ---
static const menu_item_t page_power_items[] = {
    {"Charger status",  cmd_status,      NULL},
    {"Charger ADC",     cmd_adc,         NULL},
    {"Charge ON",       cmd_charge_on,   NULL},
    {"Charge OFF",      cmd_charge_off,  NULL},
    {"Charge scan",     cmd_charge_scan, NULL},
    {"BQ27441 gauge",   bq27441_read,    NULL},
    {"LED cycle",       cmd_led,         NULL},
    {"Motor breathe",   motor_breathe,   NULL},
    {"Scan Bus B",      cmd_scan_b,      NULL},
    {"Dump Bus B",      dump_bus_b,      NULL},
};
static const menu_page_t page_power = {
    "Power", page_power_items, sizeof(page_power_items) / sizeof(page_power_items[0])
};

// --- Audio page ---
static const menu_item_t page_audio_items[] = {
    {"Amp test",       amp_test,     NULL},
    {"Amp breathe",    amp_breathe,  NULL},
    {"Bee melody",     amp_bee,      NULL},
    {"Mic test",       mic_test,     NULL},
    {"Mic raw 10s",    mic_raw,      NULL},
    {"Mic loopback",   mic_loopback, NULL},
    {"Mic record",     mic_rec,      NULL},
    {"Mic dump",       mic_dump,     NULL},
};
static const menu_page_t page_audio = {
    "Audio", page_audio_items, sizeof(page_audio_items) / sizeof(page_audio_items[0])
};

// --- Memory page ---
static const menu_item_t page_memory_items[] = {
    {"SRAM test",      sram_test,        NULL},
    {"Flash JEDEC",    flash_test,       NULL},
    {"PSRAM test",     psram_test,       NULL},
    {"PSRAM full 8MB", psram_full_test,  NULL},
    {"PSRAM sweep",    psram_speed_test, NULL},
    {"PSRAM diag",     psram_diag_test,  NULL},
    {"PSRAM probe",    psram_probe,      NULL},
    {"PSRAM J-Link",   psram_jlink_prep, NULL},
    {"Flash sweep",    flash_speed_test, NULL},
};
static const menu_page_t page_memory = {
    "Memory", page_memory_items, sizeof(page_memory_items) / sizeof(page_memory_items[0])
};

// --- LoRa page ---
static const menu_item_t page_lora_items[] = {
    {"LoRa test",       lora_test,       NULL},
    {"LoRa RX 30s LF",  cmd_lora_rx_lf, NULL},
    {"LoRa RX MF",      cmd_lora_rx_mf,  NULL},
    {"LoRa RX MF1",     cmd_lora_rx_mf1, NULL},
    {"LoRa dump",       lora_dump,       NULL},
};
static const menu_page_t page_lora = {
    "LoRa", page_lora_items, sizeof(page_lora_items) / sizeof(page_lora_items[0])
};

// Rotation wrapper — cycles 0→90→180→270 and redraws menu
static void cmd_rotate(void) {
    menu_tft_rotate();
}

// --- Display page ---
static const menu_item_t page_display_items[] = {
    {"TFT test",       tft_test,        NULL},
    {"TFT fast",       tft_fast_test,   NULL},
    {"Rotate screen",  cmd_rotate,      NULL},
};
static const menu_page_t page_display = {
    "Display", page_display_items, sizeof(page_display_items) / sizeof(page_display_items[0])
};

// --- Keyboard page ---
static const menu_item_t page_keyboard_items[] = {
    {"Key monitor",    key_monitor,     NULL},
};
static const menu_page_t page_keyboard = {
    "Keyboard", page_keyboard_items, sizeof(page_keyboard_items) / sizeof(page_keyboard_items[0])
};

// --- Core 1 page ---
static const menu_item_t page_core1_items[] = {
    {"Core1 test",     core1_test,      NULL},
};
static const menu_page_t page_core1 = {
    "Core 1", page_core1_items, sizeof(page_core1_items) / sizeof(page_core1_items[0])
};

// --- Root menu ---
static const menu_item_t root_items[] = {
    {"Sensors",   NULL, &page_sensors},
    {"Power",     NULL, &page_power},
    {"Audio",     NULL, &page_audio},
    {"Memory",    NULL, &page_memory},
    {"LoRa",      NULL, &page_lora},
    {"Display",   NULL, &page_display},
    {"Keyboard",  NULL, &page_keyboard},
    {"Core 1",    NULL, &page_core1},
};
static const menu_page_t root_page = {
    "MokyaLora Bringup", root_items, sizeof(root_items) / sizeof(root_items[0])
};

const menu_page_t *menu_root_page(void) { return &root_page; }

// ---------------------------------------------------------------------------
// Keyboard scanning for navigation
// ---------------------------------------------------------------------------

// Navigation key positions in the 6x6 matrix:
//   UP    = R0C5    DOWN  = R2C5    LEFT  = R0C2
//   RIGHT = R3C5    OK    = R1C5    BACK  = R0C1

menu_key_t menu_scan_key(void) {
    static uint32_t last_event_ms = 0;
    static uint8_t  prev_state = 0;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_event_ms < 80) return KEY_NONE;  // debounce

    uint8_t pressed[KEY_ROWS];
    key_scan_matrix(pressed);

    // Pack nav key states into a bitmask
    uint8_t state = 0;
    if (pressed[0] & (1u << 5)) state |= (1u << 0); // UP
    if (pressed[2] & (1u << 5)) state |= (1u << 1); // DOWN
    if (pressed[0] & (1u << 2)) state |= (1u << 2); // LEFT
    if (pressed[3] & (1u << 5)) state |= (1u << 3); // RIGHT
    if (pressed[1] & (1u << 5)) state |= (1u << 4); // OK
    if (pressed[0] & (1u << 1)) state |= (1u << 5); // BACK

    uint8_t newly = state & ~prev_state;  // rising edges only
    prev_state = state;

    if (!newly) return KEY_NONE;
    last_event_ms = now;

    // Priority: BACK > OK > UP > DOWN > LEFT > RIGHT
    if (newly & (1u << 5)) return KEY_BACK;
    if (newly & (1u << 4)) return KEY_OK;
    if (newly & (1u << 0)) return KEY_UP;
    if (newly & (1u << 1)) return KEY_DOWN;
    if (newly & (1u << 2)) return KEY_LEFT;
    if (newly & (1u << 3)) return KEY_RIGHT;
    return KEY_NONE;
}

// ---------------------------------------------------------------------------
// Menu rendering
// ---------------------------------------------------------------------------

#define MENU_SCALE     2
#define MENU_CELL_W   (6 * MENU_SCALE)   // 12 px
#define MENU_CELL_H   (8 * MENU_SCALE)   // 16 px
// Dynamic columns/rows based on current rotation
#define MENU_COLS     (m_width  / MENU_CELL_W)
#define MENU_ROWS     (m_height / MENU_CELL_H)
#define VISIBLE_ITEMS (MENU_ROWS - 4)     // title + sep + sep + hint = 4 rows

void menu_draw(menu_ctx_t *ctx) {
    if (!m_pio_active) {
        if (!menu_tft_reinit()) return;
        ctx->tft_active = true;
    }

    // Title bar (row 0)
    menu_str(0, 0, ctx->page->title, MENU_COLS, MC_TITLE, MC_TITBG, MENU_SCALE);

    // Separator (row 1)
    menu_rect(0, MENU_CELL_H, m_width, MENU_CELL_H, MC_SEP);

    // Menu items (rows 2..N)
    int vis = VISIBLE_ITEMS;
    int y = 2 * MENU_CELL_H;
    for (int i = 0; i < vis; i++) {
        int idx = ctx->scroll_top + i;
        if (idx < ctx->page->count) {
            bool sel = (idx == ctx->cursor);
            uint16_t fg = sel ? MC_HITEXT : MC_FG;
            uint16_t bg = sel ? MC_HILITE : MC_BG;

            int cols = MENU_COLS;
            char line[40];
            const menu_item_t *item = &ctx->page->items[idx];
            if (item->sub) {
                snprintf(line, sizeof(line), "%c %-*s>",
                         sel ? '>' : ' ', cols - 3, item->label);
            } else {
                snprintf(line, sizeof(line), "%c %-*s",
                         sel ? '>' : ' ', cols - 2, item->label);
            }
            menu_str(0, y, line, cols, fg, bg, MENU_SCALE);
        } else {
            menu_rect(0, y, m_width, MENU_CELL_H, MC_BG);
        }
        y += MENU_CELL_H;
    }

    // Separator
    int sep_row = 2 + vis;
    menu_rect(0, sep_row * MENU_CELL_H, m_width, MENU_CELL_H, MC_SEP);

    // Hint bar
    int hint_row = sep_row + 1;
    const char *hint = (ctx->parent)
        ? " [OK]Run [BACK]Back "
        : " [OK]Open [UP/DN]Nav";
    menu_str(0, hint_row * MENU_CELL_H, hint, MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
}

void menu_show_serial_banner(menu_ctx_t *ctx) {
    if (!m_pio_active) return;
    menu_clear(MC_BG);
    menu_str(0, 0, " Serial Active      ", MENU_COLS, MC_TITLE, MC_TITBG, MENU_SCALE);
    menu_str(0, 4 * MENU_CELL_H, "  COM port control  ", MENU_COLS, MC_FG, MC_BG, MENU_SCALE);
    menu_str(0, 6 * MENU_CELL_H, "  Menu resumes when ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
    menu_str(0, 7 * MENU_CELL_H, "  serial is idle.   ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
}

void menu_show_test_header(menu_ctx_t *ctx, const char *label) {
    if (!m_pio_active) return;
    menu_clear(MC_BG);
    menu_str(0, 0, " Running Test       ", MENU_COLS, MC_TITLE, MC_TITBG, MENU_SCALE);
    char line[21];
    snprintf(line, sizeof(line), "  %-18s", label);
    menu_str(0, 3 * MENU_CELL_H, line, MENU_COLS, MC_HITEXT, MC_BG, MENU_SCALE);
    menu_str(0, 6 * MENU_CELL_H, "  Output on serial. ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
    menu_str(0, 7 * MENU_CELL_H, "  BACK key to stop. ", MENU_COLS, MC_HINT, MC_BG, MENU_SCALE);
}

// ---------------------------------------------------------------------------
// Menu input handling
// ---------------------------------------------------------------------------

void menu_handle_key(menu_ctx_t *ctx, menu_key_t key) {
    switch (key) {
    case KEY_UP:
        if (ctx->cursor > 0) {
            ctx->cursor--;
            if (ctx->cursor < ctx->scroll_top)
                ctx->scroll_top = ctx->cursor;
            ctx->lcd_dirty = true;
        }
        break;

    case KEY_DOWN:
        if (ctx->cursor < ctx->page->count - 1) {
            ctx->cursor++;
            if (ctx->cursor >= ctx->scroll_top + VISIBLE_ITEMS)
                ctx->scroll_top = ctx->cursor - VISIBLE_ITEMS + 1;
            ctx->lcd_dirty = true;
        }
        break;

    case KEY_OK: {
        const menu_item_t *item = &ctx->page->items[ctx->cursor];
        if (item->sub) {
            // Enter sub-page
            ctx->parent = ctx->page;
            ctx->page = item->sub;
            ctx->cursor = 0;
            ctx->scroll_top = 0;
            ctx->lcd_dirty = true;
        } else if (item->run) {
            // Run test
            ctx->state = MS_TEST_RUNNING;
            menu_show_test_header(ctx, item->label);

            // Init BACK key for test interruption
            back_key_init();

            printf("\n--- [MENU] Running: %s ---\n", item->label);
            item->run();
            printf("--- [MENU] Done: %s ---\n", item->label);

            back_key_deinit();

            // Return to menu — force TFT reinit because tests (tft, tft_fast,
            // gnss_tft) may have released PIO GPIO or changed backlight state.
            menu_tft_stop();
            ctx->state = MS_MENU;
            ctx->tft_active = false;
            ctx->lcd_dirty = true;

            // Brief pause to avoid spurious key re-trigger
            sleep_ms(200);
        }
        break;
    }

    case KEY_BACK:
    case KEY_LEFT:
        if (ctx->parent) {
            // Find which root item pointed to the current page, to restore cursor
            const menu_page_t *p = ctx->parent;
            uint8_t prev_cursor = 0;
            for (uint8_t i = 0; i < p->count; i++) {
                if (p->items[i].sub == ctx->page) { prev_cursor = i; break; }
            }
            ctx->page = p;
            ctx->parent = (p == &root_page) ? NULL : NULL;  // only 2 levels
            ctx->cursor = prev_cursor;
            ctx->scroll_top = 0;
            ctx->lcd_dirty = true;
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Menu initialization
// ---------------------------------------------------------------------------

void menu_init(menu_ctx_t *ctx) {
    // Init keyboard GPIO (stays active for the entire session)
    key_gpio_init();

    // Init TFT + backlight
    if (menu_tft_init()) {
        ctx->tft_active = true;
    } else {
        ctx->tft_active = false;
        printf("WARNING: TFT init failed; menu display unavailable\n");
    }

    // Set initial menu state
    ctx->state = MS_MENU;
    ctx->page = &root_page;
    ctx->parent = NULL;
    ctx->cursor = 0;
    ctx->scroll_top = 0;
    ctx->serial_last_ms = 0;
    ctx->lcd_dirty = true;
}
