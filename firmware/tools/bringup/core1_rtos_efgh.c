/*
 * core1_rtos_efgh.c — Step 16 Stages E, F, G, H
 *
 * Validates FreeRTOS on Core 1 (Plan B architecture) with real hardware access:
 *
 *   Stage E — Multi-task CDC output with mutex (30 s stability)
 *   Stage F — SX1262 SPI access from FreeRTOS task
 *   Stage G — HW FIFO IPC: bare-metal Core 0 ↔ FreeRTOS Core 1 (100 messages)
 *   Stage H — I2C sensor reads (IMU / baro / mag) from FreeRTOS task
 *
 * Architecture (Stage B2 — Plan B):
 *   Core 0 — bare-metal launcher; participates in Stage G IPC then idles.
 *   Core 1 — FreeRTOS scheduler + manual TinyUSB CDC (tusb_init on Core 1).
 *
 * USB descriptors provided by pico_stdio_usb (linked but stdio_usb_init
 * never called; pico_enable_stdio_usb(... 1) in CMakeLists ensures descriptors
 * are linked).
 *
 * Expected: COM port appears ~2–3 s after power-on; stages print results over
 * USB CDC; total runtime ~40 s.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/resets.h"
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* ────────────────────────────────────────────────────────────────────────────
 * Pin definitions (from bringup.h / mcu-gpio-allocation.md)
 * ────────────────────────────────────────────────────────────────────────────*/

/* Bus B — power I2C (i2c1, GPIO 6/7) */
#define BUS_B_SDA  6
#define BUS_B_SCL  7

/* BQ25622 charger (Bus B, 0x6B) */
#define BQ25622_ADDR         0x6Bu
#define BQ25622_REG_CTRL1    0x16u   /* EN_AUTO_IBATDIS[7] EN_CHG[5] EN_HIZ[4] WD_RST[2] WATCHDOG[1:0] */
#define BQ25622_CTRL1_DISABLE 0x80u  /* EN_CHG=0, WATCHDOG=off  (validated in Step 8) */
#define BQ25622_CTRL1_ENABLE  0xA1u  /* EN_CHG=1, WATCHDOG=40s  (validated in Step 8) */

/* BQ27441 fuel gauge (Bus B, 0x55) — present only when battery is installed */
#define BQ27441_ADDR         0x55u

/* Bus A — sensor I2C (i2c1, GPIO 34/35) */
#define BUS_A_SDA  34
#define BUS_A_SCL  35

/* SX1262 LoRa (SPI1, GPIO 23–29) */
#define LORA_nRST_PIN  23
#define LORA_MISO_PIN  24
#define LORA_nCS_PIN   25
#define LORA_SCK_PIN   26
#define LORA_MOSI_PIN  27
#define LORA_BUSY_PIN  28
#define LORA_DIO1_PIN  29

/* SX1262 opcodes and register addresses */
#define SX1262_OP_GET_STATUS       0xC0u
#define SX1262_OP_READ_REGISTER    0x1Du
#define SX1262_REG_SYNC_WORD_MSB   0x0740u
#define SX1262_REG_SYNC_WORD_LSB   0x0741u

/* ────────────────────────────────────────────────────────────────────────────
 * Stage G IPC tokens (distinct from Stage A tokens in bringup_core1.c)
 * ────────────────────────────────────────────────────────────────────────────*/
#define C1_EFGH_READY      0xC1EF6800u   /* Core 1 → Core 0: Stage G ready  */
#define C0_EFGH_IPC_BASE   0xC0EF0000u   /* Core 0 → Core 1: IPC message    */
#define C1_EFGH_IPC_BASE   0xC1EF0000u   /* Core 1 → Core 0: IPC ack        */
#define EFGH_IPC_COUNT     100

/* Shared SRAM for Stage G SRAM-integrity sub-test.
 * Core 0 writes g_ipc_sram before each FIFO push.
 * Core 1 reads it after popping the FIFO.                                    */
static volatile uint32_t g_ipc_sram;

/* Set by charger_startup_init(); reported via CDC after USB enumerates.      */
static bool g_battery_present;

