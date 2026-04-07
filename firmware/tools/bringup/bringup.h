#pragma once

// ---------------------------------------------------------------------------
// Umbrella header — includes all sub-module headers
// ---------------------------------------------------------------------------
#include "bringup_pins.h"
#include "bringup_power.h"      // pulls in lm27965, bq25622, bq27441
#include "bringup_sx1262.h"

// ---------------------------------------------------------------------------
// Feature flags
// ---------------------------------------------------------------------------

// Set to 1 to compile embedded WAV files (~12 MB flash, slow build).
// Also uncomment the WAV source files in firmware/tools/bringup/CMakeLists.txt.
#define BRINGUP_WAV  0

// Set to 1 to enable ~0.5 s periodic stat prints in mic_loopback.
// Disable (0) when testing audio quality — printf stalls the CPU long enough
// to freeze the PDM CLK, causing the mic to lose lock.
#define MIC_LOOP_STATS  0

// ---------------------------------------------------------------------------
// Public function declarations (defined in the respective bringup_*.c files)
// ---------------------------------------------------------------------------

// bringup_sensors.c — Bus A: IMU, Mag, Baro, GNSS, scan
void baro_read(void);
void mag_read(void);
void imu_read(void);
void gnss_info(void);
void scan_bus_a(void);
void perform_scan(i2c_inst_t *i2c, uint sda, uint scl, const char *bus_name);

// bringup_amp.c — NAU8315 I2S amplifier
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

// bringup_mic.c — IM69D130 PDM microphone
void mic_test(void);
void mic_raw(void);
void mic_loopback(void);
void mic_rec(void);
void mic_dump(void);

// bringup_sram.c — internal SRAM test
void sram_test(void);

// bringup_flash.c — Flash test + speed sweep
void flash_test(void);
void flash_speed_test(void);// sweep M0 CLKDIV*RXDELAY from RAM, test Flash reads
struct flash_speed_result {
    uint8_t  clkdiv;
    uint8_t  rxdelay;
    uint32_t sck_mhz;
    uint32_t read_val;
    uint32_t expected;
    bool     pass;
};
void flash_speed_run(struct flash_speed_result *results, int count,
                     const uint8_t *clkdivs, const uint8_t *rxdelays,
                     uint32_t sys_hz);

// bringup_psram.c — PSRAM init, test, speed sweep
bool psram_init(void);      // call once at boot — returns true if APS6404L found
void psram_test(void);
void psram_full_test(void); // full 8 MB two-pass write+verify test
void psram_speed_test(void);// sweep CLKDIV*RXDELAY, find max PSRAM QPI speed
void psram_diag_test(void); // CLKDIV=1 error pattern analysis & timing tuning
void psram_jlink_prep(void);// write sentinel, print J-Link mem32 command
void psram_diag(void);
void psram_probe(void);
void psram_set_timing(uint8_t clkdiv, uint8_t rxdelay);
uint32_t psram_sweep_pass(void);
uint32_t psram_verify_pass(void);
void psram_set_full_timing(uint32_t timing);

// bringup_memory_tft.c — TFT consolidated diagnostics (Step 20)
void cmd_memory_diag(void);   // SRAM+Flash+PSRAM one-screen diagnostic
void cmd_psram_full_tft(void);// 8 MB test with TFT progress
void cmd_psram_tuning(void);  // merged sweep+diag+flash_sweep with TFT
void cmd_psram_debug(void);   // merged probe+jlink with TFT
void cmd_psram_dma_test(void);// DMA vs CPU throughput comparison
void psram_rd_diag(void);    // read vs write speed diagnostic (serial)

// bringup_tft.c — ST7789VI 2.4" IPS, 8080 8-bit parallel (pio1)
void tft_test(void);
void tft_fast_test(void);

// bringup_gnss_tft.c — Step 14: Teseo-LIV3FL outdoor GNSS + live TFT display
void gnss_tft_test(void);

// bringup_core1.c — Step 16 Stage A: bare-metal Core 1 validation
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
void key_tft_test(void);

// bringup_gnss_tft.c — BACK key helpers (shared for menu test interruption)
void back_key_init(void);
void back_key_deinit(void);
bool back_key_pressed(void);
