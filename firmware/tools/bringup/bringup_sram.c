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

#define PSRAM_TEST_WORDS 1024  // 4 KB

// psram_test — XIP read/write test. Requires psram_init() to have been called.
void psram_test(void) {
    printf("\n--- PSRAM XIP test (APS6404L, CS=GPIO%d) ---\n", PSRAM_CS_PIN);

    // Verify GPIO0 is XIP_CS1
    uint32_t gpio0_ctrl = *(volatile uint32_t *)0x40028004u;
    uint32_t funcsel = gpio0_ctrl & 0x1Fu;
    printf("  GPIO0 FUNCSEL=%u %s\n", (unsigned)funcsel,
           funcsel == 9 ? "(XIP_CS1 OK)" : "(WRONG — run psram_init first)");
    if (funcsel != 9) return;

    printf("  M1: timing=0x%08X rfmt=0x%08X rcmd=0x%02X wfmt=0x%08X wcmd=0x%02X\n",
           (unsigned)qmi_hw->m[1].timing, (unsigned)qmi_hw->m[1].rfmt,
           (unsigned)qmi_hw->m[1].rcmd,
           (unsigned)qmi_hw->m[1].wfmt, (unsigned)qmi_hw->m[1].wcmd);
    printf("  XIP_CTRL WRITABLE_M1=%u\n",
           (xip_ctrl_hw->ctrl & XIP_CTRL_WRITABLE_M1_BITS) ? 1 : 0);

    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_NOCACHE;

    // SPI write+readback (two words)
    psram[0] = 0xA55A1234u;
    psram[1] = 0x12345678u;
    uint32_t rd0 = psram[0];
    uint32_t rd1 = psram[1];
    bool pair_ok = (rd0 == 0xA55A1234u) && (rd1 == 0x12345678u);
    printf("  XIP wr+rd [0]: wr=0xA55A1234 rd=0x%08X\n", (unsigned)rd0);
    printf("  XIP wr+rd [1]: wr=0x12345678 rd=0x%08X\n", (unsigned)rd1);
    printf("  XIP pair: %s\n", pair_ok ? "PASS" : "FAIL");

    // Pattern test (4 KB)
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

// ---------------------------------------------------------------------------
// PSRAM CS diagnostic — Issue 8: GPIO0 stuck LOW investigation
//
// Tests:
//   1. Read GPIO0 pad input level and FUNCSEL before any changes
//   2. Read QMI direct_csr ASSERT_CS1N bit
//   3. Override GPIO0 to SIO output HIGH — check if MCU can drive it
//   4. Drive LOW, then HIGH again — confirm MCU control
//   5. Restore to XIP_CS1 function
// ---------------------------------------------------------------------------
void psram_diag(void) {
    printf("\n--- PSRAM CS diagnostic (GPIO0, Issue 8) ---\n");

    // 1. Read current GPIO0 state before touching anything
    uint32_t gpio0_ctrl = *(volatile uint32_t *)0x40028004u;  // IO_BANK0 GPIO0_CTRL
    uint32_t gpio0_status = *(volatile uint32_t *)0x40028000u;  // IO_BANK0 GPIO0_STATUS
    uint32_t pads0 = *(volatile uint32_t *)0x40038004u;  // PADS_BANK0 GPIO0
    uint32_t funcsel = gpio0_ctrl & 0x1Fu;
    bool pad_in = (gpio0_status >> 17) & 1u;  // INFROMPAD (bit 17)
    bool pad_out = (gpio0_status >> 9) & 1u;  // OUTTOPAD (bit 9)
    bool pad_oe = (gpio0_status >> 13) & 1u;  // OETOPAD (bit 13)

    printf("  [Before] GPIO0_CTRL=0x%08X  FUNCSEL=%u (%s)\n",
           (unsigned)gpio0_ctrl, (unsigned)funcsel,
           funcsel == 9 ? "XIP_CS1" :
           funcsel == 5 ? "SIO" :
           funcsel == 31 ? "NULL" : "other");
    printf("  [Before] GPIO0_STATUS=0x%08X\n", (unsigned)gpio0_status);
    printf("           INFROMPAD=%u  OUTTOPAD=%u  OETOPAD=%u\n",
           pad_in, pad_out, pad_oe);
    printf("  [Before] PADS_BANK0_GPIO0=0x%08X (OD=%u PUE=%u PDE=%u IE=%u)\n",
           (unsigned)pads0,
           (pads0 >> 7) & 1, (pads0 >> 3) & 1,
           (pads0 >> 2) & 1, (pads0 >> 6) & 1);

    // 2. Read QMI direct_csr — check if CS1n is asserted
    uint32_t direct_csr = qmi_hw->direct_csr;
    printf("  [Before] QMI direct_csr=0x%08X  EN=%u  ASSERT_CS1N=%u\n",
           (unsigned)direct_csr,
           (direct_csr >> 0) & 1u,
           (direct_csr >> 2) & 1u);

    // 3. Override to SIO output HIGH — does the pin actually go HIGH?
    gpio_init(PSRAM_CS_PIN);
    gpio_set_dir(PSRAM_CS_PIN, GPIO_OUT);
    gpio_put(PSRAM_CS_PIN, 1);
    busy_wait_ms(1);

    gpio0_status = *(volatile uint32_t *)0x40028000u;
    pad_in = (gpio0_status >> 17) & 1u;
    pad_out = (gpio0_status >> 9) & 1u;
    printf("  [SIO HIGH] OUTTOPAD=%u  INFROMPAD=%u  --> %s\n",
           pad_out, pad_in,
           pad_in ? "MCU can drive HIGH (no external short)" :
                    "STILL LOW — external short to GND!");

    // 4. Drive LOW, confirm it goes LOW
    gpio_put(PSRAM_CS_PIN, 0);
    busy_wait_ms(1);
    gpio0_status = *(volatile uint32_t *)0x40028000u;
    pad_in = (gpio0_status >> 17) & 1u;
    printf("  [SIO LOW]  INFROMPAD=%u  --> %s\n",
           pad_in,
           !pad_in ? "OK (LOW as expected)" : "unexpected HIGH");

    // 5. Drive HIGH again for final confirmation
    gpio_put(PSRAM_CS_PIN, 1);
    busy_wait_ms(1);
    gpio0_status = *(volatile uint32_t *)0x40028000u;
    pad_in = (gpio0_status >> 17) & 1u;
    printf("  [SIO HIGH] INFROMPAD=%u  --> %s\n",
           pad_in, pad_in ? "OK" : "STILL LOW");

    // 6. Restore to NULL (not XIP_CS1) to prevent M1 XIP from driving CS1
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_NULL);
    printf("  [Restored NULL] GPIO0 safe — no M1 XIP interference\n");

    printf("\n  Summary:\n");
    printf("  - If SIO HIGH showed INFROMPAD=0 -> PCB short to GND (check trace/solder)\n");
    printf("  - If SIO HIGH showed INFROMPAD=1 but XIP_CS1 is LOW -> QMI holding CS asserted\n");
    printf("  - If ASSERT_CS1N=1 above -> QMI direct mode left CS1 stuck; clear it\n");
}

