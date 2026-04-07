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
// SELECT = VDD → R channel (DATA2); data valid on CLK falling edge (tDV ≤ 100 ns)
// CLK frequency: 400 kHz – 3.3 MHz; clkdiv=20 → 3.125 MHz (3.072 MHz mode, SNR 69 dB)
// Low-SNR fallback: clkdiv=80 → 781.25 kHz (768 kHz mode, SNR 64 dB)
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

// BQ27441-G1 fuel gauge (Bus B, 0x55)
// Reference: SLUSBH1C datasheet + SLUUAC9A Technical Reference Manual
#define BQ27441_ADDR         0x55

// Standard command registers — 16-bit little-endian (addr = LSB, addr+1 = MSB)
#define BQ27441_REG_CTRL     0x00  // Control() — subcommand interface
#define BQ27441_REG_TEMP     0x02  // Temperature() — 0.1 K; °C = (raw*0.1) − 273.15
#define BQ27441_REG_VOLT     0x04  // Voltage() — mV
#define BQ27441_REG_FLAGS    0x06  // Flags() — status bits
#define BQ27441_REG_REMCAP   0x0C  // RemainingCapacity() — mAh (temperature-compensated)
#define BQ27441_REG_FULLCAP  0x0E  // FullChargeCapacity() — mAh (temperature-compensated)
#define BQ27441_REG_AVGCUR   0x10  // AverageCurrent() — signed mA (+charge, −discharge)
#define BQ27441_REG_SOC      0x1C  // StateOfCharge() — %
#define BQ27441_REG_SOH      0x20  // StateOfHealth() — hi byte=%, lo byte=status(0-6)

// CONTROL() sub-commands (write 3 bytes to 0x00: [0x00, subcmd_lo, subcmd_hi])
// Read result: write 0x00 pointer, read 2 bytes; wait ≥66 µs between write and read
#define BQ27441_CTRL_STATUS  0x0000  // Returns CONTROL_STATUS word
#define BQ27441_CTRL_DEVTYPE 0x0001  // Returns 0x0421 for BQ27441-G1
#define BQ27441_CTRL_CLR_HIB 0x0012  // CLEAR_HIBERNATE — force exit from HIBERNATE

// Flags() bit positions (16-bit little-endian)
// Low byte (0x06): [7]=OCVTAKEN [5]=ITPOR [4]=CFGUPMODE [3]=BAT_DET [2]=SOC1 [1]=SOCF [0]=DSG
// High byte (0x07): [1]=FC [0]=CHG
#define BQ27441_FLAG_DSG        (1u << 0)   // Discharging / relaxation mode
#define BQ27441_FLAG_SOCF       (1u << 1)   // SOC final threshold (default 2%)
#define BQ27441_FLAG_SOC1       (1u << 2)   // SOC low threshold  (default 10%)
#define BQ27441_FLAG_BAT_DET    (1u << 3)   // Battery detected
#define BQ27441_FLAG_ITPOR      (1u << 5)   // RAM reset to ROM defaults; host must reconfigure
#define BQ27441_FLAG_OCVTAKEN   (1u << 7)   // OCV measurement taken in relax mode
#define BQ27441_FLAG_CHG        (1u << 8)   // Fast charge allowed
#define BQ27441_FLAG_FC         (1u << 9)   // Fully charged

// CONTROL_STATUS bit positions (16-bit little-endian)
// Low byte (0x00):  [7]=INITCOMP [6]=HIBERNATE [4]=SLEEP
// High byte (0x01): [5]=SS (sealed)
#define BQ27441_CS_SLEEP        (1u << 4)
#define BQ27441_CS_HIBERNATE    (1u << 6)
#define BQ27441_CS_INITCOMP     (1u << 7)
#define BQ27441_CS_SEALED       (1u << 13)

// BQ25622 charger (Bus B, 0x6B)
#define BQ25622_ADDR  0x6B

