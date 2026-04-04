#include "bringup.h"
#include "tft_8080.pio.h"

// ---------------------------------------------------------------------------
// ST7789VI — 2.4" IPS 240×320, 8080 8-bit parallel
//
// GPIO allocation:
//   GPIO 10  TFT_nCS   — SIO output, active-low
//   GPIO 11  TFT_DCX   — SIO output, 0=command 1=data
//   GPIO 12  TFT_nWR   — PIO side-set, active-low write strobe
//   GPIO 13..20         — PIO out, D[7:0] data bus
//   GPIO 21  TFT_nRST  — SIO output, active-low reset
//   GPIO 22  TFT_TE    — SIO input, tearing effect (bringup: ignored)
//
// Uses pio1 to avoid gpio_base conflict with amp (pio0 base set to 16).
// GPIO 10–22 are all within 0–31 — pio1 default base (0) covers them.
//
// Write cycle: clkdiv=4 → 128 ns (safe for ST7789 at 1.8V VDDI)
// ---------------------------------------------------------------------------

#define TFT_PIO      pio1
#define TFT_CLK_DIV  4.0f

static uint tft_sm;
static uint tft_offset;

// ---------------------------------------------------------------------------
// PIO lifecycle
// ---------------------------------------------------------------------------

static bool tft_pio_start(void) {
    int n = pio_claim_unused_sm(TFT_PIO, false);
    if (n < 0) { printf("ERROR: no free PIO1 SM\n"); return false; }
    tft_sm = (uint)n;

    if (!pio_can_add_program(TFT_PIO, &tft_8080_program)) {
        printf("ERROR: PIO1 program space full\n");
        pio_sm_unclaim(TFT_PIO, tft_sm);
        return false;
    }
    tft_offset = pio_add_program(TFT_PIO, &tft_8080_program);
    tft_8080_program_init(TFT_PIO, tft_sm, tft_offset,
                          TFT_D0_PIN, TFT_nWR_PIN, TFT_CLK_DIV);
    pio_sm_set_enabled(TFT_PIO, tft_sm, true);
    return true;
}