// ---------------------------------------------------------------------------
// PSRAM SPI probe — single QMI direct mode session.
//
// All SPI transactions run in one RAM function to minimise time with XIP
// paused.  The shared QSPI bus means Flash is inaccessible while CS1 is
// asserted — each CS assertion is kept to the minimum required bytes.
//
// Sequence: Reset (0x66+0x99) → Read 4B → Write 4B → Read-back 4B → exit.
// Results stored in struct, printed AFTER direct mode exits and XIP resumes.
// ---------------------------------------------------------------------------
struct psram_probe_result {
    uint8_t read_id[8];     // Read ID (0x9F) response
    uint8_t read1[8];       // SPI Read (0x03) @ 0x000000
    uint8_t read2[4];       // read-back after write
    bool    busy_timeout;   // true if any BUSY wait timed out
    uint32_t gpio0_pad;     // PADS_BANK0 GPIO0 after pull-up config
};

// Wait for BUSY to clear with timeout (~1 ms). Returns true if OK.
static bool __no_inline_not_in_flash_func(qmi_wait_busy)(void) {
    for (uint32_t t = 125000; t; --t) {
        if (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)) return true;
    }
    return false;
}

// TX one byte, OE=1, NOPUSH=1 (no RX capture). Returns true if OK.
static bool __no_inline_not_in_flash_func(cs1_tx)(uint8_t b) {
    qmi_hw->direct_tx = (1u << 20) | (1u << 19) | b;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS));
    return qmi_wait_busy();
}