// BQ25622 register map (SLUSEG2D)
// Charge/voltage/current limit registers: 16-bit little-endian register pairs.
// Each logical register Xh occupies two I2C byte addresses: lo=Xh, hi=(X+1)h.
// ICHG  field: 80 mA/step, range 80–3520 mA (0x01–0x2C), POR 1040 mA (0x0D)
// VREG  field: 10 mV/step, range 3500–4800 mV (0x15E–0x1E0), POR 4200 mV (0x1A4)
// IINDPM field: 20 mA/step, range 100–3200 mA (0x05–0xA0),  POR 3200 mA (0xA0)
#define REG_ICHG_LO     0x02  // ICHG[1:0] in bits[7:6]; bits[5:0] reserved
#define REG_ICHG_HI     0x03  // ICHG[5:2] in bits[3:0]; bits[7:4] reserved
#define REG_VREG_LO     0x04  // VREG[4:0] in bits[7:3]; bits[2:0] reserved
#define REG_VREG_HI     0x05  // VREG[8:5] in bits[3:0]; bits[7:4] reserved
#define REG_IINDPM_LO   0x06  // IINDPM[3:0] in bits[7:4]; bits[3:0] reserved
#define REG_IINDPM_HI   0x07  // IINDPM[7:4] in bits[3:0]; bits[7:4] reserved
// BQ25622 ADC control (SLUSEG2D §8.6.2.28–29) — single-byte registers
// REG0x26: ADC_EN[7] ADC_RATE[6] ADC_SAMPLE[5:4] ADC_AVG[3] ADC_AVG_INIT[2]
//   0x80 = ADC_EN=1, continuous, 12-bit (ADC_SAMPLE=00 = 12-bit)
// REG0x27: per-channel disable bits (POR=0x00, all channels enabled)
#define REG_ADC_CTRL     0x26
#define REG_ADC_FUNC_DIS 0x27

// BQ25622 ADC registers (SLUSEG2D §8.6.2.30–35)
// 16-bit little-endian; raw16 = (hi<<8)|lo; decode per field:
//   IBUS/IBAT : 2s-complement → (int16_t)raw16 >> 1 (*2mA) / >>2 (*4mA)
//   VBUS/VPMID: unsigned bits[14:2] → (raw16>>2)&0x1FFF (*3.97mV)
//   VBAT/VSYS : unsigned bits[12:1] → (raw16>>1)&0x0FFF (*1.99mV)
#define REG_IBUS_ADC_LO  0x28
#define REG_IBUS_ADC_HI  0x29
#define REG_IBAT_ADC_LO  0x2A
#define REG_IBAT_ADC_HI  0x2B
#define REG_VBUS_ADC_LO  0x2C
#define REG_VBUS_ADC_HI  0x2D
#define REG_VPMID_ADC_LO 0x2E
#define REG_VPMID_ADC_HI 0x2F
#define REG_VBAT_ADC_LO  0x30
#define REG_VBAT_ADC_HI  0x31
#define REG_VSYS_ADC_LO  0x32
#define REG_VSYS_ADC_HI  0x33
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
// XIP virtual address bit 24 selects M0 (CS0/Flash) or M1 (CS1/PSRAM):
//   M0: 0x0000000-0x0FFFFFF (Flash, 16 MB)
//   M1: 0x1000000-0x1FFFFFF (PSRAM, 16 MB window, 8 MB populated)
// Uncached XIP alias: XIP_NOCACHE_NOALLOC_BASE + 0x1000000
#define PSRAM_CS_PIN       0
#define PSRAM_NOCACHE      (XIP_NOCACHE_NOALLOC_BASE + 0x1000000u)
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
#define SX1262_OP_GET_RSSI_INST         0x15u   // GetRssiInst: NOP + RSSI byte (-value/2 dBm)

// ---------------------------------------------------------------------------
// Public function declarations (defined in the respective bringup_*.c files)
// ---------------------------------------------------------------------------

// bringup_power.c — Bus B, BQ25622, BQ27441, LM27965, motor
int  lm_write(uint8_t reg, uint8_t val);
void bus_b_init(void);
void bus_b_deinit(void);
void bq25622_print_status(void);
void bq25622_enable_charge(void);
void bq25622_disable_charge(void);
void bq25622_read_adc(void);
void bq27441_read(void);
void lm27965_cycle(void);
void motor_breathe(void);
void dump_bus_b(void);

