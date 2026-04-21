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
 * flash_probe_75 — diagnose 75 MHz failure mode.
 *
 * Reads three known flash words N=256 times each at CLKDIV=1 across
 * RXDELAY 0..7 and reports up to 4 most-common values per slot. If a
 * slot always returns the SAME wrong value, the failure is a
 * deterministic sampling offset (timing — fixable with DUMMY_LEN or
 * RXDELAY tuning). If values vary randomly, we're in signal-integrity
 * territory (PCB/trace/pad, not fixable by register tweaks).
 *
 * Probe addresses:
 *   0x10000000 -> MSP sentinel (expected 0x20082000)
 *   0x10000004 -> reset handler (some Thumb code address)
 *   0x10001000 -> deep in code section (should be non-zero non-FF)
 * ------------------------------------------------------------------- */
#define PROBE_COUNT 16
#define PROBE_TOP   4

struct probe_slot {
    uint32_t values[PROBE_TOP];
    uint16_t counts[PROBE_TOP];
    uint8_t  unique;  /* bounded to PROBE_TOP; > = ran out of slots */
};

static void __no_inline_not_in_flash_func(probe_record)(
        struct probe_slot *s, uint32_t v) {
    for (int i = 0; i < s->unique && i < PROBE_TOP; i++) {
        if (s->values[i] == v) { s->counts[i]++; return; }
    }
    if (s->unique < PROBE_TOP) {
        s->values[s->unique] = v;
        s->counts[s->unique] = 1;
        s->unique++;
    } else {
        /* Saturated — just bump last slot to keep error count meaningful. */
        s->counts[PROBE_TOP - 1]++;
    }
}