static void tft_pio_stop(void) {
    pio_sm_set_enabled(TFT_PIO, tft_sm, false);
    pio_remove_program(TFT_PIO, &tft_8080_program, tft_offset);
    pio_sm_unclaim(TFT_PIO, tft_sm);
    for (uint i = 0; i < 8; i++)
        gpio_set_function(TFT_D0_PIN + i, GPIO_FUNC_NULL);
    gpio_set_function(TFT_nWR_PIN, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// Low-level write helpers
// ---------------------------------------------------------------------------

// Wait until TX FIFO is empty and SM has finished the last byte (~1 µs margin).
static inline void tft_flush(void) {
    while (!pio_sm_is_tx_fifo_empty(TFT_PIO, tft_sm))
        tight_loop_contents();
    sleep_us(1);
}

// Send one byte via PIO. DCX must already be set by caller.
static inline void tft_write8(uint8_t b) {
    pio_sm_put_blocking(TFT_PIO, tft_sm, (uint32_t)b);
}

// Send command byte (DCX=0), then restore DCX=1 for subsequent data.
static void tft_cmd(uint8_t cmd) {
    tft_flush();
    gpio_put(TFT_DCX_PIN, 0);
    tft_write8(cmd);
    tft_flush();
    gpio_put(TFT_DCX_PIN, 1);
}

// Send data byte (DCX must already be 1).
static inline void tft_dat(uint8_t d) {
    tft_write8(d);
}

// ---------------------------------------------------------------------------
// ST7789VI initialisation sequence
//
// Based on Sitronix ST7789V application note + Newhaven NHD-2.4-240320AF-CSXP
// recommended sequence. COLMOD=0x55 → 16-bit RGB565 (2 bytes per pixel).
// MADCTL=0x00 → portrait, top-left origin, RGB order.
// ---------------------------------------------------------------------------

static void st7789_init(void) {
    // Software reset — resets all registers to POR defaults
    tft_cmd(0x01);
    sleep_ms(150);

    // Sleep out — exits sleep mode, starts oscillator
    tft_cmd(0x11);
    sleep_ms(120);

    // Interface pixel format: 16-bit RGB565
    tft_cmd(0x3A); tft_dat(0x55);

    // Memory data access control: normal portrait orientation, RGB colour order
    tft_cmd(0x36); tft_dat(0x00);

    // Porch setting (default from datasheet — avoids tearing artefacts)
    tft_cmd(0xB2);
    tft_dat(0x0C); tft_dat(0x0C); tft_dat(0x00);
    tft_dat(0x33); tft_dat(0x33);

    // Gate control: VGL / VGH
    tft_cmd(0xB7); tft_dat(0x35);

    // VCOM setting
    tft_cmd(0xBB); tft_dat(0x19);

    // LCM control
    tft_cmd(0xC0); tft_dat(0x2C);

    // VDV and VRH command enable
    tft_cmd(0xC2); tft_dat(0x01);

    // VRH set
    tft_cmd(0xC3); tft_dat(0x12);

    // VDV set
    tft_cmd(0xC4); tft_dat(0x20);

    // Frame rate: 60 Hz in normal mode
    tft_cmd(0xC6); tft_dat(0x0F);

    // Power control 1
    tft_cmd(0xD0); tft_dat(0xA4); tft_dat(0xA1);

    // Positive voltage gamma control
    tft_cmd(0xE0);
    tft_dat(0xD0); tft_dat(0x04); tft_dat(0x0D); tft_dat(0x11);
    tft_dat(0x13); tft_dat(0x2B); tft_dat(0x3F); tft_dat(0x54);
    tft_dat(0x4C); tft_dat(0x18); tft_dat(0x0D); tft_dat(0x0B);
    tft_dat(0x1F); tft_dat(0x23);

    // Negative voltage gamma control
    tft_cmd(0xE1);
    tft_dat(0xD0); tft_dat(0x04); tft_dat(0x0C); tft_dat(0x11);
    tft_dat(0x13); tft_dat(0x2C); tft_dat(0x3F); tft_dat(0x44);
    tft_dat(0x51); tft_dat(0x2F); tft_dat(0x1F); tft_dat(0x1F);
    tft_dat(0x20); tft_dat(0x23);

    // Column address set: 0..239
    tft_cmd(0x2A);
    tft_dat(0x00); tft_dat(0x00);
    tft_dat(0x00); tft_dat(0xEF);

    // Row address set: 0..319
    tft_cmd(0x2B);
    tft_dat(0x00); tft_dat(0x00);
    tft_dat(0x01); tft_dat(0x3F);

    // Display inversion on (required by most ST7789 IPS panels)
    tft_cmd(0x21);

    // Display on
    tft_cmd(0x29);
    sleep_ms(20);
}

// ---------------------------------------------------------------------------
// Screen fill — 240 × 320 pixels, RGB565 colour, 2 bytes per pixel
// ---------------------------------------------------------------------------

static void tft_fill(uint16_t colour) {
    uint8_t hi = (uint8_t)(colour >> 8);
    uint8_t lo = (uint8_t)(colour & 0xFF);

    // Memory write command, then stream pixel data
    tft_cmd(0x2C);
    // DCX is now 1 (data), write all pixels directly into PIO FIFO
    for (int i = 0; i < 240 * 320; i++) {
        pio_sm_put_blocking(TFT_PIO, tft_sm, (uint32_t)hi);
        pio_sm_put_blocking(TFT_PIO, tft_sm, (uint32_t)lo);
    }
    tft_flush();
}

// ---------------------------------------------------------------------------
// Partial-update helpers
// ---------------------------------------------------------------------------

// Set CASET/RASET address window (inclusive)
static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    tft_cmd(0x2A);
    tft_dat(x0 >> 8); tft_dat(x0 & 0xFF);
    tft_dat(x1 >> 8); tft_dat(x1 & 0xFF);
    tft_cmd(0x2B);
    tft_dat(y0 >> 8); tft_dat(y0 & 0xFF);
    tft_dat(y1 >> 8); tft_dat(y1 & 0xFF);
}

// Fill a rectangle with a solid colour
static void tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour) {
    if (!w || !h) return;
    tft_set_window(x, y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    tft_cmd(0x2C);
    uint8_t hi = colour >> 8, lo = colour & 0xFF;
    for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
        pio_sm_put_blocking(TFT_PIO, tft_sm, hi);
        pio_sm_put_blocking(TFT_PIO, tft_sm, lo);
    }
    tft_flush();
}

