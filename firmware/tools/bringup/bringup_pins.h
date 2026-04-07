#pragma once

// ---------------------------------------------------------------------------
// SDK includes (shared by all bringup modules)
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip.h"
#include "hardware/regs/qmi.h"
#include "hardware/dma.h"

// ---------------------------------------------------------------------------
// Pin / peripheral definitions
// ---------------------------------------------------------------------------

// Both sensor bus (GPIO 34/35) and power bus (GPIO 6/7) map to i2c1 on RP2350.
// They cannot be used simultaneously — use bus_b_init / bus_b_deinit to switch.
#define BUS_A_SDA 34    // Sensor bus  (i2c1 — SDK peripheral name)
#define BUS_A_SCL 35
#define BUS_B_SDA 6     // Power bus   (i2c1 — same peripheral, different GPIOs)
#define BUS_B_SCL 7
#define MTR_PWM_PIN 9   // PWM4_B — drives low-side MOSFET for vibration motor

// IM69D130 PDM microphone (GPIO 4/5)
// SELECT = VDD -> R channel (DATA2); data valid on CLK falling edge (tDV <= 100 ns)
// CLK frequency: 400 kHz - 3.3 MHz; clkdiv=20 -> 3.125 MHz (3.072 MHz mode, SNR 69 dB)
// Low-SNR fallback: clkdiv=80 -> 781.25 kHz (768 kHz mode, SNR 64 dB)
#define MIC_CLK_PIN    4
#define MIC_DATA_PIN   5
#define PDM_CLK_DIV    20.0f   // 125 MHz / (2 * 20) = 3.125 MHz

// NAU8315 I2S amplifier (GPIO 30/31/32)
// VDD = 3.3 V, BTL 8 ohm speaker: P_max = 3.3^2 / 16 = 0.68 W (speaker limit 0.7 W)
// AMPLITUDE limited to 50 % -> P = 0.17 W (safe margin)
// Sample rate: 125 MHz / 40 / 64 cycles = 48 828 Hz
#define AMP_BCLK_PIN   30
#define AMP_LRCK_PIN   31
#define AMP_DAC_PIN    32
#define I2S_SAMPLE_RATE  48828
#define TONE_FREQ_HZ     444    // 48828 / 110 samples = 444 Hz
#define TONE_TABLE_LEN   110
#define MAX_AMPLITUDE    16384  // 50 % of 32767

// Note frequencies (Hz) — C major, A4 = 440 Hz reference
#define NOTE_C4   262
#define NOTE_D4   294
#define NOTE_E4   330
#define NOTE_F4   349
#define NOTE_G4   392
#define NOTE_REST   0

// Keypad matrix (6x6, GPIO 36-47)
// Diode: Anode=COL(GPIO36-41), Cathode=ROW(GPIO42-47) -> current flows COL->ROW.
// Rows    GPIO 42-47: output, driven LOW to select row.
// Columns GPIO 36-41: input pull-up, read LOW when key pressed (Vf ~0.3V < VIL 0.54V).
#define KEY_COL_BASE  36
#define KEY_ROW_BASE  42
#define KEY_COLS       6
#define KEY_ROWS       6

// PSRAM (APS6404L, 8 MB, CS = GPIO 0 / QMI CS1n)
// XIP virtual address bit 24 selects M0 (CS0/Flash) or M1 (CS1/PSRAM):
//   M0: 0x0000000-0x0FFFFFF (Flash, 16 MB)
//   M1: 0x1000000-0x1FFFFFF (PSRAM, 16 MB window, 8 MB populated)
// Uncached XIP alias: XIP_NOCACHE_NOALLOC_BASE + 0x1000000
#define PSRAM_CS_PIN       0
#define PSRAM_NOCACHE      (XIP_NOCACHE_NOALLOC_BASE + 0x1000000u)
#define PSRAM_CACHED       (XIP_BASE + 0x1000000u)
#define PSRAM_TEST_WORDS   1024    // 4 KB pattern test
#define PSRAM_SIZE_MB      8u
#define PSRAM_FULL_WORDS   (PSRAM_SIZE_MB * 1024u * 1024u / 4u)  // 2,097,152
#define PSRAM_STEP_WORDS   (1024u * 1024u / 4u)                   // 1 MB in words
#define SWEEP_WORDS        65536u   // 256 KB per pass

// SX1262 / LoRa (SPI1, GPIO 23-29)
#define LORA_nRST_PIN  23
#define LORA_MISO_PIN  24
#define LORA_nCS_PIN   25
#define LORA_SCK_PIN   26
#define LORA_MOSI_PIN  27
#define LORA_BUSY_PIN  28
#define LORA_DIO1_PIN  29

// ST7789VI 2.4" IPS, 8080 8-bit parallel (pio1)
// GPIO 10 nCS, 11 DCX, 12 nWR, 13-20 D[7:0], 21 nRST, 22 TE
#define TFT_nCS_PIN   10
#define TFT_DCX_PIN   11
#define TFT_nWR_PIN   12
#define TFT_D0_PIN    13
#define TFT_nRST_PIN  21
#define TFT_TE_PIN    22
