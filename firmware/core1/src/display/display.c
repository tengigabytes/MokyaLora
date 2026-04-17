/* display.c — Core 1 ST7789VI display driver: PIO + DMA bus, panel init,
 * partial flush, TE polling, and LM27965 backlight bring-up.
 *
 * The PIO program (tft_8080.pio) drives nWR + D[7:0]; nCS, DCX, nRST, TE stay
 * on the SIO so they can change at command/data boundaries without spending
 * PIO bandwidth. A single DMA channel pushes pixel/parameter bytes into the
 * SM TX FIFO with DREQ paced by the PIO autopull.
 *
 * Backlight is provisional — the LM27965 setup mirrors the bringup Step 4
 * sequence (Bank A code 0x16, GP register 0x21 → ENA=1) and reuses the
 * shared bus-B I2C bring-up. A dedicated power / backlight subsystem will
 * replace this in a later milestone.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display.h"
#include "st7789vi.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/regs/i2c.h"
#include "pico/stdlib.h"

#include "tft_8080.pio.h"

/* ── I2C baudrate fix for Core 1 ─────────────────────────────────────────────
 * Core 1 skips runtime_init_clocks (Core 0 owns clock init), so
 * clock_get_hz(clk_peri) returns 0 and i2c_init() silently computes a garbage
 * baudrate divisor. We manually set the SCL timing registers using the known
 * clk_peri frequency (Core 0 sets Arduino-Pico default 150 MHz).
 *
 * Formula (from Pico SDK i2c.c):
 *   period = freq_in / baudrate
 *   lcnt = period * 3/5 - 1   (low phase, slightly longer)
 *   hcnt = period - lcnt - 8  (high phase, account for rise time)
 *   For 100 kHz @ 150 MHz: period=1500, lcnt=899, hcnt=593
 */
#define CORE1_CLK_PERI_HZ  150000000u

static void i2c_set_baudrate_core1(i2c_inst_t *i2c, uint baudrate)
{
    uint period = CORE1_CLK_PERI_HZ / baudrate;
    uint lcnt = period * 3u / 5u - 1u;
    uint hcnt = period - lcnt - 8u;
    /* Clamp to valid range (must be >= 8 for standard mode). */
    if (hcnt < 8u) hcnt = 8u;
    if (lcnt < 1u) lcnt = 1u;
    i2c->hw->enable = 0;
    hw_write_masked(&i2c->hw->con,
                    I2C_IC_CON_SPEED_VALUE_STANDARD << I2C_IC_CON_SPEED_LSB,
                    I2C_IC_CON_SPEED_BITS);
    i2c->hw->ss_scl_hcnt = hcnt;
    i2c->hw->ss_scl_lcnt = lcnt;
    i2c->hw->fs_spklen = lcnt < 16u ? 1u : lcnt / 16u;
    i2c->hw->enable = 1;
}

/* ── GPIO map (Rev A schematic) ──────────────────────────────────────────── */
#define PIN_TFT_nCS    10u
#define PIN_TFT_DCX    11u
#define PIN_TFT_nWR    12u
#define PIN_TFT_D0     13u   /* D0..D7 = 13..20, contiguous */
#define PIN_TFT_nRST   21u
#define PIN_TFT_TE     22u

/* ── Bus + PIO selection ─────────────────────────────────────────────────── */
#define DISPLAY_PIO          pio1
#define DISPLAY_PIO_CLKDIV   2.0f   /* 53 ns write cycle @ 150 MHz sys_clk */

/* ── LM27965 backlight (provisional) ─────────────────────────────────────── */
#define BL_I2C            i2c1
#define PIN_BL_SDA        6u
#define PIN_BL_SCL        7u
#define LM27965_ADDR      0x36u
#define LM27965_REG_GP    0x10u
#define LM27965_REG_BANKA 0xA0u
#define LM27965_BANKA_DUTY 0x16u   /* code 22 (~40%) — matches bringup firmware */
#define LM27965_GP_TFT_ON  0x21u   /* ENA(bit0)=1, reserved bit5 stays 1 */

