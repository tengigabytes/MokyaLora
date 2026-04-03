#pragma once

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
#include "hardware/regs/qmi.h"

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
// SELECT = GND → L channel; data valid on CLK rising edge (tDV ≤ 100 ns)
// CLK frequency: 400 kHz – 3.3 MHz; clkdiv=20 → 3.125 MHz (best SNR)
#define MIC_CLK_PIN    4
#define MIC_DATA_PIN   5
#define PDM_CLK_DIV    20.0f   // 125 MHz / (2 × 20) = 3.125 MHz

// NAU8315 I2S amplifier (GPIO 30/31/32)
// VDD = 3.3 V, BTL 8 Ω speaker: P_max = 3.3² / 16 ≈ 0.68 W (speaker limit 0.7 W)
// AMPLITUDE limited to 50 % → P ≈ 0.17 W (safe margin)
// Sample rate: 125 MHz / 40 / 64 cycles = 48 828 Hz
#define AMP_BCLK_PIN   30
#define AMP_LRCK_PIN   31
#define AMP_DAC_PIN    32
#define I2S_SAMPLE_RATE  48828
#define TONE_FREQ_HZ     444    // 48828 / 110 samples ≈ 444 Hz
#define TONE_TABLE_LEN   110
#define MAX_AMPLITUDE    16384  // 50 % of 32767

// Note frequencies (Hz) — C major, A4 = 440 Hz reference
#define NOTE_C4   262
#define NOTE_D4   294
#define NOTE_E4   330
#define NOTE_F4   349
#define NOTE_G4   392
#define NOTE_REST   0

// LM27965 LED driver (Bus B, 0x36)
// RSET = 8.25 kΩ → full-scale = 30.3 mA/pin
// Bank A (D1A–D5A): TFT backlight, 5 pins → 151.5 mA @ full
// Bank B (D1B, D2B): Keyboard (3 LEDs/pin); D3B (green indicator, 1 LED → limit 40 % = 12.1 mA)
// Bank C (D1C):     Red indicator LED (1 LED → limit 40 % = 12.1 mA)
#define LM27965_ADDR     0x36
#define LM27965_GP       0x10  // ENA[0] ENB[1] ENC[2] EN5A[3] EN3B[4]; default=0x20
#define LM27965_BANKA    0xA0  // bits[4:0]: brightness code Bank A (TFT backlight)
#define LM27965_BANKB    0xB0  // bits[4:0]: brightness code Bank B (Keyboard + D3B)
#define LM27965_BANKC    0xC0  // bits[1:0]: brightness code Bank C (D1C red indicator)
// Brightness codes (Bank A/B): 0x00-0x0F=20%(6mA), 0x10-0x16=40%(12mA),
//                              0x17-0x1C=70%(21mA), 0x1D-0x1F=100%(30mA)
// Brightness codes (Bank C):   bits[1:0] 00=20% 01=40% 10=70% 11=100%

// Keypad matrix (6×6, GPIO 36–47)
// Diode: Anode=COL(GPIO36-41), Cathode=ROW(GPIO42-47) → current flows COL→ROW.
// Rows    GPIO 42–47: output, driven LOW to select row.
// Columns GPIO 36–41: input pull-up, read LOW when key pressed (Vf ~0.3V < VIL 0.54V).
#define KEY_COL_BASE  36
#define KEY_ROW_BASE  42
#define KEY_COLS       6
#define KEY_ROWS       6

// BQ25622 charger (Bus B, 0x6B)
#define BQ25622_ADDR  0x6B

// BQ25622 register map (SLUSEG2D)
#define REG_CHARGE_CTRL0    0x14
#define REG_CHARGER_CTRL1   0x16  // EN_AUTO_IBATDIS[7] FORCE_IBATDIS[6] EN_CHG[5] EN_HIZ[4] WD_RST[2] WATCHDOG[1:0]
#define REG_CHARGER_CTRL2   0x17
#define REG_CHARGER_CTRL3   0x18  // EN_OTG[6] BATFET_DLY[2] BATFET_CTRL[1:0]
#define REG_CHARGER_CTRL4   0x19
#define REG_NTC_CTRL0       0x1A  // TS_IGNORE[7]
#define REG_STATUS0         0x1D  // VSYS_STAT[4] WD_STAT[0]
#define REG_STATUS1         0x1E  // CHG_STAT[4:3] VBUS_STAT[2:0]
#define REG_FAULT_STATUS    0x1F  // BAT_FAULT_STAT[6] TS_STAT[2:0]
#define REG_CHG_FLAG0       0x20
#define REG_CHG_FLAG1       0x21
#define REG_FAULT_FLAG0     0x22
#define REG_PART_INFO       0x38  // PN[5:3] DEV_REV[2:0]  0x02 = BQ25622