void __no_inline_not_in_flash_func(flash_probe_75_run)(
        uint8_t rd, const uint32_t *addrs, int n_addr,
        struct probe_slot *out /* [n_addr] */) {
    uint32_t orig_timing = qmi_hw->m[0].timing;
    uint32_t orig_rfmt   = qmi_hw->m[0].rfmt;
    uint32_t orig_rcmd   = qmi_hw->m[0].rcmd;

    /* Program CLKDIV=1, rd = requested. rfmt/rcmd unchanged (keep
     * original boot2 config — we only tweak timing). */
    uint32_t irq_save = save_and_disable_interrupts();
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    qmi_hw->m[0].timing =
        (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
        ((uint32_t)rd << QMI_M0_TIMING_RXDELAY_LSB) |
        (1u << QMI_M0_TIMING_CLKDIV_LSB);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(irq_save);

    for (int a = 0; a < n_addr; a++) {
        out[a].unique = 0;
        for (int i = 0; i < PROBE_TOP; i++) { out[a].values[i] = 0; out[a].counts[i] = 0; }
        volatile uint32_t *p = (volatile uint32_t *)addrs[a];
        for (int k = 0; k < PROBE_COUNT; k++) {
            probe_record(&out[a], *p);
        }
    }

    /* Restore. */
    irq_save = save_and_disable_interrupts();
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    qmi_hw->m[0].timing = orig_timing;
    qmi_hw->m[0].rfmt   = orig_rfmt;
    qmi_hw->m[0].rcmd   = orig_rcmd;
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(irq_save);
}

void flash_probe_75(void) {
    printf("\n--- Flash 75 MHz failure diagnosis (256 reads/slot) ---\n");
    uint32_t sys_hz = clock_get_hz(clk_sys);
    printf("  sys_clk = %u MHz  M0 baseline timing = 0x%08X\n",
           (unsigned)(sys_hz / 1000000u),
           (unsigned)qmi_hw->m[0].timing);

    /* Expected values grabbed at the known-good baseline BEFORE any
     * timing tweak, via uncached alias so cache doesn't mask. */
    const uint32_t addrs[3] = {
        XIP_NOCACHE_NOALLOC_BASE + 0x0000u,
        XIP_NOCACHE_NOALLOC_BASE + 0x0004u,
        XIP_NOCACHE_NOALLOC_BASE + 0x1000u,
    };
    uint32_t expected[3];
    for (int i = 0; i < 3; i++) expected[i] = *(volatile uint32_t *)addrs[i];
    printf("  Expected: [0x%08X] [0x%08X] [0x%08X]\n",
           (unsigned)expected[0], (unsigned)expected[1], (unsigned)expected[2]);

    for (uint8_t rd = 0; rd < 8; rd++) {
        struct probe_slot s[3];
        flash_probe_75_run(rd, addrs, 3, s);
        printf("\n  RXDELAY=%u:\n", rd);
        for (int a = 0; a < 3; a++) {
            int correct_count = 0;
            for (int u = 0; u < s[a].unique; u++)
                if (s[a].values[u] == expected[a]) correct_count = s[a].counts[u];
            printf("    [%08X] unique=%u  correct=%u/%u",
                   (unsigned)(addrs[a] - XIP_NOCACHE_NOALLOC_BASE + 0x10000000u),
                   s[a].unique, correct_count, PROBE_COUNT);
            /* Print top entries so we can see the dominant wrong value. */
            for (int u = 0; u < s[a].unique; u++) {
                printf("  [%08X:%u]", (unsigned)s[a].values[u], s[a].counts[u]);
            }
            printf("\n");
        }
    }

    printf("\n  Interpretation:\n");
    printf("    unique==1 always -> deterministic sampling offset (timing fixable)\n");
    printf("    unique>1 many    -> random bit errors (signal integrity limit)\n");
    printf("    unique==1 correct-> that (CLKDIV=1, RXDELAY) is a valid combo\n");
}

/* ---------------------------------------------------------------------
 * flash_sweep3 — exhaustive 75 MHz config sweep.
 *
 * Hypothesis: PSRAM works at 75 MHz with DUMMY_LEN=24 (6 Q-clocks),
 * but our flash config used DUMMY_LEN=16 (4 Q-clocks). Since PSRAM and
 * flash share SCK/SD[3:0] on the same QMI bus, signal-integrity can't
 * be the sole limit — something in flash's rfmt/rcmd must differ.
 *
 * Sweep grid at CLKDIV=1:
 *   DUMMY_LEN = 4, 5, 6, 7 (Q-clocks = 16, 20, 24, 28 bits)
 *   SUFFIX = none / 0xA0 / 0xF0
 *   RXDELAY = 1, 2, 3
 * = 4 × 3 × 3 = 36 combos. For each: single-word correctness + 16 KB
 * bench (shortened to keep wall-clock manageable).
 * ------------------------------------------------------------------- */
struct fs3_result {
    uint8_t  dummy_field;  /* DUMMY_LEN field value (1..7) */
    uint8_t  suffix_mode;  /* 0=none, 1=0xA0, 2=0xF0 */
    uint8_t  rxdelay;
    uint32_t rfmt;
    uint32_t rcmd;
    uint32_t read_val;
    bool     pass;
    uint32_t us;
    uint32_t kbps;
};

#define FS3_BENCH_WORDS 4096u   /* 16 KB */

void __no_inline_not_in_flash_func(flash_sweep3_run)(
        struct fs3_result *out, int n,
        uint32_t expected) {
    uint32_t orig_timing = qmi_hw->m[0].timing;
    uint32_t orig_rfmt   = qmi_hw->m[0].rfmt;
    uint32_t orig_rcmd   = qmi_hw->m[0].rcmd;

    for (int i = 0; i < n; i++) {
        uint32_t rfmt_base =
            (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_S << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
            (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB)   |
            (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB)  |
            (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB)   |
            (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB)   |
            ((uint32_t)out[i].dummy_field     << QMI_M0_RFMT_DUMMY_LEN_LSB);

        uint32_t rcmd = (0xEBu << QMI_M0_RCMD_PREFIX_LSB);
        if (out[i].suffix_mode == 1) {
            rfmt_base |=
                (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
                (QMI_M0_RFMT_SUFFIX_LEN_VALUE_8   << QMI_M0_RFMT_SUFFIX_LEN_LSB);
            rcmd |= (0xA0u << QMI_M0_RCMD_SUFFIX_LSB);
        } else if (out[i].suffix_mode == 2) {
            rfmt_base |=
                (QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB) |
                (QMI_M0_RFMT_SUFFIX_LEN_VALUE_8   << QMI_M0_RFMT_SUFFIX_LEN_LSB);
            rcmd |= (0xF0u << QMI_M0_RCMD_SUFFIX_LSB);
        }

        out[i].rfmt = rfmt_base;
        out[i].rcmd = rcmd;

        uint32_t irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing =
            (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
            ((uint32_t)out[i].rxdelay << QMI_M0_TIMING_RXDELAY_LSB) |
            (1u << QMI_M0_TIMING_CLKDIV_LSB);
        qmi_hw->m[0].rfmt = rfmt_base;
        qmi_hw->m[0].rcmd = rcmd;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);

        uint32_t val = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
        out[i].read_val = val;
        out[i].pass = (val == expected);

        if (out[i].pass) {
            volatile uint32_t *src = (volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
            uint32_t acc = 0;
            uint32_t t0 = timer_hw->timerawl;
            for (uint32_t k = 0; k < FS3_BENCH_WORDS; k++) acc ^= src[k];
            out[i].us = timer_hw->timerawl - t0;
            if (out[i].us)
                out[i].kbps = (FS3_BENCH_WORDS * 4u * 1000u) / out[i].us;
            else
                out[i].kbps = 0;
            (void)acc;
        } else {
            out[i].us = 0;
            out[i].kbps = 0;
        }

        irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing = orig_timing;
        qmi_hw->m[0].rfmt   = orig_rfmt;
        qmi_hw->m[0].rcmd   = orig_rcmd;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);
    }
}

void flash_sweep3(void) {
    printf("\n--- Flash M0 75 MHz exhaustive sweep (DUMMY x SUFFIX x RXDELAY) ---\n");
    uint32_t expected = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    printf("  Expected: 0x%08X\n", (unsigned)expected);

    /* DUMMY field values: 4=16b(4clk) 5=20b(5clk) 6=24b(6clk) 7=28b(7clk) */
    static const uint8_t dummies[] = {4, 5, 6, 7};
    static const char *sfx_name[3] = {"none", "M=0xA0", "M=0xF0"};
    static const uint32_t sfx_qclks[3] = {0, 2, 2};

    #define FS3_N 36
    struct fs3_result res[FS3_N];
    int idx = 0;
    for (int sf = 0; sf < 3; sf++)
        for (int d = 0; d < 4; d++)
            for (uint8_t rx = 1; rx <= 3; rx++) {
                res[idx].dummy_field = dummies[d];
                res[idx].suffix_mode = sf;
                res[idx].rxdelay = rx;
                idx++;
            }

    flash_sweep3_run(res, FS3_N, expected);

    printf("  SUFFIX  DUMMY  total_wait  RXDELAY  read_val    KB/s   result\n");
    printf("  ------  -----  ----------  -------  ----------  -----  ------\n");
    for (int i = 0; i < FS3_N; i++) {
        uint32_t total_wait = res[i].dummy_field + sfx_qclks[res[i].suffix_mode];
        printf("  %-6s  %5uQ  %8uQ    %7u  0x%08X  %5u  %s\n",
               sfx_name[res[i].suffix_mode],
               (unsigned)res[i].dummy_field,
               (unsigned)total_wait,
               (unsigned)res[i].rxdelay,
               (unsigned)res[i].read_val,
               (unsigned)res[i].kbps,
               res[i].pass ? "PASS" : "FAIL");
    }
    printf("\n  (total_wait = SUFFIX clocks + DUMMY clocks, all quad)\n");
    printf("  (DUMMY field 4/5/6/7 = 16/20/24/28 bits = 4/5/6/7 Q-clocks)\n");
    #undef FS3_N
}

/* ---------------------------------------------------------------------
 * Flash QPI experiment (0x35 Enter QPI / 0xF5 Exit QPI).
 *
 * Hypothesis from sweep3: 1-4-4 mode's single-bit cmd + bit-width
 * transition fails at 75 MHz. Full QPI (4-4-4) avoids that transition
 * and matches what PSRAM uses successfully. Test: briefly put flash in
 * QPI mode, read at CLKDIV=1, then ALWAYS exit before returning.
 *
 * Safety: on every exit path (success or fail) we send 0xF5 (Exit QPI
 * in quad) + Reset Enable 0x66 / Reset 0x99 (SPI) as belt-and-braces.
 * If the MCU crashes mid-test and flash is stuck in QPI, emergency
 * recovery = J-Link halt + flash_reset command (also registered).
 * ------------------------------------------------------------------- */
struct qpi_result {
    uint32_t expected;
    uint32_t read_val;
    bool     pass;
    uint32_t us, kbps;
    uint32_t m0_timing_used, m0_rfmt_used;
};

#define QPI_BENCH_WORDS 4096u  /* 16 KB */

static void __no_inline_not_in_flash_func(flash_try_qpi_run)(
        struct qpi_result *out) {
    uint32_t orig_timing = qmi_hw->m[0].timing;
    uint32_t orig_rfmt   = qmi_hw->m[0].rfmt;
    uint32_t orig_rcmd   = qmi_hw->m[0].rcmd;

    uint32_t irq_save = save_and_disable_interrupts();

    /* --- Enter direct mode at a safe slow clock (CLKDIV=10) with
     *     AUTO CS management for CS0 (flash). ----------------------- */
    qmi_hw->direct_csr = (10u << QMI_DIRECT_CSR_CLKDIV_LSB) |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS0N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    /* Send 0x35 (Enter QPI) as single-bit on SD0. Device still in SPI
     * mode at this point, so default IWIDTH (single) is correct. */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    /* --- Reconfigure M0 for full QPI @ 75 MHz, matching PSRAM's
     *     working config (PREFIX_WIDTH=Q, DUMMY_LEN=24 bits). -------- */
    uint32_t new_timing = (2u << QMI_M0_TIMING_COOLDOWN_LSB) |
                          (2u << QMI_M0_TIMING_RXDELAY_LSB) |
                          (1u << QMI_M0_TIMING_CLKDIV_LSB);
    uint32_t new_rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB)   |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB)   |
        (6u                               << QMI_M0_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[0].timing = new_timing;
    qmi_hw->m[0].rfmt   = new_rfmt;
    qmi_hw->m[0].rcmd   = 0xEBu;  /* QPI Fast Read (in QPI: cmd is quad) */
    out->m0_timing_used = new_timing;
    out->m0_rfmt_used   = new_rfmt;

    /* Exit direct mode so M1 XIP engine (now configured for QPI) takes
     * over. */
    qmi_hw->direct_csr = (10u << QMI_DIRECT_CSR_CLKDIV_LSB); /* EN=0 */
    __asm volatile ("dsb sy" ::: "memory");

    /* --- Test read via XIP at new timing. ---------------------------- */
    uint32_t val = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    out->read_val = val;
    out->pass = (val == out->expected);

    if (out->pass) {
        volatile uint32_t *src = (volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
        uint32_t acc = 0;
        uint32_t t0 = timer_hw->timerawl;
        for (uint32_t k = 0; k < QPI_BENCH_WORDS; k++) acc ^= src[k];
        out->us = timer_hw->timerawl - t0;
        if (out->us) out->kbps = (QPI_BENCH_WORDS * 4u * 1000u) / out->us;
        (void)acc;
    } else {
        out->us = 0; out->kbps = 0;
    }

    /* --- UNCONDITIONAL exit: re-enter direct mode, send 0xF5 in QUAD
     *     (device is in QPI, cmd must be quad now), then also send
     *     Reset Enable 0x66 + Reset 0x99 (now back in SPI) as a
     *     belt-and-braces to force flash to known default. ----------- */
    qmi_hw->direct_csr = (10u << QMI_DIRECT_CSR_CLKDIV_LSB) |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS0N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    /* Exit QPI: 0xF5 sent as quad */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS |
                        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
                        0xF5u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    /* Belt-and-braces: Reset Enable + Reset (single-bit now). Works
     * from either mode per W25Q datasheet §8.2.24. */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x66u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x99u;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    /* tRST = 30 us per datasheet — spin wait. */
    for (volatile uint32_t i = 0; i < 5000; i++) __asm volatile ("nop");

    /* Restore original M0 config. */
    qmi_hw->m[0].timing = orig_timing;
    qmi_hw->m[0].rfmt   = orig_rfmt;
    qmi_hw->m[0].rcmd   = orig_rcmd;

    /* Exit direct mode — XIP resumes at baseline config. */
    qmi_hw->direct_csr = 0;
    __asm volatile ("dsb sy" ::: "memory");

    restore_interrupts(irq_save);
}

void flash_try_qpi(void) {
    printf("\n--- Flash QPI experiment (0x35 Enter / 0xF5 Exit, CLKDIV=1) ---\n");
    uint32_t expected = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    printf("  Expected: 0x%08X\n", (unsigned)expected);

    struct qpi_result r = { .expected = expected };
    flash_try_qpi_run(&r);

    printf("  M0.timing tested: 0x%08X (CLKDIV=1 RXDELAY=2)\n",
           (unsigned)r.m0_timing_used);
    printf("  M0.rfmt   tested: 0x%08X (PREFIX=Q ADDR=Q DUMMY=Q/24b DATA=Q)\n",
           (unsigned)r.m0_rfmt_used);
    printf("  read_val: 0x%08X  %s\n", (unsigned)r.read_val,
           r.pass ? "MATCH — QPI works at 75 MHz!" : "MISMATCH");
    if (r.pass) {
        printf("  Bench 16 KB: %u us = %u KB/s\n",
               (unsigned)r.us, (unsigned)r.kbps);
    }

    /* Confirm normal XIP is back (read MSP after restore). */
    uint32_t recheck = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    printf("  Post-restore recheck @ baseline: 0x%08X  %s\n",
           (unsigned)recheck,
           recheck == expected ? "OK (flash back in SPI mode)"
                               : "BAD — flash stuck in QPI, run 'flash_reset'");
}

/* ---------------------------------------------------------------------
 * flash_reset — emergency recovery. Sends 0x66 + 0x99 (Reset Enable +
 * Reset) to flash via direct mode. Tries both SPI and QPI modes.
 * Use after flash_try_qpi if the post-restore recheck fails.
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
 * flash_try_114 — test 1-1-4 mode (6Bh Fast Read Quad Output) at
 * CLKDIV=1. In 1-1-4 the cmd + addr are BOTH single-bit, only data
 * is quad. This gives the device 40 single-bit clocks of settling
 * before the IO direction switch to quad-data (vs 1-4-4 where addr
 * is already quad and switch happens much earlier).
 *
 * Datasheet §9.5: 6Bh requires 8 dummy clocks, max freq 104 MHz.
 * Sweep RXDELAY 1..5 to find the data-sampling sweet spot.
 * ------------------------------------------------------------------- */
struct fs114_result {
    uint8_t  rxdelay;
    uint32_t read_val;
    bool     pass;
    uint32_t us, kbps;
};

void __no_inline_not_in_flash_func(flash_try_114_run)(
        struct fs114_result *res, int n, uint32_t expected) {
    uint32_t orig_timing = qmi_hw->m[0].timing;
    uint32_t orig_rfmt   = qmi_hw->m[0].rfmt;
    uint32_t orig_rcmd   = qmi_hw->m[0].rcmd;

    /* rfmt for 1-1-4: PREFIX=S(8 clk), ADDR=S, DATA=Q, DUMMY=Q(8 clk).
     * No SUFFIX (6Bh has no M-byte). DUMMY_LEN field value:
     *   8 Q-clocks would need a value of 8 but field is 3 bits (0..7).
     *   We can use DUMMY_WIDTH=S instead: 8 S-clocks = DUMMY_LEN field
     *   value = 2 (8 bits) with width S... wait DUMMY_LEN is in bits.
     *   8 S-clocks = 8 bits = DUMMY_LEN_VALUE_8 field value 2, width S.
     *   That gives exactly 8 dummy clocks on single wire. */
    uint32_t rfmt =
        (QMI_M0_RFMT_PREFIX_WIDTH_VALUE_S << QMI_M0_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M0_RFMT_ADDR_WIDTH_VALUE_S   << QMI_M0_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M0_RFMT_DUMMY_WIDTH_VALUE_S  << QMI_M0_RFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB)   |
        (QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB)   |
        (QMI_M0_RFMT_DUMMY_LEN_VALUE_8    << QMI_M0_RFMT_DUMMY_LEN_LSB);
    uint32_t rcmd = (0x6Bu << QMI_M0_RCMD_PREFIX_LSB);

    for (int i = 0; i < n; i++) {
        uint32_t irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing =
            (1u << QMI_M0_TIMING_COOLDOWN_LSB) |
            ((uint32_t)res[i].rxdelay << QMI_M0_TIMING_RXDELAY_LSB) |
            (1u << QMI_M0_TIMING_CLKDIV_LSB);
        qmi_hw->m[0].rfmt = rfmt;
        qmi_hw->m[0].rcmd = rcmd;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);

        uint32_t val = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
        res[i].read_val = val;
        res[i].pass = (val == expected);

        if (res[i].pass) {
            volatile uint32_t *src = (volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
            uint32_t acc = 0;
            uint32_t t0 = timer_hw->timerawl;
            for (uint32_t k = 0; k < FLASH_BENCH_WORDS; k++) acc ^= src[k];
            res[i].us = timer_hw->timerawl - t0;
            if (res[i].us)
                res[i].kbps = (FLASH_BENCH_WORDS * 4u * 1000u) / res[i].us;
            else
                res[i].kbps = 0;
            (void)acc;
        } else {
            res[i].us = 0; res[i].kbps = 0;
        }

        irq_save = save_and_disable_interrupts();
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        qmi_hw->m[0].timing = orig_timing;
        qmi_hw->m[0].rfmt   = orig_rfmt;
        qmi_hw->m[0].rcmd   = orig_rcmd;
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
        restore_interrupts(irq_save);
    }
}

void flash_try_114(void) {
    printf("\n--- Flash 1-1-4 mode (6Bh) sweep at CLKDIV=1 (75 MHz) ---\n");
    uint32_t expected = *(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    printf("  Expected: 0x%08X\n", (unsigned)expected);
    printf("  rfmt: PREFIX=S/8b, ADDR=S, DUMMY=S/8b (8 dummy clocks), DATA=Q\n");
    printf("  rcmd: PREFIX=0x6B (Fast Read Quad Output)\n\n");

    struct fs114_result res[5];
    for (int i = 0; i < 5; i++) res[i].rxdelay = (uint8_t)(i + 1);
    flash_try_114_run(res, 5, expected);

    printf("  RXDELAY  read_val    KB/s   result\n");
    printf("  -------  ----------  -----  ------\n");
    for (int i = 0; i < 5; i++) {
        printf("  %7u  0x%08X  %5u  %s\n",
               (unsigned)res[i].rxdelay,
               (unsigned)res[i].read_val,
               (unsigned)res[i].kbps,
               res[i].pass ? "PASS" : "FAIL");
    }
}

/* ---------------------------------------------------------------------
 * flash_boost_drv — write Status Register-3 via volatile path
 * (0x50 + 0x11) to set DRV1=DRV0=0 (100% drive). Factory default is
 * 11 = 25% — may explain 75 MHz nibble-shift failures since the
 * output edge rate at 25% can't settle in a 13.3 ns half-period.
 *
 * Volatile write does not persist after power cycle → safe for
 * experimentation. flash_boost_drv_read prints SR3 so we can verify.
 * ------------------------------------------------------------------- */
/* Direct-mode helpers, copied from PSRAM pattern and adapted for CS0.
 * OE=1, NOPUSH=1: transmit without RX capture (for cmd/data bytes).
 * OE=1, NOPUSH=0: drive dummy byte and capture RX (for response). */
static bool __no_inline_not_in_flash_func(flash_direct_tx)(uint8_t b) {
    qmi_hw->direct_tx = (1u << 20) | (1u << 19) | b;  /* NOPUSH | OE | data */
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS));
    for (uint32_t t = 125000; t; --t)
        if (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)) return true;
    return false;
}

static bool __no_inline_not_in_flash_func(flash_direct_rx)(uint8_t *out) {
    qmi_hw->direct_tx = (1u << 19) | 0x00u;  /* OE | dummy, NOPUSH=0 captures RX */
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS));
    for (uint32_t t = 125000; t; --t) {
        if (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)) {
            *out = (uint8_t)qmi_hw->direct_rx;
            return true;
        }
    }
    *out = 0xEE;
    return false;
}

