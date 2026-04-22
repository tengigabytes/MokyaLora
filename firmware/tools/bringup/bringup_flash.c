#include "bringup.h"
#include "hardware/clocks.h"
#include "hardware/xip_cache.h"
#include "hardware/regs/pads_qspi.h"
#include "hardware/structs/pads_qspi.h"

// ---------------------------------------------------------------------------
// Flash test (W25Q128JW, 16 MB)
// Expected JEDEC ID: MFR=0xEF, Type=0x60, Cap=0x18
// ---------------------------------------------------------------------------

void flash_test(void) {
    printf("\n--- Flash test (W25Q128JW) ---\n");

    uint8_t txbuf[4] = {0x9F, 0, 0, 0};
    uint8_t rxbuf[4] = {0};
    flash_do_cmd(txbuf, rxbuf, 4);
    uint8_t mfr  = rxbuf[1];
    uint8_t type = rxbuf[2];
    uint8_t cap  = rxbuf[3];

    printf("  JEDEC ID  : %02X %02X %02X\n", mfr, type, cap);
    // W25Q128JW (1.8 V): MFR=0xEF, Type=0x60, Cap=0x18
    // Cap byte = log2(size_bytes); size_MB = 1 << (cap - 20)
    printf("  MFR       : %s\n", mfr == 0xEF ? "Winbond (OK)" : "UNKNOWN");
    uint32_t size_mb = (cap >= 20) ? (1u << (cap - 20)) : 0;
    printf("  Capacity  : %u MB %s\n", (unsigned)size_mb,
           size_mb == 16 ? "(OK)" : "(unexpected)");

    uint8_t uid[8];
    flash_get_unique_id(uid);
    printf("  Unique ID : %02X%02X%02X%02X%02X%02X%02X%02X\n",
           uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7]);

    bool pass = (mfr == 0xEF) && (type == 0x60) && (cap == 0x18);
    printf("  Result    : %s\n", pass ? "PASS" : "FAIL");

    // Status registers — SR2 bit1 = QE (Quad Enable); must be set for QSPI IO2/IO3
    uint8_t sr_tx[2], sr_rx[2];
    sr_tx[0] = 0x05; sr_tx[1] = 0; flash_do_cmd(sr_tx, sr_rx, 2); uint8_t sr1 = sr_rx[1];
    sr_tx[0] = 0x35; sr_tx[1] = 0; flash_do_cmd(sr_tx, sr_rx, 2); uint8_t sr2 = sr_rx[1];
    sr_tx[0] = 0x15; sr_tx[1] = 0; flash_do_cmd(sr_tx, sr_rx, 2); uint8_t sr3 = sr_rx[1];
    printf("  SR1=0x%02X SR2=0x%02X SR3=0x%02X\n", sr1, sr2, sr3);
    printf("  QE (Quad Enable / IO2-IO3 free): %s\n",
           (sr2 & 0x02) ? "SET — IO2/IO3 are QSPI data" : "CLEAR — IO2=WP# IO3=HOLD# (pulled)");

    // XIP dump — first 32 bytes of flash contents (no flash_do_cmd needed)
    volatile uint8_t *fx = (volatile uint8_t *)XIP_BASE;
    printf("  Flash[0..31]:");
    for (int i = 0; i < 32; i++) {
        if (i % 16 == 0) printf("\n    ");
        printf("%02X ", fx[i]);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Flash (M0) XIP speed test — change M0 timing from RAM, test Flash reads.
//
// This function runs entirely from RAM (no_inline_not_in_flash) so that XIP
// can be safely paused while M0.timing is changed.  It reads back a known
// Flash constant after the change to verify correctness.
// ---------------------------------------------------------------------------

// Known sentinel in Flash — address and value set by the linker.
// We use the first word of the vector table (Initial MSP) which is always
// 0x20082000 for RP2350B with 520 KB SRAM.
//
// Use the UNCACHED alias so the read bypasses the XIP cache. With cache
// enabled, a cached read at a broken candidate timing triggers an async
// cache-line fill that can hang the CPU; uncached forces a single-beat
// read that either returns garbage or satisfies BUSY within our bench.
#define FLASH_TEST_ADDR   (XIP_NOCACHE_NOALLOC_BASE)
#define FLASH_TEST_EXPECT (*(volatile uint32_t *)FLASH_TEST_ADDR)

void __no_inline_not_in_flash_func(flash_speed_run)(
        struct flash_speed_result *results, int count,
        const uint8_t *clkdivs, const uint8_t *rxdelays,
        uint32_t sys_hz) {
    // Read expected value BEFORE changing timing (while XIP still works)
    uint32_t expected = *(volatile uint32_t *)FLASH_TEST_ADDR;

    for (int i = 0; i < count; i++) {
        uint8_t cd = clkdivs[i];
        uint8_t rd = rxdelays[i];

        // Pause XIP
        uint32_t irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);

        // Save and change M0 timing
        uint32_t orig = qmi_hw->m[0].timing;
        qmi_hw->m[0].timing = (orig & ~(QMI_M0_TIMING_CLKDIV_BITS |
                                         QMI_M0_TIMING_RXDELAY_BITS))
                             | ((uint32_t)cd << QMI_M0_TIMING_CLKDIV_LSB)
                             | ((uint32_t)rd << QMI_M0_TIMING_RXDELAY_LSB);

        // Resume XIP with new timing
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);

        // Read test word from Flash (uses new timing)
        uint32_t val = *(volatile uint32_t *)FLASH_TEST_ADDR;

        // Throughput bench: read FLASH_BENCH_WORDS from uncached alias,
        // XOR-accumulate to prevent compiler eliding the loop. Uncached
        // forces every read to go through QMI so we measure raw bus
        // bandwidth at this timing, not cache effects.
        uint32_t bench_us = 0;
        uint32_t bench_kbps = 0;
        if (val == expected) {
            volatile uint32_t *src =
                (volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE);
            uint32_t acc = 0;
            uint32_t t0 = timer_hw->timerawl;
            for (uint32_t n = 0; n < FLASH_BENCH_WORDS; n++)
                acc ^= src[n];
            uint32_t t1 = timer_hw->timerawl;
            (void)acc;
            bench_us = t1 - t0;
            if (bench_us)
                bench_kbps = (FLASH_BENCH_WORDS * 4u * 1000u) / bench_us;
        }

        // Revert to original timing immediately (from RAM — safe even if
        // the read above returned garbage, because we are still in RAM)
        irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing = orig;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);

        results[i].clkdiv  = cd;
        results[i].rxdelay = rd;
        results[i].sck_mhz = sys_hz / (2u * cd) / 1000000u;
        results[i].read_val = val;
        results[i].expected = expected;
        results[i].pass     = (val == expected);
        results[i].bench_us = bench_us;
        results[i].bench_kbps = bench_kbps;
    }
}

