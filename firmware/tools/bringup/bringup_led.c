#include "bringup.h"
#include "bringup_menu.h"

// ---------------------------------------------------------------------------
// LM27965 helpers
// ---------------------------------------------------------------------------

int lm_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(i2c1, LM27965_ADDR, buf, 2, false, 50000);
}

int lm_read(uint8_t reg, uint8_t *val) {
    int r = i2c_write_timeout_us(i2c1, LM27965_ADDR, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c1, LM27965_ADDR, val, 1, false, 50000);
}

// ---------------------------------------------------------------------------
// LED interactive control — per-bank on/off + duty, TFT UI
// GP bit assignments: bit0=ENA(TFT BL) bit1=ENB(Kbd D1B+D2B)
//                     bit2=ENC(D1C red) bit3=EN5A bit4=EN3B(D3B green)
//                     bit5=reserved(keep 1)
// ENB(bit1) must be set for any Bank B output (D1B+D2B always on when ENB=1).
// EN3B(bit4) gates only D3B green; D1B+D2B require ENB=1.
// ---------------------------------------------------------------------------

// Bank descriptor
typedef struct {
    const char *name;       // display label
    uint8_t     duty_reg;   // brightness register
    uint8_t     duty_max;   // max duty value (31 for A/B, 3 for C)
    uint8_t     en_mask;    // GP enable bit(s) — OR'd into GP byte
} led_bank_t;

static const led_bank_t led_banks[] = {
    {"TFT-BL",  LM27965_BANKA, 31, 0x01},          // Bank A: ENA(bit0)
    {"Kbd+Grn", LM27965_BANKB, 31, 0x12},           // Bank B: ENB(bit1)+EN3B(bit4)
    {"Red",     LM27965_BANKC,  3, 0x04},            // Bank C: ENC(bit2)
};
#define LED_BANK_COUNT  3

static void led_draw(int sel, bool on[LED_BANK_COUNT], uint8_t duty[LED_BANK_COUNT]) {
    const int S = 2;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / (6 * S);

    menu_clear(MC_BG);
    menu_str(0, 0, " LED Control        ", COLS, MC_TITLE, MC_TITBG, S);

    for (int i = 0; i < LED_BANK_COUNT; i++) {
        char line[24];
        snprintf(line, sizeof(line), "%c%-7s %3s %2d/%2d ",
                 i == sel ? '>' : ' ',
                 led_banks[i].name,
                 on[i] ? "ON" : "OFF",
                 duty[i],
                 led_banks[i].duty_max);
        menu_str(0, (2 + i) * CH, line, COLS,
                 i == sel ? MC_HITEXT : (on[i] ? MC_OK : MC_FG),
                 i == sel ? MC_HILITE : MC_BG, S);
    }

    menu_str(0, 6 * CH, " UP/DN select       ", COLS, MC_HINT, MC_BG, S);
    menu_str(0, 7 * CH, " LT/RT duty OK=togg ", COLS, MC_HINT, MC_BG, S);
    menu_str(0, 9 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
}

static void led_apply(bool on[LED_BANK_COUNT], uint8_t duty[LED_BANK_COUNT]) {
    // Build GP byte: bit5 always set (reserved), plus enabled banks
    uint8_t gp = 0x20;
    for (int i = 0; i < LED_BANK_COUNT; i++) {
        lm_write(led_banks[i].duty_reg, duty[i]);
        if (on[i]) gp |= led_banks[i].en_mask;
    }
    lm_write(LM27965_GP, gp);
}

void led_control(void) {
    printf("\n--- LED Interactive Control ---\n");

    int sel = 0;
    bool on[LED_BANK_COUNT]   = {false, false, false};
    uint8_t duty[LED_BANK_COUNT] = {16, 16, 1};  // sensible defaults

    led_apply(on, duty);
    led_draw(sel, on, duty);

    // Use menu_scan_key for keypad input (back_key_init already called by menu)
    key_gpio_init();

    bool dirty = false;
    for (;;) {
        menu_key_t k = menu_scan_key();
        if (k == KEY_BACK) break;

        switch (k) {
        case KEY_UP:
            if (sel > 0) { sel--; dirty = true; }
            break;
        case KEY_DOWN:
            if (sel < LED_BANK_COUNT - 1) { sel++; dirty = true; }
            break;
        case KEY_OK:
            on[sel] = !on[sel];
            led_apply(on, duty);
            dirty = true;
            printf("  %s %s  duty=%d\n", led_banks[sel].name, on[sel] ? "ON" : "OFF", duty[sel]);
            break;
        case KEY_RIGHT:
            if (duty[sel] < led_banks[sel].duty_max) {
                duty[sel]++;
                if (on[sel]) led_apply(on, duty);
                dirty = true;
            }
            break;
        case KEY_LEFT:
            if (duty[sel] > 0) {
                duty[sel]--;
                if (on[sel]) led_apply(on, duty);
                dirty = true;
            }
            break;
        default:
            break;
        }

        if (dirty) {
            led_draw(sel, on, duty);
            dirty = false;
        }
        sleep_ms(20);
    }

    // Turn all LEDs off on exit (except TFT backlight which menu reclaims)
    lm_write(LM27965_GP, 0x20);
    printf("LED control done — all off\n");
}

// ---------------------------------------------------------------------------
// LM27965 register dump
// ---------------------------------------------------------------------------

void lm27965_dump_regs(void) {
    printf("\n  [0x36] LM27965 LED Driver\n");
    uint8_t v;
    lm_read(0x10, &v);
    printf("    GP     (0x10): 0x%02X  ENA=%d(TFT) ENB=%d(Kbd+D3Bg) ENC=%d(D1Cr) EN5A=%d EN3B=%d\n",
           v, v&1, (v>>1)&1, (v>>2)&1, (v>>3)&1, (v>>4)&1);
    lm_read(0xA0, &v);
    printf("    BANK_A (0xA0): 0x%02X  code=%d (TFT backlight)\n", v, v&0x1F);
    lm_read(0xB0, &v);
    printf("    BANK_B (0xB0): 0x%02X  code=%d (keyboard BL + D3B green)\n", v, v&0x1F);
    lm_read(0xC0, &v);
    printf("    BANK_C (0xC0): 0x%02X  code=%d (D1C red; 00=20%% 01=40%% 10=70%% 11=100%%)\n",
           v, v&0x3);
}