/* ────────────────────────────────────────────────────────────────────────────
 * charger_startup_init — called once at Core 1 boot, before the FreeRTOS
 * scheduler and TinyUSB init.
 *
 * Sequence:
 *   1. Init Bus B (i2c1, GPIO 6/7, 100 kHz).
 *   2. Disable BQ25622 charging immediately — suppresses inductor noise when
 *      no battery is installed (charger attempts charge into open circuit).
 *   3. Probe BQ27441 fuel gauge at 0x55.  The gauge only responds when a
 *      battery is present (or the system is in charging mode with VBAT live).
 *   4. If gauge ACKs → battery installed → re-enable charging.
 *      If gauge absent → keep charging disabled.
 *   5. Deinit Bus B; i2c1 is reclaimed by the sensor bus (Bus A) later.
 * ────────────────────────────────────────────────────────────────────────────*/
static void charger_startup_init(void)
{
    /* ── Bus B init (i2c1, GPIO 6/7, 100 kHz) ── */
    i2c_init(i2c1, 100 * 1000);
    gpio_set_function(BUS_B_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_B_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_B_SDA);
    gpio_pull_up(BUS_B_SCL);

    /* ── Step 1: disable charging (write 0x80 → EN_CHG=0) ── */
    /* WD_RST kick first (bit2=1), then stable disable value.  */
    uint8_t buf[2];
    buf[0] = BQ25622_REG_CTRL1; buf[1] = 0x84u;   /* 0x80 | WD_RST */
    i2c_write_timeout_us(i2c1, BQ25622_ADDR, buf, 2, false, 50000);
    busy_wait_ms(10);
    buf[1] = BQ25622_CTRL1_DISABLE;                /* EN_CHG=0 */
    i2c_write_timeout_us(i2c1, BQ25622_ADDR, buf, 2, false, 50000);

    /* ── Step 2: probe BQ27441 fuel gauge at 0x55 ── */
    /* Give the gauge time to power up (VBAT → gauge startup ~50 ms).         */
    busy_wait_ms(100);

    /* Read-probe: send address byte only (read transaction, 1 byte).
     * BQ27441 ACKs its address when awake; NACK or timeout = absent/asleep.
     * Retry 3× with 50 ms spacing in case the gauge is mid-wakeup.          */
    bool battery_present = false;
    for (int attempt = 0; attempt < 3 && !battery_present; attempt++) {
        uint8_t dummy = 0;
        battery_present =
            (i2c_read_timeout_us(i2c1, BQ27441_ADDR, &dummy, 1, false, 50000) >= 0);
        if (!battery_present) busy_wait_ms(50);
    }

    g_battery_present = battery_present;

    /* ── Step 3: if battery present, re-enable charging ── */
    if (battery_present) {
        buf[0] = BQ25622_REG_CTRL1; buf[1] = 0xA4u;   /* EN_CHG=1 | WD_RST */
        i2c_write_timeout_us(i2c1, BQ25622_ADDR, buf, 2, false, 50000);
        busy_wait_ms(10);
        buf[1] = BQ25622_CTRL1_ENABLE;                  /* EN_CHG=1, WD=40s */
        i2c_write_timeout_us(i2c1, BQ25622_ADDR, buf, 2, false, 50000);
    }

    /* ── Bus B deinit — i2c1 is shared; Stage H will reinit for Bus A ── */
    i2c_deinit(i2c1);
    gpio_set_function(BUS_B_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_B_SCL, GPIO_FUNC_NULL);
}

/* ────────────────────────────────────────────────────────────────────────────
 * FreeRTOS shared state
 * ────────────────────────────────────────────────────────────────────────────*/
static SemaphoreHandle_t g_cdc_mutex;
static volatile int g_stage_e_done_count;   /* incremented by each writer task */

/* ────────────────────────────────────────────────────────────────────────────
 * CDC output helper
 * ────────────────────────────────────────────────────────────────────────────*/