/* ---------------------------------------------------------------------
 * flash_bench — measure current M0 read throughput, both uncached
 * (direct QMI per-word) and cached (XIP cache-line bursts from same
 * 64 KB offset). Pure read, no config change. Runs from RAM to avoid
 * self-timing contamination.
 * ------------------------------------------------------------------- */
#define FLASH_BENCH_ADDR   XIP_NOCACHE_NOALLOC_BASE
#define FLASH_BENCH_CACHED XIP_BASE
#define FLASH_BENCH_SIZE   (FLASH_BENCH_WORDS * 4u)

static uint32_t __no_inline_not_in_flash_func(flash_bench_uncached)(void) {
    volatile uint32_t *src = (volatile uint32_t *)FLASH_BENCH_ADDR;
    uint32_t acc = 0;
    uint32_t t0 = timer_hw->timerawl;
    for (uint32_t n = 0; n < FLASH_BENCH_WORDS; n++)
        acc ^= src[n];
    uint32_t us = timer_hw->timerawl - t0;
    (void)acc;
    return us;
}

static uint32_t __no_inline_not_in_flash_func(flash_bench_cached)(void) {
    xip_cache_invalidate_range(0, FLASH_BENCH_SIZE);
    volatile uint32_t *src = (volatile uint32_t *)FLASH_BENCH_CACHED;
    uint32_t acc = 0;
    uint32_t t0 = timer_hw->timerawl;
    for (uint32_t n = 0; n < FLASH_BENCH_WORDS; n++)
        acc ^= src[n];
    uint32_t us = timer_hw->timerawl - t0;
    (void)acc;
    return us;
}

void flash_bench(void) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t cd = qmi_hw->m[0].timing & 0xFFu;
    uint32_t rd = (qmi_hw->m[0].timing >> 8) & 0x7u;
    uint32_t sck_mhz = cd ? (sys_hz / (2u * cd) / 1000000u) : 0;

    printf("\n--- Flash M0 bench (64 KB) ---\n");
    printf("  M0.timing=0x%08X  CLKDIV=%u (SCK %u MHz)  RXDELAY=%u\n",
           (unsigned)qmi_hw->m[0].timing, (unsigned)cd,
           (unsigned)sck_mhz, (unsigned)rd);
    printf("  M0.rfmt=0x%08X  M0.rcmd=0x%08X  XIP_CTRL=0x%08X\n",
           (unsigned)qmi_hw->m[0].rfmt,
           (unsigned)qmi_hw->m[0].rcmd,
           (unsigned)xip_ctrl_hw->ctrl);

    uint32_t u_us = flash_bench_uncached();
    uint32_t c_us = flash_bench_cached();
    uint32_t u_kbps = u_us ? (FLASH_BENCH_SIZE * 1000u / u_us) : 0;
    uint32_t c_kbps = c_us ? (FLASH_BENCH_SIZE * 1000u / c_us) : 0;
    printf("  uncached 64 KB: %u us = %u KB/s\n", (unsigned)u_us, (unsigned)u_kbps);
    printf("  cached   64 KB: %u us = %u KB/s  (cold miss -> fill -> reuse)\n",
           (unsigned)c_us, (unsigned)c_kbps);
}

/* ---------------------------------------------------------------------
 * flash_sweep2 — sweep CLKDIV x RXDELAY with Pico SDK boot2_w25q080
 * style rfmt: 1-4-4, SUFFIX M-byte 0xA0 (continuous-read-disabled
 * sentinel, per Pico SDK convention), 4-clock dummy after M-byte.
 *
 * Runs entirely from RAM so instruction fetch can't fault when we
 * touch M0 mid-transaction. For each (cd, rd) candidate:
 *   1. Enter direct mode (XIP paused).
 *   2. Write new M0 timing + rfmt + rcmd.
 *   3. Exit direct mode. XIP resumes at new settings.
 *   4. Read MSP sentinel (correctness) + 64 KB bench (throughput).
 *   5. Reset M0 to original inside direct mode.
 *
 * The M-byte bit flip lets us see whether the extra 2 clocks of
 * suffix matter on top of the missing ones that flash_sweep did
 * without. Cached throughput is measured via cache_invalidate + fresh
 * reads through XIP_BASE, so every pass is a cold-cache bench.
 * ------------------------------------------------------------------- */
struct flash_sweep2_result {
    uint8_t  clkdiv, rxdelay;
    uint32_t sck_mhz;
    uint32_t read_val, expected;
    bool     pass;
    uint32_t u_us, u_kbps;  /* uncached 64 KB */
    uint32_t c_us, c_kbps;  /* cached 64 KB (cold fill) */
};

