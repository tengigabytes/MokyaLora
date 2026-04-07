// bringup_tft_draw.c
// ST7789VI TFT drawing primitives for MokyaLora bringup firmware.
//
// Provides PIO1-based 8080 interface, 5x8 bitmap font, drawing primitives,
// and screen rotation.  Used by bringup_menu.c, bringup_memory_tft.c,
// bringup_gnss_tft.c, and other diagnostic modules.

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

// Screen rotation state (0=0deg, 1=90deg, 2=180deg, 3=270deg)
static uint8_t m_rotation = 1;          // default: landscape 90deg
static int m_width = 320, m_height = 240;
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

bool menu_tft_active(void) {
    return m_pio_active;
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
    menu_tft_cmd(0x36); menu_tft_dat(0x60);    // MADCTL: landscape 90deg, RGB
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
    menu_tft_cmd(0x2A);  // CASET: 0..319 (landscape)
    menu_tft_dat(0x00); menu_tft_dat(0x00); menu_tft_dat(0x01); menu_tft_dat(0x3F);
    menu_tft_cmd(0x2B);  // RASET: 0..239 (landscape)
    menu_tft_dat(0x00); menu_tft_dat(0x00); menu_tft_dat(0x00); menu_tft_dat(0xEF);
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

    // Set default rotation to landscape 90deg
    m_rotation = 1; m_width = 320; m_height = 240;

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

    // Restore current rotation (hw_init sets landscape 90deg by default)
    if (m_rotation != 1) apply_rotation();

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
//   0deg (portrait):         0x00  (240x320)
//   90deg (landscape):       0x60  MV=1 MX=1  (320x240)
//   180deg (portrait inv):   0xC0  MX=1 MY=1  (240x320)
//   270deg (landscape inv):  0xA0  MV=1 MY=1  (320x240)
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