static void __no_inline_not_in_flash_func(flash_sr3_read_write_run)(
        uint8_t *sr3_before, uint8_t *sr3_after, uint8_t drv_bits) {
    uint32_t irq_save = save_and_disable_interrupts();

    /* Enter direct mode at CLKDIV=30 (~5 MHz). AUTO_CS0N toggles CS0
     * around each direct_tx write, so we do one command+response per tx
     * sequence (bracketed by the auto-toggle). */
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) |
                         QMI_DIRECT_CSR_EN_BITS |
                         QMI_DIRECT_CSR_AUTO_CS0N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);
    /* Drain any stale RX from a previous session. */
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS))
        (void)qmi_hw->direct_rx;

    /* AUTO_CS0N deasserts CS between direct_tx writes. To keep CS LOW
     * across a multi-byte transaction, we switch to ASSERT_CS0N (manual
     * assert) instead of AUTO. */
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) |
                         QMI_DIRECT_CSR_EN_BITS;

    #define FLASH_CS_LOW()  (qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | \
                                                  QMI_DIRECT_CSR_EN_BITS |             \
                                                  QMI_DIRECT_CSR_ASSERT_CS0N_BITS)
    #define FLASH_CS_HIGH() (qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | \
                                                  QMI_DIRECT_CSR_EN_BITS)

    /* --- Read SR3 (0x15 + 1 response byte) --- */
    FLASH_CS_LOW();
    flash_direct_tx(0x15);
    flash_direct_rx(sr3_before);
    FLASH_CS_HIGH();

    /* --- Volatile Status Register Write Enable (0x50) --- */
    FLASH_CS_LOW();
    flash_direct_tx(0x50);
    FLASH_CS_HIGH();

    /* --- Write SR3 (0x11 + data byte) with new DRV bits.
     *     SR3 bits: S16:WPS S17:R S18:R S19:R S20:R S21:DRV0 S22:DRV1 S23:R
     *     drv_bits input is the byte value where bit1=DRV1, bit0=DRV0
     *     mapped into SR3: shift left by 5 so bit5=DRV0, bit6=DRV1. */
    uint8_t sr3_new = (*sr3_before & ~0x60u) | ((drv_bits & 0x03u) << 5);
    FLASH_CS_LOW();
    flash_direct_tx(0x11);
    flash_direct_tx(sr3_new);
    FLASH_CS_HIGH();

    /* --- Read SR3 back to verify --- */
    FLASH_CS_LOW();
    flash_direct_tx(0x15);
    flash_direct_rx(sr3_after);
    FLASH_CS_HIGH();

    qmi_hw->direct_csr = 0;
    __asm volatile ("dsb sy" ::: "memory");
    restore_interrupts(irq_save);

    #undef FLASH_CS_LOW
    #undef FLASH_CS_HIGH
}