void __no_inline_not_in_flash_func(flash_sweep2_run)(
        struct flash_sweep2_result *res, int count,
        const uint8_t *cdv, const uint8_t *rdv, uint32_t sys_hz) {
    uint32_t expected = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;

    const uint32_t SWEEP_RFMT =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_S << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB)   |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB)   |
        (QMI_M0_RFMT_SUFFIX_LEN_VALUE_8   << QMI_M0_RFMT_SUFFIX_LEN_LSB)   |
        (4u /* DUMMY_LEN_VALUE_16 = 4 Q-clocks */
                                          << QMI_M0_RFMT_DUMMY_LEN_LSB);
    /* rcmd: prefix = 0xEB (Fast Read Quad I/O), suffix = 0xA0 (M-byte
     * value with M[7:4] != 0xA so continuous-read mode is DISABLED —
     * device still needs the M-byte bits but treats next access as
     * fresh). Pico SDK boot2_w25q080 uses the same convention. */
    const uint32_t SWEEP_RCMD =
        (0xEBu << QMI_M0_RCMD_PREFIX_LSB) |
        (0xA0u << QMI_M0_RCMD_SUFFIX_LSB);

    uint32_t orig_timing = qmi_hw->m[0].timing;
    uint32_t orig_rfmt   = qmi_hw->m[0].rfmt;
    uint32_t orig_rcmd   = qmi_hw->m[0].rcmd;

    for (int i = 0; i < count; i++) {
        uint8_t cd = cdv[i];
        uint8_t rd = rdv[i];

        /* Program new timing/rfmt/rcmd under direct mode. */
        uint32_t irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing =
            (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
            ((uint32_t)rd << QMI_M0_TIMING_RXDELAY_LSB) |
            ((uint32_t)cd << QMI_M0_TIMING_CLKDIV_LSB);
        qmi_hw->m[0].rfmt = SWEEP_RFMT;
        qmi_hw->m[0].rcmd = SWEEP_RCMD;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);

        /* Correctness: read MSP sentinel via UNCACHED to avoid cache
         * masking. If bad timing, we may see garbage here. */
        uint32_t val = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;

        uint32_t u_us = 0, u_kbps = 0, c_us = 0, c_kbps = 0;
        if (val == expected) {
            volatile uint32_t *src_u = (volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
            uint32_t acc = 0;
            uint32_t t0 = timer_hw->timerawl;
            for (uint32_t n = 0; n < FLASH_BENCH_WORDS; n++) acc ^= src_u[n];
            u_us = timer_hw->timerawl - t0;
            if (u_us) u_kbps = FLASH_BENCH_SIZE * 1000u / u_us;

            xip_cache_invalidate_range(0, FLASH_BENCH_SIZE);
            volatile uint32_t *src_c = (volatile uint32_t *)XIP_BASE;
            acc = 0;
            t0 = timer_hw->timerawl;
            for (uint32_t n = 0; n < FLASH_BENCH_WORDS; n++) acc ^= src_c[n];
            c_us = timer_hw->timerawl - t0;
            if (c_us) c_kbps = FLASH_BENCH_SIZE * 1000u / c_us;
            (void)acc;
        }

        /* Restore original M0 config under direct mode. */
        irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing = orig_timing;
        qmi_hw->m[0].rfmt   = orig_rfmt;
        qmi_hw->m[0].rcmd   = orig_rcmd;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);

        res[i].clkdiv = cd;
        res[i].rxdelay = rd;
        res[i].sck_mhz = sys_hz / (2u * cd) / 1000000u;
        res[i].read_val = val;
        res[i].expected = expected;
        res[i].pass = (val == expected);
        res[i].u_us = u_us; res[i].u_kbps = u_kbps;
        res[i].c_us = c_us; res[i].c_kbps = c_kbps;
    }
}