// Convert hue 0-359 → RGB565 (full saturation, full value)
static uint16_t hue_to_rgb565(int h) {
    int sector = h / 60;
    int frac   = (h % 60) * 255 / 59;
    uint8_t r, g, b;
    switch (sector) {
        case 0: r = 255;        g = (uint8_t)frac;       b = 0;               break;
        case 1: r = 255 - frac; g = 255;                 b = 0;               break;
        case 2: r = 0;          g = 255;                 b = (uint8_t)frac;   break;
        case 3: r = 0;          g = 255 - frac;          b = 255;             break;
        case 4: r = (uint8_t)frac; g = 0;               b = 255;             break;
        default:r = 255;        g = 0;                   b = 255 - frac;      break;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// ---------------------------------------------------------------------------
// Dynamic sub-tests
// ---------------------------------------------------------------------------

// Sub-test 1: 8 SMPTE-style vertical colour bars (each 30 px wide)
static void tft_subtest_colorbars(void) {
    printf("  [dynamic 1/4] SMPTE colour bars...");
    static const uint16_t bars[8] = {
        0xFFFF,  // White
        0xFFE0,  // Yellow
        0x07FF,  // Cyan
        0x07E0,  // Green
        0xF81F,  // Magenta
        0xF800,  // Red
        0x001F,  // Blue
        0x0000,  // Black
    };
    for (int i = 0; i < 8; i++) {
        uint16_t x = (uint16_t)(i * 30);
        tft_fill_rect(x, 0, 30, 320, bars[i]);
    }
    printf(" done\n");
    sleep_ms(2500);
}

// Sub-test 2: Full-screen hue gradient, then animate a scroll for ~3 s
static void tft_subtest_gradient(void) {
    printf("  [dynamic 2/4] Hue gradient + scroll...");

    // Precompute all 360 hues and per-column base hue
    static uint16_t rainbow[360];
    static uint16_t col_hue[240];
    for (int i = 0; i < 360; i++) rainbow[i] = hue_to_rgb565(i);
    for (int x = 0; x < 240; x++) col_hue[x] = (uint16_t)(x * 360 / 240);

    // Static gradient (one pass)
    tft_set_window(0, 0, 239, 319);
    tft_cmd(0x2C);
    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 240; x++) {
            uint16_t c = rainbow[col_hue[x]];
            pio_sm_put_blocking(TFT_PIO, tft_sm, c >> 8);
            pio_sm_put_blocking(TFT_PIO, tft_sm, c & 0xFF);
        }
    }
    tft_flush();
    sleep_ms(1500);

    // Scroll: 90 frames, hue offset advances 4° per frame (~3 s)
    for (int offset = 0; offset < 360; offset += 4) {
        tft_set_window(0, 0, 239, 319);
        tft_cmd(0x2C);
        for (int y = 0; y < 320; y++) {
            for (int x = 0; x < 240; x++) {
                int idx = col_hue[x] + offset;
                if (idx >= 360) idx -= 360;
                uint16_t c = rainbow[idx];
                pio_sm_put_blocking(TFT_PIO, tft_sm, c >> 8);
                pio_sm_put_blocking(TFT_PIO, tft_sm, c & 0xFF);
            }
        }
        tft_flush();
    }
    printf(" done\n");
}

// Sub-test 3: Black-and-white checkerboard (32×32 px blocks)
static void tft_subtest_checkerboard(void) {
    printf("  [dynamic 3/4] Checkerboard (32×32 blocks)...");
    tft_set_window(0, 0, 239, 319);
    tft_cmd(0x2C);
    for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 240; x++) {
            uint16_t c = (((x >> 5) ^ (y >> 5)) & 1) ? 0xFFFF : 0x0000;
            pio_sm_put_blocking(TFT_PIO, tft_sm, c >> 8);
            pio_sm_put_blocking(TFT_PIO, tft_sm, c & 0xFF);
        }
    }
    tft_flush();
    printf(" done\n");
    sleep_ms(2500);
}

