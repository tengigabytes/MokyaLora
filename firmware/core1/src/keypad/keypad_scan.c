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
#include "keymap_matrix.h"
#include "key_event.h"

#include "hardware/pio.h"
#include "hardware/dma.h"

#include "FreeRTOS.h"
#include "task.h"

#include "mokya_trace.h"

volatile uint8_t  g_kp_snapshot[KEY_ROWS];
volatile uint8_t  g_kp_stable[KEY_ROWS];
volatile uint32_t g_kp_scan_tick;

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

/* ── Phase B — per-key debounce + keymap + KeyEvent enqueue ─────────── *
 *
 * Task period:         5 ms   (vTaskDelay granularity)
 * Stability threshold: 4 ticks = 20 ms window
 *
 * Each of the 36 keys carries an independent debounce FSM with three
 * bytes of state:
 *   stable[r][c]  — currently committed state (0 = released, 1 = pressed)
 *   pending[r][c] — the candidate new state we are validating
 *   count[r][c]   — number of consecutive scans where the raw reading
 *                   has equalled pending; commit when count >= threshold
 *
 * On commit: update g_kp_stable, translate through g_keymap, push an
 * event into the KeyEvent queue (KEY_SOURCE_HW). Drops from a full
 * queue are counted in g_key_event_dropped but never stall the scan.
 *
 * Phase 1.4 — slot keys (the 20 SmartZh half-keys) defer their press
 * emission so the engine can carry a long-press flag at press time:
 *
 *   short tap (release < 500 ms)  → fire press(flags=0) on release,
 *                                    immediately followed by release.
 *                                    Engine treats as fuzzy half-keyboard.
 *   long  tap (held ≥ 500 ms)     → fire press(flags=LONG_PRESS) at
 *                                    the 500 ms mark; subsequent release
 *                                    fires release as normal. Engine
 *                                    filters strictly to the secondary
 *                                    phoneme.
 *
 * Non-slot keys (FUNC, SYM1/2, OK, DPAD, MODE, BACK, DEL, SET, TAB, SPACE,
 * VOL_UP/DOWN) keep the original press-on-press / release-on-release
 * behaviour — SYM1's own long-press handler in MIE expects to see the
 * raw press immediately, and DPAD/OK are latency-sensitive.
 */
#define KP_SCAN_PERIOD_MS       5u
#define KP_DEBOUNCE_THRESHOLD   4u
#define KP_LONG_PRESS_MS      500u
#define KP_LONG_PRESS_TICKS  (KP_LONG_PRESS_MS / KP_SCAN_PERIOD_MS)

/* Returns true if the given keycode should defer its press event for
 * long-press disambiguation. Mirrors mie/src/ime_keys.cpp::kKeyTable —
 * the 20 SmartZh half-keys whose primary/secondary/tertiary phoneme
 * choice depends on hold duration. */
static bool keycode_defers_press(mokya_keycode_t kc)
{
    switch (kc) {
        case MOKYA_KEY_1: case MOKYA_KEY_3: case MOKYA_KEY_5:
        case MOKYA_KEY_7: case MOKYA_KEY_9:
        case MOKYA_KEY_Q: case MOKYA_KEY_E: case MOKYA_KEY_T:
        case MOKYA_KEY_U: case MOKYA_KEY_O:
        case MOKYA_KEY_A: case MOKYA_KEY_D: case MOKYA_KEY_G:
        case MOKYA_KEY_J: case MOKYA_KEY_L:
        case MOKYA_KEY_Z: case MOKYA_KEY_C: case MOKYA_KEY_B:
        case MOKYA_KEY_M: case MOKYA_KEY_BACKSLASH:
            return true;
        default:
            return false;
    }
}

/* Per-slot-key deferred-press FSM. */
typedef enum {
    LP_IDLE = 0,           /* nothing pending */
    LP_PRESS_PENDING,      /* debounce committed press, waiting for release
                              or long-press tick */
    LP_LONG_EMITTED,       /* long-press already fired; awaiting release */
} lp_state_t;

