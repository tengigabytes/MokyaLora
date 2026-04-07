#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// LM27965 LED driver (Bus B, 0x36)
// ---------------------------------------------------------------------------
// RSET = 8.25 kohm -> full-scale = 30.3 mA/pin
// Bank A (D1A-D5A): TFT backlight, 5 pins -> 151.5 mA @ full
// Bank B (D1B, D2B): Keyboard (3 LEDs/pin); D3B (green indicator, 1 LED -> limit 40 % = 12.1 mA)
// Bank C (D1C):     Red indicator LED (1 LED -> limit 40 % = 12.1 mA)
#define LM27965_ADDR     0x36
#define LM27965_GP       0x10  // ENA[0] ENB[1] ENC[2] EN5A[3] EN3B[4]; default=0x20
#define LM27965_BANKA    0xA0  // bits[4:0]: brightness code Bank A (TFT backlight)
#define LM27965_BANKB    0xB0  // bits[4:0]: brightness code Bank B (Keyboard + D3B)
#define LM27965_BANKC    0xC0  // bits[1:0]: brightness code Bank C (D1C red indicator)
// Brightness codes (Bank A/B): 0x00-0x0F=20%(6mA), 0x10-0x16=40%(12mA),
//                              0x17-0x1C=70%(21mA), 0x1D-0x1F=100%(30mA)
// Brightness codes (Bank C):   bits[1:0] 00=20% 01=40% 10=70% 11=100%

// Public API
int  lm_write(uint8_t reg, uint8_t val);
int  lm_read(uint8_t reg, uint8_t *val);
void lm27965_dump_regs(void);
void led_control(void);
