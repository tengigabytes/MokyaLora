/* psram.c -- APS6404L 8 MB QSPI PSRAM initialisation.
 *
 * Ported from firmware/tools/bringup/bringup_psram.c (Rev A bring-up
 * validated 2026-04-05). Stripped of diagnostic / sweep / probe code;
 * this file contains only the minimum needed to get QMI M1 live so
 * 0x11000000 becomes a readable/writable memory window.
 *
 * All QMI direct-mode transfers live in RAM (__no_inline_not_in_flash_func)
 * because we temporarily unmap XIP while talking to the PSRAM over
 * GPIO0. If instruction fetches landed in flash during that window
 * they would IACCVIOL.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psram.h"

#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/platform.h"

/* Direct SIO register access for GPIO0 — must be usable while XIP is
 * disabled (so we avoid any helper that might touch flash code paths).
 * RP2350 SIO offsets differ from RP2040 due to interleaved GPIO_HI_*
 * registers; addresses confirmed against docs/bringup/rev-a-bringup-log.md
 * §9 PSRAM init sequence. */
#define GPIO0_CTRL_REG      (*(volatile uint32_t *)0x40028004u)
#define SIO_GPIO_OUT_SET    (*(volatile uint32_t *)0xD0000018u)
#define SIO_GPIO_OUT_CLR    (*(volatile uint32_t *)0xD0000020u)
#define SIO_GPIO_OE_SET     (*(volatile uint32_t *)0xD0000038u)
#define GPIO0_BIT           (1u << 0)
#define PADS_BANK0_GPIO0    (*(volatile uint32_t *)0x40038004u)

volatile uint8_t g_psram_read_id[8] = {0};

/* ── Low-level QMI direct-mode helpers (RAM-resident) ────────────────── */

static bool __no_inline_not_in_flash_func(qmi_wait_busy)(void) {
    for (uint32_t t = 125000; t; --t) {
        if (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)) return true;
    }
    return false;
}

/* TX one byte on single-wire, OE=1, NOPUSH=1 (no RX capture). */
static bool __no_inline_not_in_flash_func(cs1_tx)(uint8_t b) {
    qmi_hw->direct_tx = (1u << 20) | (1u << 19) | b;  /* NOPUSH | OE | data */
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS));
    return qmi_wait_busy();
}

/* TX one byte in quad mode, OE=1, NOPUSH=1. Used for QPI warm-reset. */
static bool __no_inline_not_in_flash_func(cs1_tx_qpi)(uint8_t b) {
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | QMI_DIRECT_TX_OE_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | b;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS));
    return qmi_wait_busy();
}

/* RX one byte (send dummy, capture). */
static bool __no_inline_not_in_flash_func(cs1_rx)(uint8_t *out) {
    qmi_hw->direct_tx = (1u << 19) | 0x00u;  /* OE | dummy */
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS));
    if (!qmi_wait_busy()) { *out = 0xEE; return false; }
    *out = (uint8_t)qmi_hw->direct_rx;
    return true;
}

/* ── psram_init_run: full sequence, called with IRQs disabled ────────── */

struct psram_init_result {
    uint8_t  id[8];
    bool     id_ok;
    bool     busy_timeout;
};