// PSRAM (APS6404L, 8 MB, CS = GPIO 0 / QMI CS1n)
// Physical map: CS0=flash 0x000000-0xFFFFFF, CS1=PSRAM 0x800000-0xFFFFFF (M1)
// Uncached XIP alias: XIP_NOCACHE_NOALLOC_BASE + 0x800000
#define PSRAM_CS_PIN       0
#define PSRAM_NOCACHE      (XIP_NOCACHE_NOALLOC_BASE + 0x800000u)
#define PSRAM_TEST_WORDS   1024    // 4 KB pattern test

// SX1262 / LoRa (SPI1, GPIO 23–29)
#define LORA_nRST_PIN  23
#define LORA_MISO_PIN  24
#define LORA_nCS_PIN   25
#define LORA_SCK_PIN   26
#define LORA_MOSI_PIN  27
#define LORA_BUSY_PIN  28
#define LORA_DIO1_PIN  29

// SX1262 opcode constants (DS.SX1261-2 §13)
#define SX1262_OP_GET_STATUS            0xC0u
#define SX1262_OP_READ_REGISTER         0x1Du
#define SX1262_REG_SYNC_WORD_MSB        0x0740u  // default 0x14 (private network)
#define SX1262_REG_SYNC_WORD_LSB        0x0741u  // default 0x24
#define SX1262_OP_SET_STANDBY           0x80u
#define SX1262_OP_SET_PACKET_TYPE       0x8Au
#define SX1262_OP_SET_RF_FREQUENCY      0x86u
#define SX1262_OP_CALIBRATE             0x89u   // CalibParam bitmask (bits 0-6)
#define SX1262_OP_CALIBRATE_IMAGE       0x98u   // 2-byte freq band pair
#define SX1262_OP_SET_MODULATION_PARAMS 0x8Bu
#define SX1262_OP_SET_PACKET_PARAMS     0x8Cu
#define SX1262_OP_SET_BUFFER_BASE_ADDR  0x8Fu
#define SX1262_OP_SET_DIO2_RF_SWITCH    0x9Du
#define SX1262_OP_SET_RX                0x82u
#define SX1262_OP_GET_IRQ_STATUS        0x12u
#define SX1262_OP_CLEAR_IRQ_STATUS      0x02u
#define SX1262_OP_GET_RX_BUFFER_STATUS  0x13u
#define SX1262_OP_READ_BUFFER           0x1Eu

// ---------------------------------------------------------------------------
// Public function declarations (defined in the respective bringup_*.c files)
// ---------------------------------------------------------------------------

// bringup_power.c — Bus B, BQ25622, LM27965, motor
int  lm_write(uint8_t reg, uint8_t val);
void bus_b_init(void);
void bus_b_deinit(void);
void bq25622_print_status(void);
void bq25622_enable_charge(void);
void bq25622_disable_charge(void);
void lm27965_cycle(void);
void motor_breathe(void);
void dump_bus_b(void);

// bringup_sensors.c — Bus A: IMU, Mag, Baro, GNSS, scan
void baro_read(void);
void mag_read(void);
void imu_read(void);
void gnss_info(void);
void dump_bus_a(void);
void perform_scan(i2c_inst_t *i2c, uint sda, uint scl, const char *bus_name);

// bringup_audio.c
void precompute_sine(void);
void amp_test(void);
void amp_breathe(void);
void amp_bee(void);
void mic_test(void);
void mic_loopback(void);

// bringup_flash.c
void flash_test(void);
void psram_test(void);

// bringup_lora.c
void lora_test(void);
void lora_rx(uint32_t freq_hz, uint8_t sf, uint8_t bw_code,
             uint8_t cr_code, uint32_t timeout_s);
void lora_dump(void);