// TX dummy, OE=1, NOPUSH=0 (capture RX). Returns true if OK.
static bool __no_inline_not_in_flash_func(cs1_rx)(uint8_t *out) {
    qmi_hw->direct_tx = (1u << 19) | 0x00u;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS));
    if (!qmi_wait_busy()) { *out = 0xEE; return false; }
    *out = (uint8_t)qmi_hw->direct_rx;
    return true;
}

// Direct register addresses for GPIO0 SIO control (accessible from RAM)
// RP2350 SIO offsets differ from RP2040 due to interleaved GPIO_HI_* registers.
#define GPIO0_CTRL_REG      (*(volatile uint32_t *)0x40028004u)  // IO_BANK0 GPIO0_CTRL
#define SIO_GPIO_OUT_SET    (*(volatile uint32_t *)0xD0000018u)  // SIO + 0x18
#define SIO_GPIO_OUT_CLR    (*(volatile uint32_t *)0xD0000020u)  // SIO + 0x20
#define SIO_GPIO_OE_SET     (*(volatile uint32_t *)0xD0000038u)  // SIO + 0x38
#define GPIO0_BIT           (1u << 0)

// PADS_BANK0 GPIO0 register (accessible from RAM)
#define PADS_BANK0_GPIO0    (*(volatile uint32_t *)0x40038004u)

// ---------------------------------------------------------------------------
// psram_init — full PSRAM initialisation using SIO CS + QMI clock workaround.
//
// Sequence (all from RAM, interrupts disabled):
//   1. GPIO0 = SIO output HIGH + pull-up (deselect PSRAM, prevent boot glitch)
//   2. Enter QMI direct mode, set ASSERT_CS1N (clock generation)
//   3. SPI reset (0x66/0x99) + Read ID (0x9F) via SIO CS toggle
//   4. Configure M1 timing/rfmt/wfmt for SPI (0x03/0x02)
//   5. Switch GPIO0 to XIP_CS1 (FUNCSEL=9) — inside direct mode so M1 idle
//   6. Exit direct mode → M1 XIP engine takes over GPIO0
//   7. Enable writable on M1 window
// ---------------------------------------------------------------------------
struct psram_init_result {
    uint8_t  id[8];
    bool     id_ok;
    bool     busy_timeout;
    uint32_t m1_timing;
    uint32_t m1_rfmt;
};

static void __no_inline_not_in_flash_func(psram_init_run)(
        struct psram_init_result *r) {
    r->busy_timeout = false;
    r->id_ok = false;
    bool ok = true;

    // Phase 0: GPIO0 = SIO output HIGH + pull-up
    SIO_GPIO_OUT_SET = GPIO0_BIT;
    SIO_GPIO_OE_SET  = GPIO0_BIT;
    GPIO0_CTRL_REG   = 5u;  // FUNCSEL = SIO
    PADS_BANK0_GPIO0 = (1u << 6) | (1u << 4) | (1u << 3);  // IE=1 DRIVE=4mA PUE=1

    for (volatile uint32_t i = 0; i < 1000; i++) __asm volatile ("nop");

    // Phase 1: direct mode + ASSERT_CS1N for clock
    uint32_t irq_save = save_and_disable_interrupts();
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    #define CS_INIT_LOW()  (SIO_GPIO_OUT_CLR = GPIO0_BIT)
    #define CS_INIT_HIGH() (SIO_GPIO_OUT_SET = GPIO0_BIT)

    // Reset Enable (0x66)
    CS_INIT_LOW(); ok = cs1_tx(0x66); CS_INIT_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    // Reset (0x99) + tRST
    CS_INIT_LOW(); ok = cs1_tx(0x99); CS_INIT_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }
    for (volatile uint32_t i = 0; i < 12500; i++) __asm volatile ("nop");

    // Read ID (0x9F + 3 addr + 8 data)
    CS_INIT_LOW();
    ok = cs1_tx(0x9F) && cs1_tx(0x00) && cs1_tx(0x00) && cs1_tx(0x00);
    if (ok) {
        for (int i = 0; i < 8; i++)
            if (!cs1_rx(&r->id[i])) ok = false;
    }
    CS_INIT_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    r->id_ok = (r->id[0] == 0x0D && r->id[1] == 0x5D);

    // Phase 2: configure M1 for SPI read/write (conservative, single-wire)
    qmi_hw->m[1].timing = qmi_hw->m[0].timing;  // copy Flash timing
    qmi_hw->m[1].rfmt   = QMI_M1_RFMT_PREFIX_LEN_BITS;   // 8-bit cmd, single-wire
    qmi_hw->m[1].rcmd   = 0x03;                           // SPI Read
    qmi_hw->m[1].wfmt   = QMI_M1_WFMT_PREFIX_LEN_BITS;   // 8-bit cmd, single-wire
    qmi_hw->m[1].wcmd   = 0x02;                           // SPI Write

    r->m1_timing = qmi_hw->m[1].timing;
    r->m1_rfmt   = qmi_hw->m[1].rfmt;

    // Phase 3: switch GPIO0 to XIP_CS1 while still in direct mode
    // M1 XIP engine is paused → won't drive GPIO0 until we exit direct mode.
    GPIO0_CTRL_REG = 9u;  // FUNCSEL = XIP_CS1