static void __no_inline_not_in_flash_func(psram_init_run)(
        struct psram_init_result *r) {
    r->busy_timeout = false;
    r->id_ok = false;
    bool ok = true;

    /* Phase 0: GPIO0 = SIO output HIGH + pull-up. GPIO0 boots with
     * PDE=1 (pull-down) which drives CS# LOW causing bus contention;
     * override before QMI touches anything. */
    SIO_GPIO_OUT_SET = GPIO0_BIT;
    SIO_GPIO_OE_SET  = GPIO0_BIT;
    GPIO0_CTRL_REG   = 5u;                                 /* FUNCSEL=SIO */
    PADS_BANK0_GPIO0 = (1u << 6) | (1u << 4) | (1u << 3);  /* IE=1 DRIVE=4mA PUE=1 */

    for (volatile uint32_t i = 0; i < 1000; i++) __asm volatile ("nop");

    /* Phase 1: enter QMI direct mode and assert CS1N for clock. */
    uint32_t irq_save = save_and_disable_interrupts();
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    #define CS_INIT_LOW()  (SIO_GPIO_OUT_CLR = GPIO0_BIT)
    #define CS_INIT_HIGH() (SIO_GPIO_OUT_SET = GPIO0_BIT)

    /* QPI warm-reset: device may already be in QPI mode from a prior
     * boot (PSRAM keeps mode across soft reset). Issue reset-enable/
     * reset in quad mode before attempting SPI commands. */
    CS_INIT_LOW(); cs1_tx_qpi(0x66); CS_INIT_HIGH();
    CS_INIT_LOW(); cs1_tx_qpi(0x99); CS_INIT_HIGH();
    for (volatile uint32_t i = 0; i < 12500; i++) __asm volatile ("nop");

    /* SPI reset (device now in single-wire SPI mode). */
    CS_INIT_LOW(); ok = cs1_tx(0x66); CS_INIT_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    CS_INIT_LOW(); ok = cs1_tx(0x99); CS_INIT_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }
    for (volatile uint32_t i = 0; i < 12500; i++) __asm volatile ("nop");

    /* Read ID (0x9F + 3 address bytes + 8 data bytes). */
    CS_INIT_LOW();
    ok = cs1_tx(0x9F) && cs1_tx(0x00) && cs1_tx(0x00) && cs1_tx(0x00);
    if (ok) {
        for (int i = 0; i < 8; i++)
            if (!cs1_rx(&r->id[i])) ok = false;
    }
    CS_INIT_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    r->id_ok = (r->id[0] == MOKYA_PSRAM_MFID && r->id[1] == MOKYA_PSRAM_KGD);

    /* Enter QPI (0x35). From here all commands must be quad-width. */
    CS_INIT_LOW(); ok = cs1_tx(0x35); CS_INIT_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    /* Phase 2: configure M1 timing / rfmt / wfmt.
     *
     * Rev A bring-up validated CLKDIV=2 (37.5 MHz) with RXDELAY=0 and
     * COOLDOWN=2 across both boards in the lab. CLKDIV=1 worked on
     * board #2 but not #1 — stay conservative in production firmware.
     * COOLDOWN=2 satisfies APS6404L tCPH ≥ 18 ns.
     *
     * MAX_SELECT=1 (P2-15): required for APS6404L DRAM refresh. Inheriting
     * M0's MAX_SELECT=0 lets QMI hold CS asserted indefinitely across
     * back-to-back M1 accesses, starving PSRAM's self-refresh (datasheet
     * tCEM=8µs standard grade). MAX_SELECT=1 triggers CS deassertion
     * every 64 sys_clk (~427ns) + ≤316 cycle burst = 3.8µs total,
     * comfortably under tCEM. Validated full 8 MB cached round-trip:
     * 0 / 2,097,152 errors × 2 passes. Without the fix: 46.81% cached /
     * 93.75% uncached err. See docs/bringup/phase2-log.md §P2-15.
     *
     * rfmt: cmd=Q/8b, addr=Q, dummy=Q/28 bits (7 quad clocks), data=Q.
     * APS6404L 0xEB QPI datasheet says 6 wait cycles, but empirically
     * requires 7 — likely because the mode byte slot (occupied in SPI
     * mode) leaves 4+1 dummy clocks needed for QPI XIP without the
     * mode byte.
     */
    qmi_hw->m[1].timing = (qmi_hw->m[0].timing &
                            ~(QMI_M1_TIMING_COOLDOWN_BITS    |
                              QMI_M1_TIMING_MAX_SELECT_BITS  |
                              QMI_M1_TIMING_RXDELAY_BITS     |
                              QMI_M1_TIMING_CLKDIV_BITS))
                         | (2u << QMI_M1_TIMING_COOLDOWN_LSB)
                         | (1u << QMI_M1_TIMING_MAX_SELECT_LSB)
                         | (0u << QMI_M1_TIMING_RXDELAY_LSB)
                         | (2u << QMI_M1_TIMING_CLKDIV_LSB);

    qmi_hw->m[1].rfmt = (QMI_M1_RFMT_PREFIX_LEN_VALUE_8  << QMI_M1_RFMT_PREFIX_LEN_LSB)  |
                        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
                        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_RFMT_ADDR_WIDTH_LSB)   |
                        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M1_RFMT_DUMMY_WIDTH_LSB)  |
                        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_RFMT_DATA_WIDTH_LSB)   |
                        (QMI_M1_RFMT_DUMMY_LEN_VALUE_28   << QMI_M1_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[1].rcmd = 0xEBu;  /* QPI Fast Read */

    qmi_hw->m[1].wfmt = (QMI_M1_WFMT_PREFIX_LEN_VALUE_8  << QMI_M1_WFMT_PREFIX_LEN_LSB)  |
                        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
                        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_WFMT_ADDR_WIDTH_LSB)   |
                        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_WFMT_DATA_WIDTH_LSB);
    qmi_hw->m[1].wcmd = 0x38u;  /* QPI Write */

    /* Phase 3: switch GPIO0 to XIP_CS1 while still in direct mode —
     * M1 is paused, so GPIO0 stays stable until we exit direct mode. */
    GPIO0_CTRL_REG = 9u;  /* FUNCSEL = XIP_CS1 */

exit:
    CS_INIT_HIGH();
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(irq_save);

    #undef CS_INIT_LOW
    #undef CS_INIT_HIGH
}

/* ── Public entry ────────────────────────────────────────────────────── */

bool psram_init(void) {
    struct psram_init_result r;
    psram_init_run(&r);

    for (int i = 0; i < 8; ++i) g_psram_read_id[i] = r.id[i];

    if (r.id_ok) {
        /* Enable writable M1 window so writes via 0x15000000+ land on
         * PSRAM. Cached access at 0x11000000 remains read-only (writes
         * would go through the XIP cache, and PSRAM cache coherency is
         * not enforced by hardware). */
        hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    }
    return r.id_ok;
}
