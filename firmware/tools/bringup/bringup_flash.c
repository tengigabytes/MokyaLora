#include "bringup.h"
#include "hardware/clocks.h"

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
