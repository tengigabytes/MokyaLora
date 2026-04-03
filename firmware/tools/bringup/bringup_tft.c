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

    // Colour fill sequence — verify visually on display
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
        sleep_ms(1500);
    }

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