// Sub-test 4: Bouncing yellow block on blue background (~80 frames)
static void tft_subtest_bounce(void) {
    printf("  [dynamic 4/4] Bouncing block (80 frames)...\n");
    const uint16_t BW = 32, BH = 32;
    const uint16_t BG = 0x001F;  // blue
    const uint16_t FG = 0xFFE0;  // yellow

    tft_fill(BG);

    int bx = 0, by = 0, dx = 4, dy = 3;
    for (int f = 0; f < 80; f++) {
        tft_fill_rect((uint16_t)bx, (uint16_t)by, BW, BH, BG);  // erase
        bx += dx;  by += dy;
        if (bx < 0)              { bx = 0;            dx = -dx; }
        if (by < 0)              { by = 0;             dy = -dy; }
        if (bx + (int)BW > 240)  { bx = 240 - BW;     dx = -dx; }
        if (by + (int)BH > 320)  { by = 320 - BH;     dy = -dy; }
        tft_fill_rect((uint16_t)bx, (uint16_t)by, BW, BH, FG);  // draw
        sleep_ms(40);
    }
    printf("    done\n");
}

// ---------------------------------------------------------------------------
// tft_test — public entry point
// ---------------------------------------------------------------------------

void tft_test(void) {
    printf("\n--- TFT LCD Test (ST7789VI, 8080 8-bit parallel) ---\n");
    printf("  nCS=GPIO%-2d  DCX=GPIO%-2d  nWR=GPIO%-2d\n",
           TFT_nCS_PIN, TFT_DCX_PIN, TFT_nWR_PIN);
    printf("  D[7:0]=GPIO%d..%d  nRST=GPIO%-2d\n",
           TFT_D0_PIN, TFT_D0_PIN + 7, TFT_nRST_PIN);
    printf("  clkdiv=%.0f  write cycle=%.0f ns\n",
           TFT_CLK_DIV, 4.0f * TFT_CLK_DIV * 1000.0f / 125.0f);

    // SIO-controlled pins
    gpio_init(TFT_nCS_PIN);  gpio_set_dir(TFT_nCS_PIN,  GPIO_OUT); gpio_put(TFT_nCS_PIN,  1);
    gpio_init(TFT_DCX_PIN);  gpio_set_dir(TFT_DCX_PIN,  GPIO_OUT); gpio_put(TFT_DCX_PIN,  1);
    gpio_init(TFT_nRST_PIN); gpio_set_dir(TFT_nRST_PIN, GPIO_OUT); gpio_put(TFT_nRST_PIN, 1);
    gpio_init(TFT_TE_PIN);   gpio_set_dir(TFT_TE_PIN,   GPIO_IN);

    // PIO for data bus + nWR
    if (!tft_pio_start()) {
        gpio_set_function(TFT_nCS_PIN,  GPIO_FUNC_NULL);
        gpio_set_function(TFT_DCX_PIN,  GPIO_FUNC_NULL);
        gpio_set_function(TFT_nRST_PIN, GPIO_FUNC_NULL);
        return;
    }

    // Hardware reset
    printf("  Hardware reset...\n");
    gpio_put(TFT_nRST_PIN, 0); sleep_ms(10);
    gpio_put(TFT_nRST_PIN, 1); sleep_ms(120);

    gpio_put(TFT_nCS_PIN, 0);   // assert CS for entire session

    // Init sequence
    printf("  Sending init sequence...\n");
    st7789_init();
    printf("  Init done.\n");

    // Backlight on via LM27965 Bank A (40%)
    bus_b_init();
    lm_write(LM27965_BANKA, 0x16);
    lm_write(LM27965_GP, 0x21);   // ENA=1 (bit0), bit5 always 1
    bus_b_deinit();
    printf("  Backlight on (40%%)\n");

    // --- Step A: solid colour fills (basic pixel sanity) ---
    static const struct { uint16_t colour; const char *name; } seq[] = {
        { 0xF800, "Red"   },
        { 0x07E0, "Green" },
        { 0x001F, "Blue"  },
        { 0xFFFF, "White" },
        { 0x0000, "Black" },
    };
    for (int i = 0; i < 5; i++) {
        printf("  Fill %-6s (0x%04X) ...", seq[i].name, seq[i].colour);
        tft_fill(seq[i].colour);
        printf(" done\n");
        sleep_ms(1000);
    }

    // --- Step B: dynamic tests ---
    tft_subtest_colorbars();
    tft_subtest_gradient();
    tft_subtest_checkerboard();
    tft_subtest_bounce();

    // --- Step C: backlight fade (white screen, 32 codes up then down) ---
    printf("  [backlight] Fade in/out...");
    tft_fill(0xFFFF);
    bus_b_init();
    for (int c = 0; c <= 0x1F; c++) { lm_write(LM27965_BANKA, (uint8_t)c); sleep_ms(40); }
    for (int c = 0x1F; c >= 0; c--) { lm_write(LM27965_BANKA, (uint8_t)c); sleep_ms(40); }
    lm_write(LM27965_BANKA, 0x16);  // restore 40%
    bus_b_deinit();
    printf(" done\n");

    // Backlight off
    bus_b_init();
    lm_write(LM27965_GP, 0x20);
    bus_b_deinit();

    gpio_put(TFT_nCS_PIN, 1);
    tft_pio_stop();

    gpio_set_function(TFT_nCS_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(TFT_DCX_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(TFT_nRST_PIN, GPIO_FUNC_NULL);

    printf("Done\n");
}

// ---------------------------------------------------------------------------
// Step 13 — TFT LCD Fast Refresh
//
// Sub-tests:
//   A  TE pin frequency check — verify ST7789VI TE toggles at ~60 Hz
//   B  Baseline FPS           — 10 solid fills via CPU polling
//   C  DMA solid fill FPS     — 10 fills via DMA + 2-byte ring (clkdiv=4)
//   D  DMA at clkdiv=3        — 96 ns write cycle; compare FPS
//   E  TE-gated DMA fill      — gate frame start on TE rising edge (10 frames)
//
// DMA notes:
//   - DMA_SIZE_8 byte writes to a 32-bit FIFO register: RP2350 replicates the
//     byte into all 4 byte lanes (→ FIFO word = 0xXXXXXXXX). The PIO autopull
//     threshold is 8 bits, shift-right, so bits[7:0] are consumed first — the
//     replicated byte in bits[7:0] is the correct value. ✓
//   - Solid fill uses a 2-byte ring buffer {hi, lo} so no framebuffer is needed.
//   - tft_solid_pix must be 2-byte aligned for ring_size=1 to wrap correctly.
// ---------------------------------------------------------------------------

// 2-byte ring source for solid-colour DMA fills
static uint8_t __attribute__((aligned(2))) tft_solid_pix[2];

// DMA transfer: solid colour via 2-byte ring buffer
// Caller must have called tft_cmd(0x2C) (RAMWR) and have DCX=1 (data phase).
static void tft_dma_solid(int dma_ch, uint16_t colour) {
    tft_solid_pix[0] = (uint8_t)(colour >> 8);
    tft_solid_pix[1] = (uint8_t)(colour & 0xFF);

    dma_channel_config cfg = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_dreq(&cfg, pio_get_dreq(TFT_PIO, tft_sm, true));
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_ring(&cfg, false, 1);  // read ring: 2^1 = 2 bytes

    dma_channel_configure(dma_ch, &cfg,
        (volatile void *)&TFT_PIO->txf[tft_sm],
        tft_solid_pix,
        240 * 320 * 2,
        true);
    dma_channel_wait_for_finish_blocking(dma_ch);
    tft_flush();
}

// Full solid-colour fill via DMA: issues RAMWR then calls tft_dma_solid()
static void tft_fill_dma(int dma_ch, uint16_t colour) {
    tft_set_window(0, 0, 239, 319);
    tft_cmd(0x2C);
    tft_dma_solid(dma_ch, colour);
}

void tft_fast_test(void) {
    printf("\n--- TFT Fast Refresh Test (Step 13) ---\n");

    // --- Hardware init (identical to tft_test) ---
    gpio_init(TFT_nCS_PIN);  gpio_set_dir(TFT_nCS_PIN,  GPIO_OUT); gpio_put(TFT_nCS_PIN,  1);
    gpio_init(TFT_DCX_PIN);  gpio_set_dir(TFT_DCX_PIN,  GPIO_OUT); gpio_put(TFT_DCX_PIN,  1);
    gpio_init(TFT_nRST_PIN); gpio_set_dir(TFT_nRST_PIN, GPIO_OUT); gpio_put(TFT_nRST_PIN, 1);
    gpio_init(TFT_TE_PIN);   gpio_set_dir(TFT_TE_PIN,   GPIO_IN);

    if (!tft_pio_start()) {
        gpio_set_function(TFT_nCS_PIN,  GPIO_FUNC_NULL);
        gpio_set_function(TFT_DCX_PIN,  GPIO_FUNC_NULL);
        gpio_set_function(TFT_nRST_PIN, GPIO_FUNC_NULL);
        return;
    }

    gpio_put(TFT_nRST_PIN, 0); sleep_ms(10);
    gpio_put(TFT_nRST_PIN, 1); sleep_ms(120);
    gpio_put(TFT_nCS_PIN, 0);

    st7789_init();

    // Enable TE output (V-blank only, mode=0).
    // The init sequence does not send TEON — add it here before any TE tests.
    tft_cmd(0x35); tft_dat(0x00);

    bus_b_init();
    lm_write(LM27965_BANKA, 0x16);
    lm_write(LM27965_GP, 0x21);
    bus_b_deinit();

    int dma_ch = dma_claim_unused_channel(true);

    // -------------------------------------------------------------------------
    // Sub-test A — TE pin frequency check
    // -------------------------------------------------------------------------
    printf("\n[A] TE pin frequency (2 s window)...\n");
    {
        uint32_t t_start = to_ms_since_boot(get_absolute_time());
        uint32_t rising  = 0;
        bool prev = gpio_get(TFT_TE_PIN);
        while (to_ms_since_boot(get_absolute_time()) - t_start < 2000) {
            bool cur = gpio_get(TFT_TE_PIN);
            if (cur && !prev) rising++;
            prev = cur;
        }
        float freq = rising / 2.0f;
        printf("  TE rising edges in 2 s: %lu  →  %.1f Hz\n", rising, freq);
        if (rising == 0)
            printf("  WARNING: no TE signal — check TEON command and GPIO22\n");
        else if (freq >= 55.0f && freq <= 65.0f)
            printf("  Result: PASS (~60 Hz)\n");
        else
            printf("  Result: NOTE — outside 55-65 Hz expected range\n");
    }

    // -------------------------------------------------------------------------
    // Sub-test B — Baseline FPS (CPU polling)
    // -------------------------------------------------------------------------
    printf("\n[B] Baseline FPS — CPU polling, clkdiv=%.0f (10 fills)...\n", TFT_CLK_DIV);
    {
        static const uint16_t colours[2] = {0xF800, 0x001F};  // red / blue
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        for (int i = 0; i < 10; i++) tft_fill(colours[i & 1]);
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - t0;
        printf("  10 fills in %lu ms  →  %.2f FPS  (%.2f ms/frame)\n",
               elapsed, 10000.0f / (float)elapsed, elapsed / 10.0f);
        printf("  Theoretical byte rate: %.3f MB/s  (%.0f ns/byte)\n",
               (240.0f * 320 * 2 * 10) / (elapsed / 1000.0f) / 1e6f,
               (float)elapsed * 1e6f / (240.0f * 320 * 2 * 10));
    }

    // -------------------------------------------------------------------------
    // Sub-test C — DMA solid fill, clkdiv=4 (128 ns write cycle)
    // -------------------------------------------------------------------------
    printf("\n[C] DMA solid fill — clkdiv=%.0f, 128 ns cycle (10 fills)...\n", TFT_CLK_DIV);
    {
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        for (int i = 0; i < 10; i++) tft_fill_dma(dma_ch, i & 1 ? 0x07E0 : 0xFFE0);
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - t0;
        printf("  10 fills in %lu ms  →  %.2f FPS  (%.2f ms/frame)\n",
               elapsed, 10000.0f / (float)elapsed, elapsed / 10.0f);
        float byte_rate_mb = (240.0f * 320 * 2 * 10) / (elapsed / 1000.0f) / 1e6f;
        printf("  Byte rate: %.3f MB/s\n", byte_rate_mb);
    }

    // -------------------------------------------------------------------------
    // Sub-test D — DMA solid fill, clkdiv=3 (96 ns write cycle)
    //
    // ST7789VI twc spec: min 66 ns @ 3.3V VDDI; ~100 ns recommended @ 1.8V VDDI.
    // 96 ns is close to the 1.8V recommendation — observe for display glitches.
    // -------------------------------------------------------------------------
    printf("\n[D] DMA solid fill — clkdiv=3, 96 ns cycle (10 fills)...\n");
    printf("  (Watch for pixel glitches — 96 ns is near the 1.8V VDDI limit)\n");
    {
        tft_flush();
        pio_sm_set_clkdiv(TFT_PIO, tft_sm, 3.0f);
        pio_sm_clkdiv_restart(TFT_PIO, tft_sm);

        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        for (int i = 0; i < 10; i++) tft_fill_dma(dma_ch, i & 1 ? 0xF81F : 0x07FF);
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - t0;
        printf("  10 fills in %lu ms  →  %.2f FPS  (%.2f ms/frame)\n",
               elapsed, 10000.0f / (float)elapsed, elapsed / 10.0f);
        float byte_rate_mb = (240.0f * 320 * 2 * 10) / (elapsed / 1000.0f) / 1e6f;
        printf("  Byte rate: %.3f MB/s\n", byte_rate_mb);

        // Restore safe clkdiv
        tft_flush();
        pio_sm_set_clkdiv(TFT_PIO, tft_sm, TFT_CLK_DIV);
        pio_sm_clkdiv_restart(TFT_PIO, tft_sm);
        printf("  clkdiv restored to %.0f\n", TFT_CLK_DIV);
    }

    // -------------------------------------------------------------------------
    // Sub-test E — TE-gated DMA fill (10 frames)
    //
    // Gates each frame transfer to the TE rising edge (start of V-blank).
    // Frame starts in the safe blanking window — no mid-frame tearing.
    // Expected: throughput matches display refresh rate (~60 FPS if transfer
    // completes within one frame; otherwise the effective rate is lower).
    // -------------------------------------------------------------------------
    printf("\n[E] TE-gated DMA fill — 10 frames (wait TE rise before each)...\n");
    if (!gpio_get(TFT_TE_PIN) && true) {  // always run; print skip only if needed
        // Wait for at most 100 ms for first TE edge; skip if no signal
        uint32_t t_wait = to_ms_since_boot(get_absolute_time());
        bool got_te = false;
        bool prev = gpio_get(TFT_TE_PIN);
        while (to_ms_since_boot(get_absolute_time()) - t_wait < 100) {
            bool cur = gpio_get(TFT_TE_PIN);
            if (cur && !prev) { got_te = true; break; }
            prev = cur;
        }
        if (!got_te) {
            printf("  SKIP: no TE signal within 100 ms timeout\n");
            goto fast_cleanup;
        }
    }
    {
        static const uint16_t te_colours[2] = {0xFFFF, 0x0000};
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        for (int i = 0; i < 10; i++) {
            // Wait for TE rising edge (start of V-blank)
            while ( gpio_get(TFT_TE_PIN)) tight_loop_contents();  // wait for low
            while (!gpio_get(TFT_TE_PIN)) tight_loop_contents();  // wait for rise
            tft_fill_dma(dma_ch, te_colours[i & 1]);
        }
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - t0;
        printf("  10 TE-gated frames in %lu ms  →  %.2f FPS\n",
               elapsed, 10000.0f / (float)elapsed);
        if (elapsed <= 10 * 17 + 50)
            printf("  Result: transfer fits within one frame period — tear-free capable\n");
        else
            printf("  Result: transfer exceeds one frame — reduce clkdiv or use partial update\n");
    }

fast_cleanup:
    dma_channel_unclaim(dma_ch);

    bus_b_init();
    lm_write(LM27965_GP, 0x20);
    bus_b_deinit();

    gpio_put(TFT_nCS_PIN, 1);
    tft_pio_stop();
    gpio_set_function(TFT_nCS_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(TFT_DCX_PIN,  GPIO_FUNC_NULL);
    gpio_set_function(TFT_nRST_PIN, GPIO_FUNC_NULL);

    printf("Done\n");
}
