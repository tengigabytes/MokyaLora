#include "bringup.h"

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
// PSRAM test (APS6404L, 8 MB, CS = GPIO 0 / QMI CS1n)
//
// pico-sdk 2.2.0 has no hardware/psram.h — use QMI registers directly.
// Physical address map (RP2350B, 16 MB flash on CS0):
//   CS0 physical 0x000000..0xFFFFFF  (flash)
//   CS1 physical 0x800000..0xFFFFFF  (PSRAM, 8 MB on M1)
// Default ATRANS[2,3] already map virtual 0x800000..0xFFFFFF → M1.
// XIP uncached access: XIP_NOCACHE_NOALLOC_BASE + 0x800000.
// ---------------------------------------------------------------------------

// Must run from RAM: QMI direct mode pauses XIP.
// Sends Reset-Enable (0x66), Reset (0x99), Enter-QPI (0x35) to CS1 via SPI.
static void __no_inline_not_in_flash_func(psram_enter_qpi)(void) {
    // Disable interrupts: XIP is paused during direct mode;
    // any flash-resident ISR (e.g. USB CDC) would hang if allowed to fire.
    uint32_t irq_save = save_and_disable_interrupts();

    // Enable direct mode
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);

    // Helper: send one byte to CS1 (SPI single-wire, OE=1, NOPUSH=1)
    #define _CS1_SEND(b) do { \
        hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS); \
        qmi_hw->direct_tx = (1u<<20)|(1u<<19)|(uint8_t)(b); \
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)); \
        while (  qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS); \
        hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS); \
    } while (0)

    _CS1_SEND(0x66);  // Reset Enable
    _CS1_SEND(0x99);  // Reset — tRST = 50 µs min

    // Busy-wait ~50 µs at 125 MHz (no sleep_ms from flash here)
    for (volatile uint32_t i = 0; i < 6250; i++) __asm volatile ("nop");

    _CS1_SEND(0x35);  // Enter QPI mode

    #undef _CS1_SEND

    // Disable direct mode — XIP resumes
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);

    restore_interrupts(irq_save);
}

void psram_test(void) {
    printf("\n--- PSRAM test (APS6404L, CS=GPIO%d) ---\n", PSRAM_CS_PIN);

    // GPIO 0 → QMI CS1n function
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);

    // Dump GPIO0 CTRL and QMI M1 registers for hardware verification
    uint32_t gpio0_ctrl = *(volatile uint32_t *)0x40028004u;  // IO_BANK0 GPIO0_CTRL
    printf("  GPIO0_CTRL : 0x%08X  FUNCSEL=%u %s\n",
           (unsigned)gpio0_ctrl, (unsigned)(gpio0_ctrl & 0x1F),
           (gpio0_ctrl & 0x1F) == 9 ? "(XIP_CS1 OK)" : "(WRONG - expected 9)");
    printf("  QMI M1 timing=0x%08X rfmt=0x%08X rcmd=0x%08X\n",
           (unsigned)qmi_hw->m[1].timing,
           (unsigned)qmi_hw->m[1].rfmt,
           (unsigned)qmi_hw->m[1].rcmd);

    // Step 1a: QPI probe — bootrom may have already put PSRAM in QPI mode.
    qmi_hw->m[1].timing = qmi_hw->m[0].timing;
    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_NOCACHE;
    uint32_t qpi_probe = psram[0];
    printf("  QPI probe (bootrom config, rcmd=0xEB @ 0): 0x%08X  [%s]\n",
           (unsigned)qpi_probe,
           qpi_probe != 0xFFFFFFFF ? "bus alive (QPI mode)" : "no response");

    // Step 1b: SPI single-wire probe — reset to SPI mode first, then probe.
    qmi_hw->m[1].rfmt   = QMI_M1_RFMT_PREFIX_LEN_BITS;  // all fields 0 = single-wire
    qmi_hw->m[1].rcmd   = 0x03;
    qmi_hw->m[1].wfmt   = QMI_M1_WFMT_PREFIX_LEN_BITS;
    qmi_hw->m[1].wcmd   = 0x02;
    uint32_t spi_probe = psram[0];
    printf("  SPI probe  (single-wire, rcmd=0x03 @ 0):  0x%08X  [%s]\n",
           (unsigned)spi_probe,
           spi_probe != 0xFFFFFFFF ? "bus alive (SPI mode)" : "no response");

    if (qpi_probe == 0xFFFFFFFF && spi_probe == 0xFFFFFFFF) {
        printf("  Both probes failed -> hardware issue (CS1/QSPI lines or chip)\n");
        return;
    }

    // Enter QPI mode (SPI→QPI: 0x66 Reset-Enable, 0x99 Reset, 0x35 Enter-QPI)
    psram_enter_qpi();

    // Step 2: configure M1 for QPI quad read/write
    qmi_hw->m[1].rfmt =
        QMI_M1_RFMT_PREFIX_LEN_BITS                   |  // 8-bit prefix
        (2u << QMI_M0_RFMT_PREFIX_WIDTH_LSB)           |  // quad prefix
        (2u << QMI_M0_RFMT_ADDR_WIDTH_LSB)             |  // quad addr
        (6u << QMI_M0_RFMT_DUMMY_LEN_LSB)              |  // 6 dummy cycles
        (2u << QMI_M0_RFMT_DUMMY_WIDTH_LSB)            |  // quad dummy
        (2u << QMI_M0_RFMT_DATA_WIDTH_LSB);               // quad data
    qmi_hw->m[1].rcmd = 0xEB;

    qmi_hw->m[1].wfmt =
        QMI_M1_WFMT_PREFIX_LEN_BITS                   |  // 8-bit prefix
        (2u << QMI_M0_WFMT_PREFIX_WIDTH_LSB)           |  // quad prefix
        (2u << QMI_M0_WFMT_ADDR_WIDTH_LSB)             |  // quad addr
        (2u << QMI_M0_WFMT_DATA_WIDTH_LSB);               // quad data
    qmi_hw->m[1].wcmd = 0x38;

    // Pattern test via uncached XIP alias
    printf("  QPI pattern test (0xEB read, 0x38 write)...\n");

    for (int i = 0; i < PSRAM_TEST_WORDS; i++)
        psram[i] = (uint32_t)(0xA5000000u | (uint32_t)i);

    int errors = 0;
    for (int i = 0; i < PSRAM_TEST_WORDS; i++) {
        uint32_t expected = (uint32_t)(0xA5000000u | (uint32_t)i);
        if (psram[i] != expected) {
            if (errors < 4)
                printf("  ERR @ +0x%04X: wr 0x%08X rd 0x%08X\n",
                       i * 4, (unsigned)expected, (unsigned)psram[i]);
            errors++;
        }
    }
    printf("  Pattern (4 KB): %s", errors == 0 ? "PASS" : "FAIL");
    if (errors) printf(" (%d errors)", errors);
    printf("\n");
}
