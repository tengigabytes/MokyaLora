/* keypad_scan.c — PIO + DMA implementation of the 6x6 keypad scanner.
 *
 * Architecture. PIO0 runs keypad_scan_program in a tight loop. Two DMA
 * channels keep it fed:
 *   - TX DMA copies 8 bytes of row masks from RAM into the PIO TX FIFO,
 *     paced by PIO's TX DREQ. Read address wraps at the 8-byte boundary
 *     (channel_config_set_ring), so the masks loop forever.
 *   - RX DMA copies the 8 PIO RX FIFO words into an 8-byte result ring
 *     in RAM. Write address wraps at the 8-byte boundary so the ring
 *     mirrors the mask table: s_rx_ring[r] is the result of mask[r].
 *
 * The DMA transfer_count is set to UINT32_MAX. At sys_clk=150 MHz /
 * CLKDIV=150 / 20 PIO cycles per row, that gives ~5.8 days of
 * continuous scanning before the channel exhausts and has to be
 * restarted. Chaining can be added later if uptime demands exceed that;
 * Phase A-DMA does not wire a watchdog restart.
 *
 * Row masks. 6 active + 2 dummy, padded to 8 bytes so DMA ring mode
 * (ring_size_bits = 3) wraps cleanly:
 *   [0..5] = 0x3E, 0x3D, 0x3B, 0x37, 0x2F, 0x1F   (one row LOW, others HIGH)
 *   [6..7] = 0x3F, 0x3F                            (all HIGH, no selection)
 *
 * The dummy entries exercise the scan loop without selecting any row,
 * so s_rx_ring[6..7] always read 0x3F (all cols HIGH via pull-up). They
 * are ignored by keypad_read().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "keypad_scan.h"
#include "keypad_scan.pio.h"

#include "hardware/pio.h"
#include "hardware/dma.h"

volatile uint8_t g_kp_snapshot[KEY_ROWS];

/* PIO0 — PIO1 is owned by the display driver. */
#define KEY_PIO           pio0

/* At sys_clk=150 MHz with CLKDIV=150, PIO runs at 1 MHz -> 1 us per cycle.
 * Program's nop [15] gives 16 us per-row settling, matching the bringup
 * polling scanner's 10 us with margin. */
#define KEY_PIO_CLKDIV    150.0f

/* 8-entry TX ring: 6 real row masks + 2 dummies. 8-byte aligned so DMA
 * ring_size_bits = 3 wraps the read address cleanly. Lives in flash. */
static const uint8_t s_row_masks[8] __attribute__((aligned(8))) = {
    0x3E, /* row 0: 0b111110 */
    0x3D, /* row 1: 0b111101 */
    0x3B, /* row 2: 0b111011 */
    0x37, /* row 3: 0b110111 */
    0x2F, /* row 4: 0b101111 */
    0x1F, /* row 5: 0b011111 */
    0x3F, /* dummy — no row selected */
    0x3F, /* dummy — no row selected */
};

/* RX ring mirrors the TX ring; RX DMA writes here at the same cadence
 * the PIO produces results. Aligned + power-of-two so DMA ring wrap works. */
static volatile uint8_t s_rx_ring[8] __attribute__((aligned(8)));

static uint s_sm;
static uint s_pio_offset;
static int  s_tx_dma;
static int  s_rx_dma;

void keypad_init(void)
{
    /* PIO can only address 32 consecutive GPIOs at a time. The keypad lives
     * on GPIO 36-47, so switch the PIO GPIO base from the default 0 to 16
     * (addressing window becomes 16-47). MUST be set before pio_sm_init
     * or the SDK rejects the config with PICO_ERROR_BAD_ALIGNMENT. */
    pio_set_gpio_base(KEY_PIO, 16);

    s_sm = (uint)pio_claim_unused_sm(KEY_PIO, true);
    s_pio_offset = pio_add_program(KEY_PIO, &keypad_scan_program);
    keypad_scan_program_init(KEY_PIO, s_sm, s_pio_offset,
                             KEY_ROW_BASE, KEY_COL_BASE, KEY_PIO_CLKDIV);

    s_tx_dma = dma_claim_unused_channel(true);
    s_rx_dma = dma_claim_unused_channel(true);

    /* TX DMA: stream s_row_masks[] into the PIO TX FIFO, forever.
     *   - 8-bit transfers, one byte per push
     *   - Read increments, write fixed at PIO TX FIFO
     *   - Read wraps at 8-byte boundary (ring_size_bits = 3)
     *   - Paced by PIO TX DREQ (stalls when FIFO full) */
    {
        dma_channel_config cfg = dma_channel_get_default_config(s_tx_dma);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, false);
        channel_config_set_dreq(&cfg, pio_get_dreq(KEY_PIO, s_sm, true));
        channel_config_set_ring(&cfg, false, 3); /* read wraps at 2^3 = 8 B */
        dma_channel_configure(s_tx_dma, &cfg,
                              &KEY_PIO->txf[s_sm],
                              s_row_masks,
                              0xFFFFFFFFu,
                              false);
    }

    /* RX DMA: drain PIO RX FIFO into s_rx_ring[].
     *   - 8-bit transfers: DMA pops the 32-bit FIFO word, keeps the low
     *     byte (col[5:0] landed in the LSBs via ISR shift-left)
     *   - Read fixed at PIO RX FIFO, write increments
     *   - Write wraps at 8-byte boundary (ring_size_bits = 3)
     *   - Paced by PIO RX DREQ (stalls when FIFO empty) */
    {
        dma_channel_config cfg = dma_channel_get_default_config(s_rx_dma);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
        channel_config_set_read_increment(&cfg, false);
        channel_config_set_write_increment(&cfg, true);
        channel_config_set_dreq(&cfg, pio_get_dreq(KEY_PIO, s_sm, false));
        channel_config_set_ring(&cfg, true, 3); /* write wraps at 2^3 = 8 B */
        dma_channel_configure(s_rx_dma, &cfg,
                              s_rx_ring,
                              &KEY_PIO->rxf[s_sm],
                              0xFFFFFFFFu,
                              false);
    }

    /* Start both DMAs in a single atomic write so neither runs ahead of
     * the other, then release the PIO SM to begin executing. */
    dma_start_channel_mask((1u << s_tx_dma) | (1u << s_rx_dma));
    pio_sm_set_enabled(KEY_PIO, s_sm, true);
}

void keypad_read(volatile uint8_t out_state[KEY_ROWS])
{
    /* Raw ring holds active-low readings (0 bit = pressed). Invert and
     * mask to 6 bits. DMA may be updating any single byte of the ring
     * during this read; the other 5 bytes are guaranteed stable for at
     * least one scan period (~120 us), so the worst case is one row
     * being stale by a single scan. Phase B debounce absorbs that. */
    for (uint32_t r = 0; r < KEY_ROWS; r++) {
        out_state[r] = (uint8_t)((~s_rx_ring[r]) & 0x3Fu);
    }
}