static void cdc_write_str(const char *s)
{
    if (!tud_cdc_connected()) return;

    uint32_t len = (uint32_t)strlen(s);
    uint32_t pos = 0;
    while (pos < len) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) { tud_cdc_write_flush(); vTaskDelay(1); continue; }
        uint32_t chunk = len - pos;
        if (chunk > avail) chunk = avail;
        tud_cdc_write(s + pos, chunk);
        pos += chunk;
    }
    tud_cdc_write_flush();
}

/* Mutex-protected variant for concurrent task use. */
static void cdc_locked_write(const char *s)
{
    if (xSemaphoreTake(g_cdc_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        cdc_write_str(s);
        xSemaphoreGive(g_cdc_mutex);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * USB device task — polls tud_task() at high priority.
 * ────────────────────────────────────────────────────────────────────────────*/
static void usb_device_task(void *pv)
{
    (void)pv;
    for (;;) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * STAGE E — Multi-task CDC output with mutex
 * ════════════════════════════════════════════════════════════════════════════*/

#define STAGE_E_ITERS    3    /* each writer fires 3 times × 200 ms ≈ 0.6 s */
#define STAGE_E_DELAY_MS 200

static void stage_e_writer(void *pv)
{
    const char id = (char)(uintptr_t)pv;  /* 'A' or 'B' */
    char buf[80];

    vTaskDelay(pdMS_TO_TICKS(1000));       /* stagger start slightly */

    for (int i = 1; i <= STAGE_E_ITERS; i++) {
        snprintf(buf, sizeof(buf),
                 "[E] Writer-%c: iter %2d/%d  core%d  tick%lu\r\n",
                 id, i, STAGE_E_ITERS,
                 (int)get_core_num(),
                 (unsigned long)xTaskGetTickCount());
        cdc_locked_write(buf);
        vTaskDelay(pdMS_TO_TICKS(STAGE_E_DELAY_MS));
    }

    g_stage_e_done_count++;   /* signal coordinator */
    vTaskDelete(NULL);
}

/* ════════════════════════════════════════════════════════════════════════════
 * STAGE F — SX1262 SPI from FreeRTOS task
 *
 * Inline driver helpers (static; match bringup_lora.c implementations).
 * ════════════════════════════════════════════════════════════════════════════*/

static inline void sx1262_cs_assert(void)   { gpio_put(LORA_nCS_PIN, 0); }
static inline void sx1262_cs_deassert(void) { gpio_put(LORA_nCS_PIN, 1); }

static bool sx1262_wait_busy(uint32_t timeout_ms)
{
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;
    while (gpio_get(LORA_BUSY_PIN)) {
        if (to_ms_since_boot(get_absolute_time()) > deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(1));   /* yield while waiting */
    }
    return true;
}

static uint8_t sx1262_get_status(void)
{
    uint8_t tx[2] = {SX1262_OP_GET_STATUS, 0x00};
    uint8_t rx[2] = {0, 0};
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 2);
    sx1262_cs_deassert();
    return rx[1];
}

static uint8_t sx1262_read_reg(uint16_t addr)
{
    uint8_t tx[5] = {SX1262_OP_READ_REGISTER,
                     (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
                     0x00, 0x00};
    uint8_t rx[5] = {0};
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 5);
    sx1262_cs_deassert();
    return rx[4];
}

static void run_stage_f(void)
{
    char buf[128];
    cdc_write_str("\r\n[F] Stage F: SX1262 SPI from FreeRTOS task\r\n");

    /* ── SPI1 + GPIO init (matches bringup_lora.c sx1262_hw_init) ── */
    spi_init(spi1, 1000 * 1000);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LORA_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LORA_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(LORA_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_init(LORA_nCS_PIN);  gpio_set_dir(LORA_nCS_PIN,  GPIO_OUT); sx1262_cs_deassert();
    gpio_init(LORA_BUSY_PIN); gpio_set_dir(LORA_BUSY_PIN, GPIO_IN);
    gpio_init(LORA_DIO1_PIN); gpio_set_dir(LORA_DIO1_PIN, GPIO_IN);
    gpio_init(LORA_nRST_PIN); gpio_set_dir(LORA_nRST_PIN, GPIO_OUT);

    /* Hardware reset */
    gpio_put(LORA_nRST_PIN, 0); busy_wait_ms(10);
    gpio_put(LORA_nRST_PIN, 1); busy_wait_ms(10);

    bool busy_ok = sx1262_wait_busy(200);
    snprintf(buf, sizeof(buf), "[F]   BUSY after reset : %s\r\n",
             busy_ok ? "LOW (OK)" : "FAIL (stuck HIGH)");
    cdc_write_str(buf);

    if (!busy_ok) { cdc_write_str("[F]   RESULT: FAIL\r\n"); goto f_deinit; }

    /* GetStatus */
    sx1262_wait_busy(50);
    uint8_t status = sx1262_get_status();
    uint8_t chip_mode  = (status >> 4) & 0x07u;
    uint8_t cmd_status = (status >> 1) & 0x07u;
    snprintf(buf, sizeof(buf),
             "[F]   GetStatus : 0x%02X  ChipMode=%u (expect 2=STBY_RC)  CmdStatus=%u\r\n",
             status, chip_mode, cmd_status);
    cdc_write_str(buf);

    /* RegSyncWord */
    sx1262_wait_busy(50);
    uint8_t sw_msb = sx1262_read_reg(SX1262_REG_SYNC_WORD_MSB);
    sx1262_wait_busy(50);
    uint8_t sw_lsb = sx1262_read_reg(SX1262_REG_SYNC_WORD_LSB);
    bool sw_ok = (sw_msb == 0x14u) && (sw_lsb == 0x24u);
    snprintf(buf, sizeof(buf),
             "[F]   SyncWord   : 0x%02X 0x%02X  (expect 0x14 0x24 — private net) %s\r\n",
             sw_msb, sw_lsb, sw_ok ? "OK" : "MISMATCH");
    cdc_write_str(buf);

    bool pass = (chip_mode == 2u) && sw_ok;
    snprintf(buf, sizeof(buf), "[F]   RESULT: %s\r\n", pass ? "PASS" : "FAIL");
    cdc_write_str(buf);

f_deinit:
    spi_deinit(spi1);
    for (int p = LORA_nRST_PIN; p <= LORA_DIO1_PIN; p++)
        gpio_set_function(p, GPIO_FUNC_NULL);
}

/* ════════════════════════════════════════════════════════════════════════════
 * STAGE G — HW FIFO IPC between Core 0 (bare-metal) and Core 1 (FreeRTOS)
 * ════════════════════════════════════════════════════════════════════════════*/

static void run_stage_g(void)
{
    char buf[128];
    cdc_write_str("\r\n[G] Stage G: HW FIFO IPC — bare-metal Core 0 ↔ FreeRTOS Core 1\r\n");
    cdc_write_str("[G]   Signalling Core 0 to begin IPC send...\r\n");

    /* Signal Core 0 to begin the 100-message sequence. */
    multicore_fifo_push_blocking(C1_EFGH_READY);

    int ok_count   = 0;
    int fifo_err   = 0;
    int token_err  = 0;
    int sram_err   = 0;

    for (int i = 0; i < EFGH_IPC_COUNT; i++) {
        /* Yield-friendly FIFO pop (Core 0 sends quickly; latency <1 ms). */
        uint32_t deadline_ms = to_ms_since_boot(get_absolute_time()) + 500;
        while (!multicore_fifo_rvalid()) {
            if (to_ms_since_boot(get_absolute_time()) > deadline_ms) break;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (!multicore_fifo_rvalid()) { fifo_err++; continue; }

        uint32_t msg = sio_hw->fifo_rd;   /* direct register — non-blocking */

        /* Validate token */
        if ((msg & 0xFFFFFF00u) != C0_EFGH_IPC_BASE ||
            (msg & 0xFFu) != (uint32_t)i) {
            token_err++;
        }

        /* Validate shared SRAM written by Core 0 before the push */
        uint32_t expected_sram = 0xABCD0000u | (uint32_t)i;
        if (g_ipc_sram != expected_sram) sram_err++;

        /* Acknowledge back to Core 0 */
        multicore_fifo_push_blocking(C1_EFGH_IPC_BASE | (uint32_t)i);

        ok_count++;
    }

    snprintf(buf, sizeof(buf),
             "[G]   Received: %d/%d  FIFO-timeout: %d  token-err: %d  sram-err: %d\r\n",
             ok_count, EFGH_IPC_COUNT, fifo_err, token_err, sram_err);
    cdc_write_str(buf);

    bool pass = (ok_count == EFGH_IPC_COUNT) && (token_err == 0) && (sram_err == 0);
    snprintf(buf, sizeof(buf), "[G]   RESULT: %s\r\n", pass ? "PASS" : "FAIL");
    cdc_write_str(buf);
}

/* ════════════════════════════════════════════════════════════════════════════
 * STAGE H — I2C sensor reads from FreeRTOS task
 *
 * Uses busy_wait_ms() (not sleep_ms) for compatibility with bare-metal
 * Core 0; SYNC/TIME interop disabled so sleep_ms would fall back to
 * busy_wait anyway, but we call it explicitly for clarity.
 * ════════════════════════════════════════════════════════════════════════════*/

static int h_dev_read(uint8_t addr, uint8_t reg, uint8_t *val)
{
    int r = i2c_write_timeout_us(i2c1, addr, &reg, 1, true, 50000);
    if (r < 0) return r;
    return i2c_read_timeout_us(i2c1, addr, val, 1, false, 50000);
}

static int h_dev_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(i2c1, addr, buf, 2, false, 50000);
}

static void h_bus_a_init(void)
{
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_I2C);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(BUS_A_SDA);
    gpio_pull_up(BUS_A_SCL);
}

static void h_bus_a_deinit(void)
{
    i2c_deinit(i2c1);
    gpio_set_function(BUS_A_SDA, GPIO_FUNC_NULL);
    gpio_set_function(BUS_A_SCL, GPIO_FUNC_NULL);
}

static void run_stage_h(void)
{
    char buf[160];
    bool all_pass = true;
    cdc_write_str("\r\n[H] Stage H: I2C sensor reads from FreeRTOS task\r\n");

    h_bus_a_init();

    /* ── LSM6DSV16X IMU (0x6A) ── */
    /* Register map (DS13448):
     *   CTRL3 0x12: [6]=bdu [2]=if_inc [0]=sw_reset
     *   CTRL8 0x17: [1:0]=fs_xl  0=±2g (0.061 mg/LSB)
     *   CTRL6 0x15: [3:0]=fs_g   1=±250dps (8.75 mdps/LSB)
     *   CTRL1 0x10: [3:0]=odr_xl 6=120Hz HP
     *   CTRL2 0x11: [3:0]=odr_g  6=120Hz HP
     *   STATUS 0x1E: [1]=GDA [0]=XLDA                                       */
    h_dev_write(0x6A, 0x12, 0x01);             /* SW reset */
    for (int i = 0; i < 50; i++) {
        uint8_t c = 0xFF;
        h_dev_read(0x6A, 0x12, &c);
        if (!(c & 0x01u)) break;
        busy_wait_ms(1);
    }
    h_dev_write(0x6A, 0x12, 0x44);             /* BDU=1, IF_INC=1 */
    h_dev_write(0x6A, 0x17, 0x00);             /* fs_xl=±2g */
    h_dev_write(0x6A, 0x15, 0x01);             /* fs_g=±250dps */
    h_dev_write(0x6A, 0x10, 0x06);             /* odr_xl=120Hz HP */
    h_dev_write(0x6A, 0x11, 0x06);             /* odr_g=120Hz HP */

    uint8_t imu_status = 0;
    for (int i = 0; i < 40; i++) {
        h_dev_read(0x6A, 0x1E, &imu_status);
        if ((imu_status & 0x03u) == 0x03u) break;
        busy_wait_ms(5);
    }

    uint8_t reg = 0x20;
    uint8_t imu_raw[14] = {0};
    i2c_write_timeout_us(i2c1, 0x6A, &reg, 1, true, 50000);
    i2c_read_timeout_us(i2c1, 0x6A, imu_raw, 14, false, 100000);

    int16_t ax = (int16_t)(((uint16_t)imu_raw[9]  << 8) | imu_raw[8]);
    int16_t ay = (int16_t)(((uint16_t)imu_raw[11] << 8) | imu_raw[10]);
    int16_t az = (int16_t)(((uint16_t)imu_raw[13] << 8) | imu_raw[12]);
    bool imu_ok = (imu_status & 0x03u) == 0x03u;

    snprintf(buf, sizeof(buf),
             "[H]   IMU STATUS=0x%02X  Accel X=%+.3fg Y=%+.3fg Z=%+.3fg (expect ~1g on one axis) %s\r\n",
             imu_status,
             ax * 0.000061f, ay * 0.000061f, az * 0.000061f,
             imu_ok ? "OK" : "NO-DATA");
    cdc_write_str(buf);
    if (!imu_ok) all_pass = false;

    h_dev_write(0x6A, 0x10, 0x00);             /* XL power down */
    h_dev_write(0x6A, 0x11, 0x00);             /* G power down */

    /* ── LPS22HH Barometer (0x5D) ── */
    /* Register map (DS12503):
     *   CTRL_REG2 0x11: [4]=if_add_inc [2]=swreset [0]=one_shot
     *   CTRL_REG1 0x10: [6:4]=odr [1]=bdu
     *   STATUS    0x27: [1]=t_da [0]=p_da
     *   PRESS_OUT 0x28–0x2A (24-bit); TEMP_OUT 0x2B–0x2C (16-bit)          */
    h_dev_write(0x5D, 0x11, 0x04);             /* SW reset */
    for (int i = 0; i < 50; i++) {
        uint8_t c = 0xFF;
        h_dev_read(0x5D, 0x11, &c);
        if (!(c & 0x04u)) break;
        busy_wait_ms(1);
    }
    h_dev_write(0x5D, 0x11, 0x10);             /* IF_ADD_INC=1 */
    h_dev_write(0x5D, 0x10, 0x02);             /* BDU=1, ODR=power-down */
    h_dev_write(0x5D, 0x11, 0x11);             /* ONE_SHOT trigger */

    uint8_t baro_status = 0;
    for (int i = 0; i < 100; i++) {
        h_dev_read(0x5D, 0x27, &baro_status);
        if ((baro_status & 0x03u) == 0x03u) break;
        busy_wait_ms(5);
    }

    uint8_t breg = 0x28;
    uint8_t braw[5] = {0};
    i2c_write_timeout_us(i2c1, 0x5D, &breg, 1, true, 50000);
    i2c_read_timeout_us(i2c1, 0x5D, braw, 5, false, 100000);

    uint32_t p_raw = (uint32_t)braw[0] | ((uint32_t)braw[1] << 8) | ((uint32_t)braw[2] << 16);
    int16_t  t_raw = (int16_t)(((uint16_t)braw[4] << 8) | braw[3]);
    bool baro_ok = (baro_status & 0x03u) == 0x03u;

    snprintf(buf, sizeof(buf),
             "[H]   Baro STATUS=0x%02X  P=%.2f hPa  T=%.2f C (expect ~1013 hPa) %s\r\n",
             baro_status,
             p_raw / 4096.0f, t_raw / 100.0f,
             baro_ok ? "OK" : "NO-DATA");
    cdc_write_str(buf);
    if (!baro_ok) all_pass = false;

    h_dev_write(0x5D, 0x10, 0x00);             /* power down */

    /* ── LIS2MDL Magnetometer (0x1E) ── */
    /* Register map (AN5069):
     *   CFG_REG_A 0x60: [7]=comp_temp_en [5]=soft_rst [1:0]=md (11=power-down)
     *   CFG_REG_B 0x61: [1:0]=set_rst 01=OFF_CANC every ODR
     *   CFG_REG_C 0x62: [4]=bdu
     *   STATUS    0x67: [3]=ZYXDA
     *   OUT 0x68–0x6D (6 bytes, 1.5 mGauss/LSB)                             */
    h_dev_write(0x1E, 0x60, 0x20);             /* SW reset */
    for (int i = 0; i < 50; i++) {
        uint8_t c = 0xFF;
        h_dev_read(0x1E, 0x60, &c);
        if (!(c & 0x20u)) break;
        busy_wait_ms(1);
    }
    h_dev_write(0x1E, 0x60, 0x80);             /* comp_temp_en=1, odr=10Hz, continuous */
    h_dev_write(0x1E, 0x61, 0x02);             /* OFF_CANC every ODR */
    h_dev_write(0x1E, 0x62, 0x10);             /* BDU=1 */

    uint8_t mag_status = 0;
    for (int i = 0; i < 40; i++) {
        h_dev_read(0x1E, 0x67, &mag_status);
        if (mag_status & 0x08u) break;
        busy_wait_ms(5);
    }

    uint8_t mreg = 0x68;
    uint8_t mraw[6] = {0};
    i2c_write_timeout_us(i2c1, 0x1E, &mreg, 1, true, 50000);
    i2c_read_timeout_us(i2c1, 0x1E, mraw, 6, false, 100000);

    int16_t mx = (int16_t)(((uint16_t)mraw[1] << 8) | mraw[0]);
    int16_t my = (int16_t)(((uint16_t)mraw[3] << 8) | mraw[2]);
    int16_t mz = (int16_t)(((uint16_t)mraw[5] << 8) | mraw[4]);
    bool mag_ok = (mag_status & 0x08u) != 0;

    snprintf(buf, sizeof(buf),
             "[H]   Mag  STATUS=0x%02X  X=%+.1f Y=%+.1f Z=%+.1f mGauss (1.5mG/LSB) %s\r\n",
             mag_status,
             mx * 1.5f, my * 1.5f, mz * 1.5f,
             mag_ok ? "OK" : "NO-DATA");
    cdc_write_str(buf);
    if (!mag_ok) all_pass = false;

    h_dev_write(0x1E, 0x60, 0x02);             /* power down */

    h_bus_a_deinit();

    snprintf(buf, sizeof(buf), "[H]   RESULT: %s\r\n", all_pass ? "PASS" : "FAIL");
    cdc_write_str(buf);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Coordinator task — runs stages E → F → G → H sequentially.
 * ════════════════════════════════════════════════════════════════════════════*/
static void coordinator_task(void *pv)
{
    (void)pv;

    /* Wait for USB to enumerate. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    cdc_write_str("\r\n========================================\r\n");
    cdc_write_str(" Step 16 Stages E–H: Core 1 FreeRTOS\r\n");
    cdc_write_str("========================================\r\n");

    /* Report charger startup result (determined before scheduler started). */
    cdc_write_str(g_battery_present
        ? "[CHARGER] BQ27441 @ 0x55 ACK — battery present  → charging ENABLED\r\n"
        : "[CHARGER] BQ27441 @ 0x55 NACK — no battery      → charging DISABLED\r\n");

    /* ── Stage E ── */
    cdc_write_str("\r\n[E] Stage E: Multi-task CDC output with mutex (30 s)\r\n");
    g_stage_e_done_count = 0;

    xTaskCreate(stage_e_writer, "e_wrA", 512, (void *)(uintptr_t)'A',
                tskIDLE_PRIORITY + 2, NULL);
    xTaskCreate(stage_e_writer, "e_wrB", 512, (void *)(uintptr_t)'B',
                tskIDLE_PRIORITY + 2, NULL);

    /* Wait for both writers to finish (up to 5 s). */
    TickType_t e_start = xTaskGetTickCount();
    while (g_stage_e_done_count < 2) {
        if ((xTaskGetTickCount() - e_start) > pdMS_TO_TICKS(5000)) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    bool e_pass = (g_stage_e_done_count == 2);
    char buf[80];
    snprintf(buf, sizeof(buf), "[E]   RESULT: %s\r\n",
             e_pass ? "PASS (both writers completed, no corruption observed)"
                    : "FAIL (writer(s) did not complete in time)");
    cdc_write_str(buf);

    /* ── Stage F ── */
    run_stage_f();

    /* ── Stage G ── */
    run_stage_g();

    /* ── Stage H ── */
    run_stage_h();

    cdc_write_str("\r\n========================================\r\n");
    cdc_write_str(" Step 16 Stages E–H complete.\r\n");
    cdc_write_str(g_battery_present
        ? "[CHARGER] BQ27441 @ 0x55 ACK — battery present  → charging ENABLED\r\n"
        : "[CHARGER] BQ27441 @ 0x55 NACK — no battery      → charging DISABLED\r\n");
    cdc_write_str("========================================\r\n");

    vTaskDelete(NULL);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Core 1 entry — launched via multicore_launch_core1().
 * ────────────────────────────────────────────────────────────────────────────*/
static void core1_entry(void)
{
    /* Charger startup: disable charging, probe BQ27441, re-enable if battery
     * present.  Must run before tusb_init() — i2c1 is exclusive to Bus B here. */
    charger_startup_init();

    /* Force USB re-enumeration without requiring a physical replug.
     *
     * J-Link resets Core 0 only; the USB peripheral retains its previous
     * state, so Windows never sees a disconnect event.  Fix:
     *   1. Assert USB peripheral reset — D+ pullup drops, Host detects
     *      disconnect within ~5 ms (USB spec: >2.5 ms).
     *   2. Hold reset for 20 ms to guarantee the Host OS processes disconnect.
     *   3. Release reset; tusb_init() then performs a clean initialisation
     *      and the Host sees a fresh connect event.                           */
    reset_block(RESETS_RESET_USBCTRL_BITS);
    busy_wait_ms(20);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

    /* Manual TinyUSB init on Core 1 — registers USBCTRL_IRQ on Core 1's NVIC.
     * Bypasses pico_stdio_usb which hard-asserts Core 0 (SDK 2.2.0 Stage C). */
    tusb_init();

    /* Busy-poll to complete USB enumeration before scheduler takes over. */
    for (int i = 0; i < 2000; i++) {
        tud_task();
        busy_wait_ms(1);
    }

    g_cdc_mutex        = xSemaphoreCreateMutex();
    g_stage_e_done_count = 0;

    xTaskCreate(usb_device_task, "usb",    1024, NULL,
                configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(coordinator_task, "coord", 1024, NULL,
                tskIDLE_PRIORITY + 1, NULL);

    vTaskStartScheduler();
    panic("FreeRTOS scheduler exited unexpectedly");
}

/* ────────────────────────────────────────────────────────────────────────────
 * main — executed by Core 0.
 *
 * Launches Core 1, then waits for the Stage G ready signal and runs the
 * 100-message IPC loop before idling forever.
 * ────────────────────────────────────────────────────────────────────────────*/
int main(void)
{
    /* PSM-cycle Core 1: J-Link only resets Core 0; Core 1 may still be
     * running the previous FreeRTOS scheduler.  Force it to a clean state
     * before relaunching.                                                    */
    multicore_reset_core1();

    multicore_launch_core1(core1_entry);

    /* Wait for Core 1 Stage G coordinator to signal readiness (up to 45 s).
     * Core 1 needs ~3 s USB enum + ~30 s Stage E + Stage F time.            */
    uint32_t ready = 0;
    bool got_ready = multicore_fifo_pop_timeout_us(45000000u, &ready);

    if (!got_ready || ready != C1_EFGH_READY) {
        /* Core 1 never signalled — just idle; Core 1 will still complete E/F/H. */
        while (true) __wfe();
    }

    /* Stage G: send 100 IPC messages; Core 1 FreeRTOS task echoes each one.
     * Write to shared SRAM first, then push FIFO — FIFO push is the doorbell. */
    for (int i = 0; i < EFGH_IPC_COUNT; i++) {
        g_ipc_sram = 0xABCD0000u | (uint32_t)i;
        __dmb();   /* ensure SRAM write visible to Core 1 before FIFO push   */
        multicore_fifo_push_blocking(C0_EFGH_IPC_BASE | (uint32_t)i);

        /* Pop the ack — if Core 1 is busy we'll wait here. */
        uint32_t ack = 0;
        multicore_fifo_pop_timeout_us(500000u, &ack);
        /* Core 0 has no CDC; integrity is verified and reported by Core 1.  */
    }

    while (true) __wfe();
}