exit:
    CS_INIT_HIGH();
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(irq_save);

    #undef CS_INIT_LOW
    #undef CS_INIT_HIGH
}

bool psram_init(void) {
    struct psram_init_result r;
    psram_init_run(&r);

    if (r.id_ok)
        hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    printf("[PSRAM] ID=%02X %02X -> %s  timing=0x%08X rfmt=0x%08X\n",
           r.id[0], r.id[1],
           r.id_ok ? "APS6404L OK" : "FAIL",
           (unsigned)r.m1_timing, (unsigned)r.m1_rfmt);
    if (r.busy_timeout) printf("[PSRAM] WARNING: busy timeout\n");

    return r.id_ok;
}

// ---------------------------------------------------------------------------
// psram_probe — diagnostic SPI probe (independent of psram_init).
// ---------------------------------------------------------------------------
static void __no_inline_not_in_flash_func(psram_probe_run)(
        struct psram_probe_result *r) {
    r->busy_timeout = false;
    bool ok = true;

    // --- Phase 0: GPIO0 = SIO output HIGH + internal pull-up ---
    // GPIO0 boots with PDE=1 (pull-down) → PSRAM CE# LOW → bus contention.
    // Fix: drive HIGH via SIO first, then enable pull-up, disable pull-down.
    SIO_GPIO_OUT_SET = GPIO0_BIT;           // drive HIGH (CS deasserted)
    SIO_GPIO_OE_SET  = GPIO0_BIT;           // output enable
    GPIO0_CTRL_REG   = 5u;                  // FUNCSEL = SIO

    // PADS_BANK0 GPIO0: set PUE=1, PDE=0, IE=1, OD=0
    // Default is 0x56 = ISO=0, OD=0, IE=1, DRIVE=1, PUE=0, PDE=1, SLEWFAST=0
    // Target:    0x4C = ISO=0, OD=0, IE=1, DRIVE=1, PUE=1, PDE=0, SLEWFAST=0
    PADS_BANK0_GPIO0 = (1u << 6) |  // IE=1
                       (1u << 4) |  // DRIVE=4mA (bits[5:4]=01)
                       (1u << 3);   // PUE=1, PDE=0
    r->gpio0_pad = PADS_BANK0_GPIO0;

    // Small delay to let pull-up stabilise
    for (volatile uint32_t i = 0; i < 1000; i++) __asm volatile ("nop");

    // --- Phase 1: QMI direct mode with ASSERT_CS1N for clock generation ---
    // We need BOTH:
    //   SIO GPIO0 LOW/HIGH → physical CE# to APS6404L
    //   QMI ASSERT_CS1N    → QMI generates SCLK (CS1N goes to internal pad, no ext conn)
    uint32_t irq_save = save_and_disable_interrupts();
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);

    // Assert CS1N in QMI so it generates clock when we write direct_tx.
    // This CS1N goes to internal QSPI CS1 pad (not GPIO0), harmless.
    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    // CS assert/deassert via SIO (physical CE# to PSRAM)
    #define CS_LOW()  (SIO_GPIO_OUT_CLR = GPIO0_BIT)
    #define CS_HIGH() (SIO_GPIO_OUT_SET = GPIO0_BIT)

    // --- Reset Enable (0x66) ---
    CS_LOW();
    ok = cs1_tx(0x66);
    CS_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    // --- Reset (0x99) ---
    CS_LOW();
    ok = cs1_tx(0x99);
    CS_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    // tRST ≥ 100 µs (APS6404L spec)
    for (volatile uint32_t i = 0; i < 12500; i++) __asm volatile ("nop");

    // --- Read ID (0x9F) → read_id[8] ---
    // APS6404L: 0x9F + 3 addr bytes (0x00) + 8 data bytes
    // Expected: MF ID = 0x0D (AP Memory), KGD = 0x5D, then EID[5:0]
    CS_LOW();
    ok = cs1_tx(0x9F) && cs1_tx(0x00) && cs1_tx(0x00) && cs1_tx(0x00);
    if (ok) {
        for (int i = 0; i < 8; i++)
            if (!cs1_rx(&r->read_id[i])) ok = false;
    }
    CS_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    // --- SPI Read (0x03) @ 0x000000 → read1[8] ---
    CS_LOW();
    ok = cs1_tx(0x03) && cs1_tx(0x00) && cs1_tx(0x00) && cs1_tx(0x00);
    if (ok) {
        for (int i = 0; i < 8; i++)
            if (!cs1_rx(&r->read1[i])) ok = false;
    }
    CS_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    // --- SPI Write (0x02) @ 0x000000 ← DE AD BE EF ---
    CS_LOW();
    ok = cs1_tx(0x02) && cs1_tx(0x00) && cs1_tx(0x00) && cs1_tx(0x00)
      && cs1_tx(0xDE) && cs1_tx(0xAD) && cs1_tx(0xBE) && cs1_tx(0xEF);
    CS_HIGH();
    if (!ok) { r->busy_timeout = true; goto exit; }

    // Short delay after write
    for (volatile uint32_t i = 0; i < 500; i++) __asm volatile ("nop");

    // --- SPI Read (0x03) @ 0x000000 → read2[4] ---
    CS_LOW();
    ok = cs1_tx(0x03) && cs1_tx(0x00) && cs1_tx(0x00) && cs1_tx(0x00);
    if (ok) {
        for (int i = 0; i < 4; i++)
            if (!cs1_rx(&r->read2[i])) ok = false;
    }
    CS_HIGH();
    if (!ok) r->busy_timeout = true;

