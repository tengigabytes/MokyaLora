#include "bringup.h"

// ---------------------------------------------------------------------------
// Internal SRAM test (RP2350B, 520 KB)
//
// Uses a 16 KB static buffer in .bss to avoid stack/heap conflicts.
// Tests with 5 patterns: 0xAAAAAAAA, 0x55555555, 0xFF00FF00, 0x00FF00FF,
// and an address-based pattern. Reports buffer address so the bank is visible.
//
// Static usage is derived from linker symbols:
//   __data_start__ — start of RAM (base of .data section, = 0x20000000)
//   __bss_end__    — end of .bss  (top of all statically allocated RAM)
//   __StackTop     — top of SRAM  (= 0x20082000, 520 KB boundary)
// ---------------------------------------------------------------------------

#define SRAM_TEST_WORDS 4096   // 16 KB
#define SRAM_TOTAL_KB   520u   // RP2350B: 520 KB internal SRAM

extern char __data_start__;
extern char __bss_end__;
extern char __StackTop;

static uint32_t sram_test_buf[SRAM_TEST_WORDS];

void sram_test(void) {
    uint32_t total    = SRAM_TOTAL_KB * 1024u;
    uint32_t static_used = (uint32_t)((uintptr_t)&__bss_end__ -
                                      (uintptr_t)&__data_start__);
    uint32_t free_ram    = (uint32_t)((uintptr_t)&__StackTop -
                                      (uintptr_t)&__bss_end__);
    uint32_t pct = (static_used * 100u) / total;

    printf("\n--- Internal SRAM (RP2350B, 520 KB) ---\n");
    printf("  Total        : %u KB  (0x%08X – 0x%08X)\n",
           total / 1024,
           (unsigned)(uintptr_t)&__data_start__,
           (unsigned)(uintptr_t)&__StackTop);
    printf("  Static used  : %u KB  (.data + .bss)  %u%%\n",
           static_used / 1024, pct);
    printf("  Free (heap+stack): %u KB\n", free_ram / 1024);
    printf("  Buffer @ 0x%08X  size=%u KB\n",
           (unsigned)(uintptr_t)sram_test_buf,
           (unsigned)(SRAM_TEST_WORDS * 4 / 1024));

    static const uint32_t patterns[] = {
        0xAAAAAAAAu, 0x55555555u, 0xFF00FF00u, 0x00FF00FFu
    };
    int total_errors = 0;

    for (int p = 0; p < (int)(sizeof(patterns)/sizeof(patterns[0])); p++) {
        uint32_t pat = patterns[p];
        for (int i = 0; i < SRAM_TEST_WORDS; i++) sram_test_buf[i] = pat;
        int errs = 0;
        for (int i = 0; i < SRAM_TEST_WORDS; i++) {
            if (sram_test_buf[i] != pat) {
                if (errs < 2)
                    printf("  ERR pat=0x%08X @ +0x%04X: rd=0x%08X\n",
                           (unsigned)pat, i * 4, (unsigned)sram_test_buf[i]);
                errs++;
            }
        }
        printf("  Pattern 0x%08X: %s\n", (unsigned)pat,
               errs == 0 ? "PASS" : "FAIL");
        total_errors += errs;
    }

    // Address-based pattern
    for (int i = 0; i < SRAM_TEST_WORDS; i++) sram_test_buf[i] = 0xA5000000u | (uint32_t)i;
    int errs = 0;
    for (int i = 0; i < SRAM_TEST_WORDS; i++) {
        uint32_t expected = 0xA5000000u | (uint32_t)i;
        if (sram_test_buf[i] != expected) {
            if (errs < 2)
                printf("  ERR addr-pat @ +0x%04X: exp=0x%08X rd=0x%08X\n",
                       i * 4, (unsigned)expected, (unsigned)sram_test_buf[i]);
            errs++;
        }
    }
    printf("  Address pattern:       %s\n", errs == 0 ? "PASS" : "FAIL");
    total_errors += errs;

    printf("  Result: %s\n", total_errors == 0 ? "PASS" : "FAIL");
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
    _CS1_SEND(0x99);  // Reset — tRST = 100 µs min (APS6404L spec)

    // Busy-wait ~100 µs at 125 MHz (no sleep_ms from flash here)
    for (volatile uint32_t i = 0; i < 12500; i++) __asm volatile ("nop");

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

    // Step 1a: SPI single-wire write+read probe.
    // Write a known pattern first, then read back — avoids false failure if the
    // location happens to contain 0xFFFFFFFF after power-on.
    // QMI always drives 24-bit address; no ADDR_LEN field in RP2350 QMI.
    qmi_hw->m[1].timing = qmi_hw->m[0].timing;
    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_NOCACHE;

    qmi_hw->m[1].rfmt = QMI_M1_RFMT_PREFIX_LEN_BITS;  // 8-bit prefix, single-wire
    qmi_hw->m[1].rcmd = 0x03;
    qmi_hw->m[1].wfmt = QMI_M1_WFMT_PREFIX_LEN_BITS;
    qmi_hw->m[1].wcmd = 0x02;

    // Write two distinct words, read them back
    psram[0] = 0xA55A1234u;
    psram[1] = 0x12345678u;
    uint32_t spi_rd0 = psram[0];
    uint32_t spi_rd1 = psram[1];
    bool spi_ok = (spi_rd0 == 0xA55A1234u) && (spi_rd1 == 0x12345678u);
    printf("  SPI wr+rd [0]: wr=0xA55A1234 rd=0x%08X\n", (unsigned)spi_rd0);
    printf("  SPI wr+rd [1]: wr=0x12345678 rd=0x%08X\n", (unsigned)spi_rd1);
    printf("  SPI probe: %s\n", spi_ok ? "PASS" : "FAIL");

    // Step 1b: QPI probe using bootrom config (Quad I/O Read, command on single wire).
    // Bootrom rfmt=0x000492A8: CMD(SPI)+ADDR(quad)+MODE-BYTE(quad)+4dummy+DATA(quad).
    // Device does NOT need to be in QPI mode for this — works from default SPI state.
    // Re-use the pattern just written in Step 1a to check readback via quad path.
    qmi_hw->m[1].rfmt = 0x000492A8u;  // bootrom Quad I/O Read config
    qmi_hw->m[1].rcmd = 0xEB;
    uint32_t qpi_rd0 = psram[0];
    uint32_t qpi_rd1 = psram[1];
    bool qpi_ok = (qpi_rd0 == 0xA55A1234u) && (qpi_rd1 == 0x12345678u);
    printf("  QPI wr+rd [0]: exp=0xA55A1234 rd=0x%08X\n", (unsigned)qpi_rd0);
    printf("  QPI wr+rd [1]: exp=0x12345678 rd=0x%08X\n", (unsigned)qpi_rd1);
    printf("  QPI probe: %s\n", qpi_ok ? "PASS" : "FAIL");

    if (!spi_ok && !qpi_ok) {
        printf("  Both probes failed -> hardware issue (CS1/QSPI lines, VCC, or chip)\n");
        return;
    }

    // Enter QPI mode (SPI→QPI: 0x66 Reset-Enable, 0x99 Reset, 0x35 Enter-QPI)
    psram_enter_qpi();

    // Step 2: configure M1 for QPI quad read/write
    qmi_hw->m[1].rfmt =
        QMI_M1_RFMT_PREFIX_LEN_BITS                   |  // 8-bit prefix
        (2u << QMI_M0_RFMT_PREFIX_WIDTH_LSB)           |  // quad prefix
        (2u << QMI_M0_RFMT_ADDR_WIDTH_LSB)             |  // quad addr
        (6u << QMI_M0_RFMT_DUMMY_LEN_LSB)              |  // 6 dummy cycles (mode+wait)
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