static void __no_inline_not_in_flash_func(flash_sr3_read_run)(uint8_t *sr3) {
    uint32_t irq_save = save_and_disable_interrupts();
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS))
        (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr = (30u << QMI_DIRECT_CSR_CLKDIV_LSB) |
                         QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS0N_BITS;
    flash_direct_tx(0x15);
    flash_direct_rx(sr3);
    qmi_hw->direct_csr = 0;
    __asm volatile ("dsb sy" ::: "memory");
    restore_interrupts(irq_save);
}

void flash_read_sr3(void) {
    uint8_t s = 0;
    flash_sr3_read_run(&s);
    printf("\n--- Flash SR3 = 0x%02X  DRV=%u%u (%s)\n",
           s, (s >> 6) & 1, (s >> 5) & 1,
           ((s >> 5) & 3) == 0 ? "100%%" :
           ((s >> 5) & 3) == 1 ? "50%%" :
           ((s >> 5) & 3) == 2 ? "75%%" : "25%%");
}

void flash_boost_drv(void) {
    printf("\n--- Flash DRV boost (SR3 DRV1/DRV0 -> 00 = 100%%) ---\n");
    uint8_t before = 0, after = 0;
    flash_sr3_read_write_run(&before, &after, 0x00);
    printf("  SR3 before: 0x%02X  DRV=%u%u (%s)\n",
           before, (before >> 6) & 1, (before >> 5) & 1,
           ((before >> 5) & 3) == 0 ? "100%%" :
           ((before >> 5) & 3) == 1 ? "50%%"  :
           ((before >> 5) & 3) == 2 ? "75%%"  : "25%%");
    printf("  SR3 after : 0x%02X  DRV=%u%u (%s)\n",
           after, (after >> 6) & 1, (after >> 5) & 1,
           ((after >> 5) & 3) == 0 ? "100%%" :
           ((after >> 5) & 3) == 1 ? "50%%"  :
           ((after >> 5) & 3) == 2 ? "75%%"  : "25%%");
    printf("  (Volatile write — reverts on power cycle)\n");
    printf("  Now try flash_sweep2 or flash_probe_75 at CLKDIV=1.\n");
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