void flash_sweep2(void) {
    printf("\n--- Flash M0 sweep (1-4-4 + M-byte SUFFIX=0xA0, 64 KB bench) ---\n");
    uint32_t sys_hz = clock_get_hz(clk_sys);
    printf("  sys_clk = %u MHz\n", (unsigned)(sys_hz / 1000000u));
    printf("  Test: read first word of flash @ uncached alias\n");
    printf("  Expected: 0x%08X\n\n", (unsigned)*(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE);

    #define SW2_COMBOS 24
    uint8_t cdv[SW2_COMBOS], rdv[SW2_COMBOS];
    int n = 0;
    for (uint8_t cd = 1; cd <= 3; cd++)
        for (uint8_t rd = 0; rd <= 7; rd++) {
            cdv[n] = cd; rdv[n] = rd; n++;
        }
    struct flash_sweep2_result res[SW2_COMBOS];
    flash_sweep2_run(res, SW2_COMBOS, cdv, rdv, sys_hz);

    printf("  CLKDIV RXDELAY SCK   read_val    uncached_KBps  cached_KBps  result\n");
    printf("  ------ ------- ---   ----------  -------------  -----------  ------\n");
    for (int i = 0; i < SW2_COMBOS; i++) {
        printf("  %6u %7u %3u   0x%08X  %13u  %11u  %s\n",
               res[i].clkdiv, res[i].rxdelay, (unsigned)res[i].sck_mhz,
               (unsigned)res[i].read_val,
               (unsigned)res[i].u_kbps, (unsigned)res[i].c_kbps,
               res[i].pass ? "PASS" : "FAIL");
    }
    printf("\n  rfmt used: PREFIX=S/8b, ADDR=Q, SUFFIX=Q/8b (M-byte 0xA0),\n");
    printf("             DUMMY=Q/16b (4 Q-clocks), DATA=Q\n");
    printf("  rcmd: PREFIX=0xEB (Fast Read Quad I/O), SUFFIX=0xA0\n");
    #undef SW2_COMBOS
}


/* ---------------------------------------------------------------------
 * flash_reset — emergency recovery. Sends Reset Enable (0x66) + Reset
 * (0x99) to flash via direct mode, in both quad and single-bit cmd
 * widths. W25Q datasheet §8.2.24: this sequence resets the device to
 * power-on state regardless of current SPI/QPI mode. Kept as a
 * standalone utility in case future bringup experiments leave flash
 * in an unexpected mode.
 * ------------------------------------------------------------------- */
static void __no_inline_not_in_flash_func(flash_reset_run)(void) {
    uint32_t irq_save = save_and_disable_interrupts();

    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS0N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    /* QPI reset attempt */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                        0x66u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                        0x99u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    for (volatile uint32_t i = 0; i < 5000; i++) __asm volatile ("nop");

    /* SPI reset attempt (device may now be in SPI after QPI reset) */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x66u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x99u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    for (volatile uint32_t i = 0; i < 5000; i++) __asm volatile ("nop");

    qmi_hw->direct_csr = 0;
    __asm volatile ("dsb sy" ::: "memory");
    restore_interrupts(irq_save);
}

void flash_reset(void) {
    printf("\n--- Flash emergency reset (66+99 in QPI + SPI) ---\n");
    flash_reset_run();
    uint32_t v = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    printf("  Post-reset MSP read: 0x%08X  %s\n",
           (unsigned)v,
           v == 0x20082000u ? "OK" : "still wrong");
}

/* ---------------------------------------------------------------------
 * flash_boost_pads — apply Pico SDK boot2_w25q080.S high-speed pad
 * config. Raspberry Pi's official board does this via boot2, but boot2
 * doesn't run on RP2350 by default, so we're on bootrom defaults
 * (4 mA SCLK, slow slew, SD schmitt ON) which may be the root cause
 * of the 75 MHz nibble shifts.
 *
 * Changes (mirrored from boot2_w25q080.S lines 95-99, 158-164):
 *   - GPIO_QSPI_SCLK: DRIVE=2 (8 mA), SLEWFAST=1
 *   - GPIO_QSPI_SD0..SD3: SCHMITT=0 (faster input edge)
 *
 * Read-only to flash device — no persistent effect.
 * ------------------------------------------------------------------- */
void flash_boost_pads(void) {
    printf("\n--- Boost RP2350 QSPI pads for high-speed flash ---\n");

    uint32_t sclk_before = pads_qspi_hw->io[0];  /* QSPI_SCLK index = 0 in some SDKs */
    /* Pico SDK pads_qspi_hw struct layout: io[0]=SCLK, [1]=SD0, [2]=SD1, [3]=SD2, [4]=SD3, [5]=SS */
    printf("  Before: SCLK=0x%08X  SD0=0x%08X  SD1=0x%08X  SD2=0x%08X  SD3=0x%08X\n",
           (unsigned)pads_qspi_hw->io[0], (unsigned)pads_qspi_hw->io[1],
           (unsigned)pads_qspi_hw->io[2], (unsigned)pads_qspi_hw->io[3],
           (unsigned)pads_qspi_hw->io[4]);
    (void)sclk_before;

    /* Decode SCLK */
    uint32_t s = pads_qspi_hw->io[0];
    printf("    SCLK   DRIVE=%u (%s)  SLEWFAST=%u  SCHMITT=%u\n",
           (unsigned)((s & PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_BITS) >> PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB),
           ((s & PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_BITS) >> PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB) == 0 ? "2mA" :
           ((s & PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_BITS) >> PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB) == 1 ? "4mA" :
           ((s & PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_BITS) >> PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB) == 2 ? "8mA" : "12mA",
           (unsigned)(!!(s & PADS_QSPI_GPIO_QSPI_SCLK_SLEWFAST_BITS)),
           (unsigned)(!!(s & PADS_QSPI_GPIO_QSPI_SCLK_SCHMITT_BITS)));

    /* Apply boot2_w25q080 values: SCLK drive=2 (8mA) + slewfast, SD schmitt off. */
    pads_qspi_hw->io[0] =
        (PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_VALUE_8MA << PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB) |
        PADS_QSPI_GPIO_QSPI_SCLK_SLEWFAST_BITS |
        PADS_QSPI_GPIO_QSPI_SCLK_IE_BITS;
    for (int i = 1; i <= 4; i++) {
        uint32_t v = pads_qspi_hw->io[i];
        v &= ~PADS_QSPI_GPIO_QSPI_SD0_SCHMITT_BITS;
        pads_qspi_hw->io[i] = v;
    }

    printf("  After:  SCLK=0x%08X  SD0=0x%08X  SD1=0x%08X  SD2=0x%08X  SD3=0x%08X\n",
           (unsigned)pads_qspi_hw->io[0], (unsigned)pads_qspi_hw->io[1],
           (unsigned)pads_qspi_hw->io[2], (unsigned)pads_qspi_hw->io[3],
           (unsigned)pads_qspi_hw->io[4]);
    s = pads_qspi_hw->io[0];
    printf("    SCLK   DRIVE=%u (%s)  SLEWFAST=%u  SCHMITT=%u\n",
           (unsigned)((s & PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_BITS) >> PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB),
           ((s & PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_BITS) >> PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB) == 2 ? "8mA" : "?",
           (unsigned)(!!(s & PADS_QSPI_GPIO_QSPI_SCLK_SLEWFAST_BITS)),
           (unsigned)(!!(s & PADS_QSPI_GPIO_QSPI_SCLK_SCHMITT_BITS)));
    printf("  Now try flash_sweep2 at CLKDIV=1.\n");
}

/* ---------------------------------------------------------------------
 * flash_pad_ablation — find the minimum pad tweak needed for 75 MHz.
 *
 * Sweeps 2^3 = 8 combinations of:
 *   SCLK DRIVE   : 4 mA (default)  / 8 mA (boot2_w25q080)
 *   SCLK SLEWFAST: 0 (default)     / 1 (boot2)
 *   SD0-3 SCHMITT: 1 (default)     / 0 (boot2)
 *
 * For each combo at CLKDIV=1 RXDELAY=2: single-word correctness +
 * 16 KB uncached bench. Goal: ship with least aggressive pad config.
 *
 * Ablation runs entirely from RAM. Flash timing is switched to
 * CLKDIV=1 for each combo and restored at the end of each iteration.
 * ------------------------------------------------------------------- */
struct pad_combo_result {
    uint8_t drive_val;    /* 1 = 4 mA, 2 = 8 mA */
    uint8_t slewfast;     /* 0 or 1 */
    uint8_t schmitt_off;  /* 1 = schmitt cleared */
    uint32_t pads_sclk;
    uint32_t pads_sd0;
    uint32_t read_val;
    bool     pass;
    uint32_t kbps;
};

void __no_inline_not_in_flash_func(flash_pad_ablation_run)(
        struct pad_combo_result *out, int n, uint32_t expected) {
    uint32_t orig_sclk = pads_qspi_hw->io[0];
    uint32_t orig_sd0  = pads_qspi_hw->io[1];
    uint32_t orig_sd1  = pads_qspi_hw->io[2];
    uint32_t orig_sd2  = pads_qspi_hw->io[3];
    uint32_t orig_sd3  = pads_qspi_hw->io[4];
    uint32_t orig_timing = qmi_hw->m[0].timing;
    uint32_t orig_rfmt   = qmi_hw->m[0].rfmt;
    uint32_t orig_rcmd   = qmi_hw->m[0].rcmd;

    /* Keep interrupts disabled for the entire ablation. Any ISR that
     * fetches from flash during a bad-timing iteration would hang
     * (observed: PC stuck in INTISR3 when interrupts ran between
     * iterations). Total runtime ~2 ms so USB tolerance is fine. */
    uint32_t irq_save = save_and_disable_interrupts();

    /* Preload M0 config for 75 MHz 1-4-4 with M-byte — same as
     * flash_sweep2. Keep this constant across the ablation. */
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    qmi_hw->m[0].timing =
        (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
        (2u << QMI_M0_TIMING_RXDELAY_LSB) |
        (1u << QMI_M0_TIMING_CLKDIV_LSB);
    qmi_hw->m[0].rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_S << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB)   |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB)   |
        (QMI_M0_RFMT_SUFFIX_LEN_VALUE_8   << QMI_M0_RFMT_SUFFIX_LEN_LSB)   |
        (4u << QMI_M0_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[0].rcmd =
        (0xEBu << QMI_M0_RCMD_PREFIX_LSB) |
        (0xA0u << QMI_M0_RCMD_SUFFIX_LSB);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);

    for (int i = 0; i < n; i++) {
        uint32_t sclk_val =
            ((uint32_t)out[i].drive_val << PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB) |
            (out[i].slewfast ? PADS_QSPI_GPIO_QSPI_SCLK_SLEWFAST_BITS : 0) |
            (out[i].schmitt_off ? 0 : PADS_QSPI_GPIO_QSPI_SCLK_SCHMITT_BITS) |
            PADS_QSPI_GPIO_QSPI_SCLK_IE_BITS;
        uint32_t sd_mask = PADS_QSPI_GPIO_QSPI_SD0_SCHMITT_BITS;

        pads_qspi_hw->io[0] = sclk_val;
        for (int j = 1; j <= 4; j++) {
            uint32_t v = pads_qspi_hw->io[j];
            if (out[i].schmitt_off) v &= ~sd_mask;
            else                    v |= sd_mask;
            pads_qspi_hw->io[j] = v;
        }

        out[i].pads_sclk = pads_qspi_hw->io[0];
        out[i].pads_sd0  = pads_qspi_hw->io[1];

        uint32_t val = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
        out[i].read_val = val;
        out[i].pass = (val == expected);
        if (out[i].pass) {
            volatile uint32_t *src = (volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
            uint32_t acc = 0;
            uint32_t t0 = timer_hw->timerawl;
            for (uint32_t k = 0; k < 4096u; k++) acc ^= src[k];
            uint32_t us = timer_hw->timerawl - t0;
            if (us) out[i].kbps = (4096u * 4u * 1000u) / us;
            else    out[i].kbps = 0;
            (void)acc;
        } else {
            out[i].kbps = 0;
        }
    }

    /* Restore pads + M0 inside the same interrupts-disabled window. */
    pads_qspi_hw->io[0] = orig_sclk;
    pads_qspi_hw->io[1] = orig_sd0;
    pads_qspi_hw->io[2] = orig_sd1;
    pads_qspi_hw->io[3] = orig_sd2;
    pads_qspi_hw->io[4] = orig_sd3;
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    qmi_hw->m[0].timing = orig_timing;
    qmi_hw->m[0].rfmt   = orig_rfmt;
    qmi_hw->m[0].rcmd   = orig_rcmd;
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(irq_save);
}

void flash_pad_ablation(void) {
    printf("\n--- Flash pad ablation at CLKDIV=1 RXDELAY=2 (75 MHz) ---\n");
    uint32_t expected = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    printf("  Expected: 0x%08X  (baseline pads + default CLKDIV)\n\n",
           (unsigned)expected);

    struct pad_combo_result res[8];
    int idx = 0;
    for (uint8_t drv = 1; drv <= 2; drv++) {       /* 4 mA or 8 mA */
        for (uint8_t slew = 0; slew <= 1; slew++) {
            for (uint8_t sch_off = 0; sch_off <= 1; sch_off++) {
                res[idx].drive_val = drv;
                res[idx].slewfast = slew;
                res[idx].schmitt_off = sch_off;
                idx++;
            }
        }
    }
    flash_pad_ablation_run(res, 8, expected);

    printf("  SCLK_DRIVE  SLEWFAST  SD_SCHMITT  SCLK_pad   SD0_pad    read_val    KB/s   result\n");
    printf("  ----------  --------  ----------  --------   --------   ----------  -----  ------\n");
    for (int i = 0; i < 8; i++) {
        printf("  %s     %u          %s         0x%06X   0x%06X   0x%08X  %5u  %s\n",
               res[i].drive_val == 2 ? "8mA " : "4mA ",
               (unsigned)res[i].slewfast,
               res[i].schmitt_off ? "off" : "on ",
               (unsigned)res[i].pads_sclk,
               (unsigned)res[i].pads_sd0,
               (unsigned)res[i].read_val,
               (unsigned)res[i].kbps,
               res[i].pass ? "PASS" : "FAIL");
    }
    printf("\n  (combos listed default-first: 4mA/slow/on is the bootrom baseline)\n");
    printf("  Find the smallest change-count that passes = minimum necessary tweaks.\n");
}

/* ---------------------------------------------------------------------
 * flash_deep_scan — whole-flash XOR compare at baseline (current M0)
 * vs CLKDIV=1 + SLEWFAST=1.
 *
 * Motivation: flash_pad_ablation only reads first 16 KB (4096 words).
 * Production firmware hit a deterministic corruption at flash offset
 * 0x2B8A6 (180 KB deep) under CLKDIV=1, where M0 returns stale/wrong
 * bytes despite the shallow-address test passing. This scan covers
 * the full 16 MB address space so we can tell whether CLKDIV=1 is
 * genuinely unreliable or whether it was just a production-specific
 * interaction.
 *
 * Approach: per-64 KB block (256 blocks for 16 MB), accumulate an XOR
 * of all 16384 words in each block. Pass 1 at baseline timing (known
 * good; CLKDIV=3 by default). Pass 2 after switching to CLKDIV=1 +
 * SLEWFAST=1 + RXDELAY=2. Any block whose XOR differs between the
 * two passes is a corruption signal. Report count + first bad block;
 * then word-scan the first bad block at baseline timing to locate
 * the exact first wrong word.
 *
 * Interrupts disabled across both passes (~1.6 s total at 20 MB/s).
 * Runs from RAM so the CLKDIV=1 window doesn't fault its own fetch.
 * ------------------------------------------------------------------- */
#define DEEP_BLOCK_WORDS  16384u      /* 64 KB per block */
#define DEEP_BLOCKS       256u        /* 16 MB / 64 KB  */

static uint32_t s_deep_baseline[DEEP_BLOCKS];

struct deep_scan_result {
    uint32_t baseline_timing;
    uint32_t clkdiv1_timing;
    uint32_t bad_blocks;        /* count of blocks whose XOR differs */
    int32_t  first_bad_blk;     /* -1 if all clean */
    uint32_t first_bad_word_idx;    /* within block */
    uint32_t baseline_word_val;
    uint32_t clkdiv1_word_val;
    uint32_t pass1_ms;
    uint32_t pass2_ms;
};

void __no_inline_not_in_flash_func(flash_deep_scan_run)(
        struct deep_scan_result *r) {
    volatile uint32_t *flash = (volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t orig_pads = pads_qspi_hw->io[0];
    uint32_t orig_timing = qmi_hw->m[0].timing;
    r->baseline_timing = orig_timing;

    uint32_t irq_save = save_and_disable_interrupts();

    /* Pass 1: baseline XOR per 64 KB block. */
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    for (uint32_t blk = 0; blk < DEEP_BLOCKS; blk++) {
        uint32_t acc = 0;
        const uint32_t base = blk * DEEP_BLOCK_WORDS;
        for (uint32_t w = 0; w < DEEP_BLOCK_WORDS; w++)
            acc ^= flash[base + w];
        s_deep_baseline[blk] = acc;
    }
    r->pass1_ms = to_ms_since_boot(get_absolute_time()) - t0;

    /* Apply SLEWFAST=1 + CLKDIV=1 RXDELAY=2 COOLDOWN=1. */
    pads_qspi_hw->io[0] = orig_pads | PADS_QSPI_GPIO_QSPI_SCLK_SLEWFAST_BITS;
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    const uint32_t kMask = QMI_M0_TIMING_COOLDOWN_BITS |
                           QMI_M0_TIMING_RXDELAY_BITS |
                           QMI_M0_TIMING_CLKDIV_BITS;
    qmi_hw->m[0].timing = (orig_timing & ~kMask) |
                          (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
                          (2u << QMI_M0_TIMING_RXDELAY_LSB) |
                          (1u << QMI_M0_TIMING_CLKDIV_LSB);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    __asm volatile ("dsb sy" ::: "memory");
    r->clkdiv1_timing = qmi_hw->m[0].timing;

    /* Pass 2: CLKDIV=1 XOR per block, find first divergence. */
    r->bad_blocks = 0;
    r->first_bad_blk = -1;
    t0 = to_ms_since_boot(get_absolute_time());
    for (uint32_t blk = 0; blk < DEEP_BLOCKS; blk++) {
        uint32_t acc = 0;
        const uint32_t base = blk * DEEP_BLOCK_WORDS;
        for (uint32_t w = 0; w < DEEP_BLOCK_WORDS; w++)
            acc ^= flash[base + w];
        if (acc != s_deep_baseline[blk]) {
            r->bad_blocks++;
            if (r->first_bad_blk < 0) r->first_bad_blk = (int32_t)blk;
        }
    }
    r->pass2_ms = to_ms_since_boot(get_absolute_time()) - t0;

    /* Restore baseline timing + pads. Word-level localisation of the
     * first bad address is skipped here (it would need a 64 KB RAM
     * buffer for the whole block's CLKDIV=1 values, which blows the
     * 520 KB RAM budget when combined with the PSRAM + TFT state).
     * User can follow up with SWD `mem32` at the bad block base and
     * binary-search to find the exact first wrong word. */
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    qmi_hw->m[0].timing = orig_timing;
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    pads_qspi_hw->io[0] = orig_pads;
    __asm volatile ("dsb sy" ::: "memory");
    restore_interrupts(irq_save);
    r->first_bad_word_idx = 0xFFFFFFFFu;
    r->baseline_word_val = 0;
    r->clkdiv1_word_val = 0;
}

void flash_deep_scan(void) {
    printf("\n--- Flash deep scan (full 16 MB: baseline vs CLKDIV=1 + SLEWFAST=1) ---\n");
    struct deep_scan_result r = {0};
    flash_deep_scan_run(&r);

    printf("  baseline M0.timing = 0x%08X\n", (unsigned)r.baseline_timing);
    printf("  CLKDIV=1 M0.timing = 0x%08X\n", (unsigned)r.clkdiv1_timing);
    printf("  Pass 1 (baseline)  = %u ms  (%u KB/s)\n",
           (unsigned)r.pass1_ms,
           r.pass1_ms ? (unsigned)(16u * 1024u * 1000u / r.pass1_ms) : 0);
    printf("  Pass 2 (CLKDIV=1)  = %u ms  (%u KB/s)\n",
           (unsigned)r.pass2_ms,
           r.pass2_ms ? (unsigned)(16u * 1024u * 1000u / r.pass2_ms) : 0);
    printf("  Bad blocks (XOR mismatch) = %u / %u\n",
           (unsigned)r.bad_blocks, (unsigned)DEEP_BLOCKS);

    if (r.first_bad_blk < 0) {
        printf("  CLKDIV=1 reads entire 16 MB correctly — matches baseline.\n");
        printf("  => Production HardFault is NOT a CLKDIV=1 data-corruption issue.\n");
        return;
    }

    uint32_t bad_blk_addr = 0x10000000u + (uint32_t)r.first_bad_blk * (DEEP_BLOCK_WORDS * 4u);
    printf("  First bad block @ 0x%08X (block #%u, offset 0x%X)\n",
           (unsigned)bad_blk_addr, (unsigned)r.first_bad_blk,
           (unsigned)((uint32_t)r.first_bad_blk * (DEEP_BLOCK_WORDS * 4u)));
    printf("  => CLKDIV=1 + SLEWFAST=1 is genuinely unreliable at deep flash\n");
    printf("     addresses; bringup_pad_ablation's 16 KB test was too narrow.\n");
    printf("  Word-level isolation: use SWD `mem32 0x%08X N` at two\n",
           (unsigned)bad_blk_addr);
    printf("  timings (baseline then CLKDIV=1) to bisect the exact bad word.\n");
}

/* ---------------------------------------------------------------------
 * flash_deep_ablation — re-run the pad ablation using deep_scan as
 * the oracle (not the one-word sentinel check in flash_pad_ablation,
 * which was a false positive for CLKDIV=1).
 *
 * For each of 4 pad configs (minimum → full boot2_w25q080), scan the
 * entire 16 MB address space at CLKDIV=1 + RXDELAY=2 and count how
 * many 64 KB blocks diverge from the baseline (CLKDIV=3) XOR. A
 * config "passes" only if bad_blocks == 0.
 *
 * Runs IRQ-off throughout (~6.4 s total for 4 combos × 1.6 s each).
 * ------------------------------------------------------------------- */
struct dab_combo {
    uint8_t  drive;     /* 1=4 mA default, 2=8 mA */
    bool     slewfast;
    bool     schmitt_off;  /* true = clear SCHMITT on SD0-3 */
    uint32_t bad_blocks;
    uint32_t pass_ms;
};

void __no_inline_not_in_flash_func(flash_deep_ablation_run)(
        struct dab_combo *combos, int n) {
    volatile uint32_t *flash = (volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t orig_sclk = pads_qspi_hw->io[0];
    uint32_t orig_sd[4] = { pads_qspi_hw->io[1], pads_qspi_hw->io[2],
                            pads_qspi_hw->io[3], pads_qspi_hw->io[4] };
    uint32_t orig_timing = qmi_hw->m[0].timing;

    uint32_t irq_save = save_and_disable_interrupts();

    /* Baseline XOR table at current (safe) M0 timing. */
    for (uint32_t blk = 0; blk < DEEP_BLOCKS; blk++) {
        uint32_t acc = 0;
        const uint32_t base = blk * DEEP_BLOCK_WORDS;
        for (uint32_t w = 0; w < DEEP_BLOCK_WORDS; w++)
            acc ^= flash[base + w];
        s_deep_baseline[blk] = acc;
    }

    for (int i = 0; i < n; i++) {
        /* Apply pad config for this combo. */
        uint32_t sclk_val =
            ((uint32_t)combos[i].drive << PADS_QSPI_GPIO_QSPI_SCLK_DRIVE_LSB) |
            (combos[i].slewfast ? PADS_QSPI_GPIO_QSPI_SCLK_SLEWFAST_BITS : 0) |
            (combos[i].schmitt_off ? 0 : PADS_QSPI_GPIO_QSPI_SCLK_SCHMITT_BITS) |
            PADS_QSPI_GPIO_QSPI_SCLK_IE_BITS;
        pads_qspi_hw->io[0] = sclk_val;
        for (int j = 1; j <= 4; j++) {
            uint32_t v = pads_qspi_hw->io[j];
            if (combos[i].schmitt_off)
                v &= ~PADS_QSPI_GPIO_QSPI_SD0_SCHMITT_BITS;
            else
                v |= PADS_QSPI_GPIO_QSPI_SD0_SCHMITT_BITS;
            pads_qspi_hw->io[j] = v;
        }

        /* Switch M0 to CLKDIV=1 RXDELAY=2. */
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        const uint32_t kMask = QMI_M0_TIMING_COOLDOWN_BITS |
                               QMI_M0_TIMING_RXDELAY_BITS |
                               QMI_M0_TIMING_CLKDIV_BITS;
        qmi_hw->m[0].timing = (orig_timing & ~kMask) |
                              (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
                              (2u << QMI_M0_TIMING_RXDELAY_LSB) |
                              (1u << QMI_M0_TIMING_CLKDIV_LSB);
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        __asm volatile ("dsb sy" ::: "memory");

        /* Scan at CLKDIV=1 and diff against baseline. */
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        uint32_t bad = 0;
        for (uint32_t blk = 0; blk < DEEP_BLOCKS; blk++) {
            uint32_t acc = 0;
            const uint32_t base = blk * DEEP_BLOCK_WORDS;
            for (uint32_t w = 0; w < DEEP_BLOCK_WORDS; w++)
                acc ^= flash[base + w];
            if (acc != s_deep_baseline[blk]) bad++;
        }
        combos[i].pass_ms = to_ms_since_boot(get_absolute_time()) - t0;
        combos[i].bad_blocks = bad;

        /* Restore M0 timing between combos so the next combo starts
         * from a known-good baseline read (and won't compound errors
         * if the pad change alone leaves M0 wedged). */
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing = orig_timing;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        __asm volatile ("dsb sy" ::: "memory");
    }

    /* Restore everything. */
    pads_qspi_hw->io[0] = orig_sclk;
    pads_qspi_hw->io[1] = orig_sd[0];
    pads_qspi_hw->io[2] = orig_sd[1];
    pads_qspi_hw->io[3] = orig_sd[2];
    pads_qspi_hw->io[4] = orig_sd[3];
    restore_interrupts(irq_save);
}

void flash_deep_ablation(void) {
    printf("\n--- Flash deep ablation: adding DRIVE and SCHMITT on top of SLEWFAST ---\n");
    printf("  Oracle: 16 MB block-XOR compared baseline (CLKDIV=3) vs CLKDIV=1.\n");
    printf("  Goal: find minimum pad config that passes the full-flash verify.\n\n");

    struct dab_combo combos[4] = {
        { 1, true, false, 0, 0 },  /* SLEWFAST only */
        { 2, true, false, 0, 0 },  /* + DRIVE=8 mA */
        { 1, true, true,  0, 0 },  /* + SCHMITT off */
        { 2, true, true,  0, 0 },  /* full boot2_w25q080 boost */
    };
    flash_deep_ablation_run(combos, 4);

    printf("  SCLK DRIVE  SLEWFAST  SD SCHMITT  bad_blocks/%u  scan_ms  result\n",
           (unsigned)DEEP_BLOCKS);
    printf("  ----------  --------  ----------  -------------  -------  ------\n");
    for (int i = 0; i < 4; i++) {
        printf("  %s       %s        %s         %8u       %5u    %s\n",
               combos[i].drive == 2 ? "8mA" : "4mA",
               combos[i].slewfast    ? "on " : "off",
               combos[i].schmitt_off ? "off" : "on ",
               (unsigned)combos[i].bad_blocks,
               (unsigned)combos[i].pass_ms,
               combos[i].bad_blocks == 0 ? "PASS" : "FAIL");
    }
    printf("\n  0 bad blocks = CLKDIV=1 is actually viable at this pad config.\n");
}

void flash_speed_test(void) {
    printf("\n--- Flash (M0) XIP Speed Test ---\n");

    uint32_t sys_hz = clock_get_hz(clk_sys);
    printf("  sys_clk = %u MHz\n", (unsigned)(sys_hz / 1000000u));
    printf("  Test: read vector table MSP @ 0x%08X\n", FLASH_TEST_ADDR);
    printf("  Expected value: 0x%08X\n\n", (unsigned)FLASH_TEST_EXPECT);

    // Build test matrix: CLKDIV=1..4 × RXDELAY=0..3
    #define FLASH_COMBOS 16
    uint8_t cd_arr[FLASH_COMBOS], rd_arr[FLASH_COMBOS];
    struct flash_speed_result res[FLASH_COMBOS];
    int idx = 0;
    for (uint8_t cd = 1; cd <= 4; cd++)
        for (uint8_t rd = 0; rd <= 3; rd++) {
            cd_arr[idx] = cd;
            rd_arr[idx] = rd;
            idx++;
        }

    flash_speed_run(res, FLASH_COMBOS, cd_arr, rd_arr, sys_hz);

    printf("  CLKDIV  RXDELAY  SCK_MHz  read_val    bench_us  KB/s    result\n");
    printf("  ------  -------  -------  ----------  --------  ------  ------\n");
    for (int i = 0; i < FLASH_COMBOS; i++) {
        printf("  %6u  %7u  %7u  0x%08X  %8u  %6u  %s\n",
               res[i].clkdiv, res[i].rxdelay, res[i].sck_mhz,
               (unsigned)res[i].read_val,
               (unsigned)res[i].bench_us,
               (unsigned)res[i].bench_kbps,
               res[i].pass ? "PASS" : "FAIL");
    }
    printf("\n  (bench = 64 KB uncached read at the candidate timing)\n");
    #undef FLASH_COMBOS
}