/* ── Driver state ────────────────────────────────────────────────────────── */
static uint  s_sm;
static uint  s_pio_offset;
static int   s_dma_ch;

/* 2-byte ring source for solid-colour fills — must be 2-byte aligned so the
 * PIO autopull / DMA ring (size=1, i.e. 2 bytes) wraps correctly. */
static uint8_t __attribute__((aligned(2))) s_solid_pix[2];

/* Per-row scratch buffer for display_flush_rect_strided(): one row's worth of
 * endian-swapped pixels ready for the panel. Keeps the LVGL framebuffer in
 * its native little-endian layout. 480 B for 240 px max row width. */
static uint8_t __attribute__((aligned(4))) s_flush_scratch[DISPLAY_W * 2u];

/* ── Low-level bus helpers ───────────────────────────────────────────────── */

static inline void bus_flush(void)
{
    while (!pio_sm_is_tx_fifo_empty(DISPLAY_PIO, s_sm))
        tight_loop_contents();
    /* Allow the last byte to clock out (4 PIO cycles ≈ 80 ns). */
    sleep_us(1);
}

static inline void bus_write_byte(uint8_t b)
{
    pio_sm_put_blocking(DISPLAY_PIO, s_sm, (uint32_t)b);
}

static void bus_send_cmd(uint8_t cmd)
{
    bus_flush();
    gpio_put(PIN_TFT_DCX, 0);
    bus_write_byte(cmd);
    bus_flush();
    gpio_put(PIN_TFT_DCX, 1);
}

static void bus_send_data(uint8_t d)
{
    bus_write_byte(d);
}

/* ── Address window ──────────────────────────────────────────────────────── */

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    bus_send_cmd(ST7789_CASET);
    bus_send_data((uint8_t)(x0 >> 8)); bus_send_data((uint8_t)(x0 & 0xFF));
    bus_send_data((uint8_t)(x1 >> 8)); bus_send_data((uint8_t)(x1 & 0xFF));
    bus_send_cmd(ST7789_RASET);
    bus_send_data((uint8_t)(y0 >> 8)); bus_send_data((uint8_t)(y0 & 0xFF));
    bus_send_data((uint8_t)(y1 >> 8)); bus_send_data((uint8_t)(y1 & 0xFF));
}

/* ── DMA pixel push ──────────────────────────────────────────────────────── */

/* Pre-built channel config templates so each flush avoids the SDK helper
 * overhead. set_read_increment is the only field that ever changes. */
static dma_channel_config make_pixel_cfg(bool read_inc, bool ring_2b)
{
    dma_channel_config cfg = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_dreq(&cfg, pio_get_dreq(DISPLAY_PIO, s_sm, true));
    channel_config_set_read_increment(&cfg, read_inc);
    channel_config_set_write_increment(&cfg, false);
    if (ring_2b)
        channel_config_set_ring(&cfg, false, 1);  /* read ring = 2 bytes */
    return cfg;
}

/* ── Backlight (LM27965 over I2C bus B) ──────────────────────────────────── */

/* Bus B recovery: 9 SCL pulses + manual STOP, per bringup i2c_custom_scan.c.
 * Handles the case where a slave has SDA clamped low after a boot-time glitch
 * on the 1.8V pull-up rail. Called before any I2C traffic on bus B.          */
static void bus_b_recovery(void)
{
    gpio_init(PIN_BL_SCL);
    gpio_set_dir(PIN_BL_SCL, GPIO_OUT);
    gpio_put(PIN_BL_SCL, 1);

    gpio_init(PIN_BL_SDA);
    gpio_set_dir(PIN_BL_SDA, GPIO_IN);
    gpio_pull_up(PIN_BL_SDA);

    sleep_us(10);

    for (int i = 0; i < 9; i++) {
        gpio_put(PIN_BL_SCL, 0);
        sleep_us(5);
        gpio_put(PIN_BL_SCL, 1);
        sleep_us(5);
        if (gpio_get(PIN_BL_SDA)) break;
    }

    /* Manual STOP: SDA rising edge while SCL HIGH. */
    gpio_set_dir(PIN_BL_SDA, GPIO_OUT);
    gpio_put(PIN_BL_SDA, 0);
    sleep_us(5);
    gpio_put(PIN_BL_SCL, 1);
    sleep_us(5);
    gpio_put(PIN_BL_SDA, 1);
    sleep_us(10);

    gpio_set_dir(PIN_BL_SCL, GPIO_IN);
    gpio_set_dir(PIN_BL_SDA, GPIO_IN);
    gpio_disable_pulls(PIN_BL_SCL);
    gpio_disable_pulls(PIN_BL_SDA);
}