exit:
    CS_HIGH();  // ensure physical CS deasserted
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    restore_interrupts(irq_save);

    // Leave GPIO0 as SIO output HIGH (safe idle state, pull-up active)
    #undef CS_LOW
    #undef CS_HIGH
}

void psram_probe(void) {
    printf("\n--- PSRAM SPI probe (SIO CS + QMI clock, GPIO0) ---\n");

    struct psram_probe_result r;
    psram_probe_run(&r);

    // Print results (safe — direct mode exited, XIP active)
    printf("  GPIO0 pad: 0x%08X (PUE=%u PDE=%u IE=%u)\n",
           (unsigned)r.gpio0_pad,
           (r.gpio0_pad >> 3) & 1, (r.gpio0_pad >> 2) & 1,
           (r.gpio0_pad >> 6) & 1);

    printf("  Read ID:   %02X %02X %02X %02X %02X %02X %02X %02X",
           r.read_id[0], r.read_id[1], r.read_id[2], r.read_id[3],
           r.read_id[4], r.read_id[5], r.read_id[6], r.read_id[7]);
    bool id_ok = (r.read_id[0] == 0x0D && r.read_id[1] == 0x5D);
    printf("  %s\n", id_ok ? "(APS6404L OK: MF=0x0D KGD=0x5D)" :
                             "(unexpected — check wiring)");

    printf("  SPI Read:  %02X %02X %02X %02X %02X %02X %02X %02X\n",
           r.read1[0], r.read1[1], r.read1[2], r.read1[3],
           r.read1[4], r.read1[5], r.read1[6], r.read1[7]);

    printf("  Write:     DE AD BE EF\n");
    printf("  Read-back: %02X %02X %02X %02X",
           r.read2[0], r.read2[1], r.read2[2], r.read2[3]);
    bool match = (r.read2[0]==0xDE && r.read2[1]==0xAD &&
                  r.read2[2]==0xBE && r.read2[3]==0xEF);
    printf("  %s\n", match ? "PASS" : "MISMATCH");

    if (r.busy_timeout) printf("  WARNING: BUSY timeout occurred\n");

    printf("  M1: timing=0x%08X rfmt=0x%08X rcmd=0x%08X\n",
           (unsigned)qmi_hw->m[1].timing, (unsigned)qmi_hw->m[1].rfmt,
           (unsigned)qmi_hw->m[1].rcmd);
}