void keypad_scan_task(void *pv)
{
    (void)pv;

    /* 36-byte per-key state — file-static scope to keep scan-task stack
     * small. Zero-initialised via BSS. */
    static uint8_t stable[KEY_ROWS][KEY_COLS];
    static uint8_t pending[KEY_ROWS][KEY_COLS];
    static uint8_t count[KEY_ROWS][KEY_COLS];

    /* Long-press FSM state, also indexed by (row, col). */
    static lp_state_t lp_state[KEY_ROWS][KEY_COLS];
    static uint32_t   lp_press_tick[KEY_ROWS][KEY_COLS];

    uint8_t raw[KEY_ROWS];

    keypad_init();

    const TickType_t period = pdMS_TO_TICKS(KP_SCAN_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        keypad_read(raw);

        const uint32_t now_tick = g_kp_scan_tick;

        for (uint32_t r = 0; r < KEY_ROWS; r++) {
            g_kp_snapshot[r] = raw[r];

            uint8_t stable_row = g_kp_stable[r];
            for (uint32_t c = 0; c < KEY_COLS; c++) {
                const uint8_t bit     = (uint8_t)((raw[r] >> c) & 1u);
                const uint8_t stbl    = stable[r][c];

                if (bit != stbl) {
                    /* Differs from committed state: track a candidate. */
                    if (bit == pending[r][c]) {
                        if (count[r][c] < 0xFFu) count[r][c]++;
                    } else {
                        pending[r][c] = bit;
                        count[r][c]   = 1u;
                    }
                } else {
                    /* Matches committed state — drop any pending change. */
                    count[r][c]   = 0u;
                    pending[r][c] = stbl;
                }

                if (bit != stbl && count[r][c] >= KP_DEBOUNCE_THRESHOLD) {
                    /* Commit the new state. */
                    stable[r][c] = bit;
                    count[r][c]  = 0u;
                    if (bit) {
                        stable_row |= (uint8_t)(1u << c);
                    } else {
                        stable_row &= (uint8_t)~(1u << c);
                    }

                    const mokya_keycode_t kc = g_keymap[r][c];
                    if (kc == MOKYA_KEY_NONE) continue;

                    if (!keycode_defers_press(kc)) {
                        /* Non-slot key: emit immediately as before. */
                        (void)key_event_push_hw(kc, bit != 0u);
                        TRACE("kpad", "commit", "kc=0x%02x,p=%u",
                              (unsigned)kc, (unsigned)bit);
                        continue;
                    }

                    /* Slot key: drive the deferred-press FSM. */
                    if (bit) {
                        /* Fresh press — start the long-press timer.
                         * Do NOT enqueue the press yet. */
                        lp_state[r][c]      = LP_PRESS_PENDING;
                        lp_press_tick[r][c] = now_tick;
                        TRACE("kpad", "lp_down", "kc=0x%02x", (unsigned)kc);
                    } else {
                        /* Release transition. */
                        switch (lp_state[r][c]) {
                        case LP_PRESS_PENDING:
                            /* Short tap — emit press(flags=0) then release. */
                            (void)key_event_push_hw_flags(kc, true,  0u);
                            (void)key_event_push_hw_flags(kc, false, 0u);
                            TRACE("kpad", "lp_short", "kc=0x%02x", (unsigned)kc);
                            break;
                        case LP_LONG_EMITTED:
                            /* Long press already fired its press — just
                             * release, with the same flag bit so the
                             * arbitration / log shows the long-press pair. */
                            (void)key_event_push_hw_flags(kc, false,
                                                          MOKYA_KEY_FLAG_LONG_PRESS);
                            TRACE("kpad", "lp_long_up", "kc=0x%02x",
                                  (unsigned)kc);
                            break;
                        default:
                            /* Spurious release with no recorded press —
                             * still emit a release so consumers stay
                             * symmetric. */
                            (void)key_event_push_hw_flags(kc, false, 0u);
                            break;
                        }
                        lp_state[r][c] = LP_IDLE;
                    }
                }
            }
            g_kp_stable[r] = stable_row;
        }

        /* Long-press timer evaluation, after the row pass so the FSM
         * sees the latest committed state. Walks all 36 keys — cheap
         * (one tick comparison each). */
        for (uint32_t r = 0; r < KEY_ROWS; r++) {
            for (uint32_t c = 0; c < KEY_COLS; c++) {
                const mokya_keycode_t kc = g_keymap[r][c];
                if (kc == MOKYA_KEY_NONE || !keycode_defers_press(kc)) continue;

                const uint32_t held = now_tick - lp_press_tick[r][c];
                if (lp_state[r][c] == LP_PRESS_PENDING &&
                    held >= KP_LONG_PRESS_TICKS) {
                    (void)key_event_push_hw_flags(kc, true,
                                                  MOKYA_KEY_FLAG_LONG_PRESS);
                    lp_state[r][c] = LP_LONG_EMITTED;
                    TRACE("kpad", "lp_long", "kc=0x%02x", (unsigned)kc);
                }
            }
        }

        g_kp_scan_tick++;
        vTaskDelayUntil(&last_wake, period);
    }
}