static void backlight_init(void)
{
    /* bus_b_recovery() unsticks SDA if a slave is holding it low. */
    bus_b_recovery();

    /* i2c_init() brings the peripheral out of reset and enables it, but its
     * baudrate calculation uses clock_get_hz(clk_peri) which returns 0 on
     * Core 1 (we skip runtime_init_clocks). Call i2c_init with any baudrate
     * then fix up with our known 150 MHz clk_peri. */
    i2c_init(BL_I2C, 100 * 1000);
    i2c_set_baudrate_core1(BL_I2C, 100 * 1000);
    gpio_set_function(PIN_BL_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_BL_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_BL_SDA);
    gpio_pull_up(PIN_BL_SCL);

    const uint8_t set_duty[2] = { LM27965_REG_BANKA, LM27965_BANKA_DUTY };
    const uint8_t enable_a[2] = { LM27965_REG_GP,    LM27965_GP_TFT_ON  };

    /* Order: duty first, GP second — matches bringup_tft.c and led_apply(). */
    (void)i2c_write_timeout_us(BL_I2C, LM27965_ADDR, set_duty, 2, false, 50000);
    (void)i2c_write_timeout_us(BL_I2C, LM27965_ADDR, enable_a, 2, false, 50000);

    i2c_deinit(BL_I2C);
    gpio_set_function(PIN_BL_SDA, GPIO_FUNC_NULL);
    gpio_set_function(PIN_BL_SCL, GPIO_FUNC_NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool display_init(void)
{
    /* SIO pins. nCS HIGH, DCX HIGH (data default), nRST HIGH. */
    gpio_init(PIN_TFT_nCS);  gpio_set_dir(PIN_TFT_nCS,  GPIO_OUT); gpio_put(PIN_TFT_nCS,  1);
    gpio_init(PIN_TFT_DCX);  gpio_set_dir(PIN_TFT_DCX,  GPIO_OUT); gpio_put(PIN_TFT_DCX,  1);
    gpio_init(PIN_TFT_nRST); gpio_set_dir(PIN_TFT_nRST, GPIO_OUT); gpio_put(PIN_TFT_nRST, 1);
    gpio_init(PIN_TFT_TE);   gpio_set_dir(PIN_TFT_TE,   GPIO_IN);

    /* PIO program load + SM init. */
    int sm_n = pio_claim_unused_sm(DISPLAY_PIO, false);
    if (sm_n < 0) return false;
    s_sm = (uint)sm_n;

    if (!pio_can_add_program(DISPLAY_PIO, &tft_8080_program)) {
        pio_sm_unclaim(DISPLAY_PIO, s_sm);
        return false;
    }
    s_pio_offset = pio_add_program(DISPLAY_PIO, &tft_8080_program);
    tft_8080_program_init(DISPLAY_PIO, s_sm, s_pio_offset,
                          PIN_TFT_D0, PIN_TFT_nWR, DISPLAY_PIO_CLKDIV);
    pio_sm_set_enabled(DISPLAY_PIO, s_sm, true);

    /* DMA channel. */
    int ch = dma_claim_unused_channel(false);
    if (ch < 0) {
        pio_sm_set_enabled(DISPLAY_PIO, s_sm, false);
        pio_remove_program(DISPLAY_PIO, &tft_8080_program, s_pio_offset);
        pio_sm_unclaim(DISPLAY_PIO, s_sm);
        return false;
    }
    s_dma_ch = ch;

    /* ST7789VI hardware reset: nRST low ≥ 10 µs, then ≥ 120 ms recovery. */
    gpio_put(PIN_TFT_nRST, 0); sleep_ms(10);
    gpio_put(PIN_TFT_nRST, 1); sleep_ms(120);

    /* Hold nCS low for the rest of the session — we are the only PIO master. */
    gpio_put(PIN_TFT_nCS, 0);

    st7789_init(bus_send_cmd, bus_send_data);

    backlight_init();
    return true;
}

void display_flush_rect(uint16_t x0, uint16_t y0,
                        uint16_t x1, uint16_t y1,
                        const uint8_t *pixels)
{
    const uint32_t bytes = (uint32_t)(x1 - x0 + 1u)
                         * (uint32_t)(y1 - y0 + 1u) * 2u;

    set_window(x0, y0, x1, y1);
    bus_send_cmd(ST7789_RAMWR);

    dma_channel_config cfg = make_pixel_cfg(/*read_inc=*/true, /*ring_2b=*/false);
    dma_channel_configure(s_dma_ch, &cfg,
                          (volatile void *)&DISPLAY_PIO->txf[s_sm],
                          pixels,
                          bytes,
                          true);
    dma_channel_wait_for_finish_blocking(s_dma_ch);
    bus_flush();
}

void display_flush_rect_strided(uint16_t x0, uint16_t y0,
                                uint16_t x1, uint16_t y1,
                                const uint16_t *fb_base,
                                uint16_t fb_stride_px)
{
    const uint16_t w = (uint16_t)(x1 - x0 + 1u);
    const uint16_t h = (uint16_t)(y1 - y0 + 1u);
    const uint32_t row_bytes = (uint32_t)w * 2u;

    set_window(x0, y0, x1, y1);
    bus_send_cmd(ST7789_RAMWR);

    dma_channel_config cfg = make_pixel_cfg(/*read_inc=*/true, /*ring_2b=*/false);

    for (uint16_t r = 0; r < h; r++) {
        const uint16_t *src = fb_base +
                              (uint32_t)(y0 + r) * (uint32_t)fb_stride_px +
                              (uint32_t)x0;
        /* Fast byte-swap using ARM REV16: swaps both halfwords in a 32-bit
         * word simultaneously. Process 2 pixels per iteration. */
        const uint32_t *src32 = (const uint32_t *)src;
        uint32_t *dst32 = (uint32_t *)s_flush_scratch;
        uint16_t pairs = w >> 1;
        for (uint16_t i = 0; i < pairs; i++) {
            uint32_t v = src32[i];
            __asm__ volatile ("rev16 %0, %1" : "=r"(v) : "r"(v));
            dst32[i] = v;
        }
        /* Handle odd trailing pixel if width is odd. */
        if (w & 1u) {
            uint16_t p = src[w - 1u];
            ((uint16_t *)s_flush_scratch)[w - 1u] = (uint16_t)((p >> 8) | (p << 8));
        }

        dma_channel_configure(s_dma_ch, &cfg,
                              (volatile void *)&DISPLAY_PIO->txf[s_sm],
                              s_flush_scratch,
                              row_bytes,
                              true);
        dma_channel_wait_for_finish_blocking(s_dma_ch);
    }
    bus_flush();
}

void display_fill_solid(uint16_t rgb565)
{
    s_solid_pix[0] = (uint8_t)(rgb565 >> 8);
    s_solid_pix[1] = (uint8_t)(rgb565 & 0xFF);

    set_window(0, 0, DISPLAY_W - 1u, DISPLAY_H - 1u);
    bus_send_cmd(ST7789_RAMWR);

    dma_channel_config cfg = make_pixel_cfg(/*read_inc=*/true, /*ring_2b=*/true);
    dma_channel_configure(s_dma_ch, &cfg,
                          (volatile void *)&DISPLAY_PIO->txf[s_sm],
                          s_solid_pix,
                          (uint32_t)DISPLAY_W * DISPLAY_H * 2u,
                          true);
    dma_channel_wait_for_finish_blocking(s_dma_ch);
    bus_flush();
}

void display_wait_te_rise(void)
{
    while ( gpio_get(PIN_TFT_TE)) tight_loop_contents();
    while (!gpio_get(PIN_TFT_TE)) tight_loop_contents();
}