// bringup_sensors.c — Bus A: IMU, Mag, Baro, GNSS, scan
void baro_read(void);
void mag_read(void);
void imu_read(void);
void gnss_info(void);
void scan_bus_a(void);
void perform_scan(i2c_inst_t *i2c, uint sda, uint scl, const char *bus_name);

// Set to 1 to compile embedded WAV files (~12 MB flash, slow build).
// Also uncomment the WAV source files in firmware/tools/bringup/CMakeLists.txt.
#define BRINGUP_WAV  0

// Set to 1 to enable ~0.5 s periodic stat prints in mic_loopback.
// Disable (0) when testing audio quality — printf stalls the CPU long enough
// to freeze the PDM CLK, causing the mic to lose lock.
#define MIC_LOOP_STATS  0

// bringup_audio.c
void precompute_sine(void);
void amp_test(void);
void amp_breathe(void);
void amp_bee(void);
#if BRINGUP_WAV
void amp_voice(void);
void amp_gc(void);
void amp_test01_8k(void);
void amp_test01_16k(void);
void amp_test01_44k(void);
void amp_test01_48k(void);
void amp_test01_all(void);
#endif
void mic_test(void);
void mic_raw(void);
void mic_loopback(void);
void mic_rec(void);
void mic_dump(void);

// bringup_flash.c
void sram_test(void);
void flash_test(void);
bool psram_init(void);      // call once at boot — returns true if APS6404L found
void psram_test(void);
void psram_full_test(void); // full 8 MB two-pass write+verify test
void psram_speed_test(void);// sweep CLKDIV×RXDELAY, find max PSRAM QPI speed
void psram_diag_test(void); // CLKDIV=1 error pattern analysis & timing tuning
void flash_speed_test(void);// sweep M0 CLKDIV×RXDELAY from RAM, test Flash reads
void psram_jlink_prep(void);// write sentinel, print J-Link mem32 command
void psram_diag(void);
void psram_probe(void);

// bringup_lora.c
void lora_test(void);
void lora_rx(uint32_t freq_hz, uint8_t sf, uint8_t bw_code,
             uint8_t cr_code, uint32_t timeout_s);
void lora_dump(void);

// bringup_tft.c — ST7789VI 2.4" IPS, 8080 8-bit parallel (pio1)
// GPIO 10 nCS, 11 DCX, 12 nWR, 13-20 D[7:0], 21 nRST, 22 TE
#define TFT_nCS_PIN   10
#define TFT_DCX_PIN   11
#define TFT_nWR_PIN   12
#define TFT_D0_PIN    13
#define TFT_nRST_PIN  21
#define TFT_TE_PIN    22
void tft_test(void);
void tft_fast_test(void);

// bringup_gnss_tft.c — Step 14: Teseo-LIV3FL outdoor GNSS + live TFT display
// Streams NMEA over I2C Bus A; renders GPS fix, lat/lon/alt/UTC, TTFF, and
// raw NMEA sentences on the ST7789VI using a built-in 5x8 bitmap font.
void gnss_tft_test(void);

// bringup_core1.c — Step 16 Stage A: bare-metal Core 1 validation
// Tests: boot, FIFO echo, shared SRAM spinlock, GPIO from Core 1
void core1_test(void);

// i2c_custom_scan.c — bringup REPL entry points
// bringup_repl_init() MUST be called from Core 0 (registers USB CDC interrupt).
// bringup_repl_loop() may then be called from Core 0 or Core 1.
// bringup_repl_run() is a convenience wrapper that calls both sequentially.
void bringup_repl_init(void);
void bringup_repl_loop(void);
void bringup_repl_run(void);

// i2c_custom_scan.c — command dispatch (called by menu and serial REPL)
void handle_command(const char *cmd);

// i2c_custom_scan.c — keypad matrix (6x6 GPIO polling)
void key_gpio_init(void);
void key_gpio_deinit(void);
void key_scan_matrix(uint8_t pressed[KEY_ROWS]);
void key_monitor(void);

// bringup_gnss_tft.c — BACK key helpers (shared for menu test interruption)
void back_key_init(void);
void back_key_deinit(void);
bool back_key_pressed(void);
