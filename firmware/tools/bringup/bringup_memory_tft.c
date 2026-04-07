#include "bringup.h"
#include "bringup_menu.h"
#include "hardware/clocks.h"

// ===========================================================================
// TFT consolidated diagnostics (Step 20)
// ===========================================================================

// ---------------------------------------------------------------------------
// Memory Diagnostic — consolidated SRAM + Flash + PSRAM on one TFT screen
//
// Sub-tests:
//   1. SRAM 16 KB 5-pattern test
//   2. Flash JEDEC ID (W25Q128JW: EF 60 18)
//   3. Flash QE bit (SR2 bit 1)
//   4. PSRAM init (APS6404L ID 0D 5D)
//   5. PSRAM 4 KB pattern test
// ---------------------------------------------------------------------------
void cmd_memory_diag(void) {
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / CW;

    menu_clear(MC_BG);
    menu_str(0, 0, " Memory Diagnostic  ", COLS, MC_TITLE, MC_TITBG, S);

    printf("\n--- Memory Diagnostic (consolidated) ---\n");
    int pass = 0;
    int total = 5;
    char line[24];

    // --- 1. SRAM 5-pattern test ---
    {
        static uint32_t buf[4096]; // 16 KB
        static const uint32_t pats[] = {
            0xAAAAAAAAu, 0x55555555u, 0xFF00FF00u, 0x00FF00FFu
        };
        int errs = 0;
        for (int p = 0; p < 4; p++) {
            for (int i = 0; i < 4096; i++) buf[i] = pats[p];
            for (int i = 0; i < 4096; i++)
                if (buf[i] != pats[p]) errs++;
        }
        // Address pattern
        for (int i = 0; i < 4096; i++) buf[i] = 0xA5000000u | (uint32_t)i;
        for (int i = 0; i < 4096; i++)
            if (buf[i] != (0xA5000000u | (uint32_t)i)) errs++;

        bool ok = (errs == 0);
        if (ok) pass++;
        snprintf(line, sizeof(line), " SRAM 16K 5pat  %s", ok ? " OK" : "ERR");
        menu_str(0, 2 * CH, line, COLS, ok ? MC_OK : MC_ERR, MC_BG, S);
        printf("  SRAM 16KB 5-pattern: %s (%d errors)\n", ok ? "PASS" : "FAIL", errs);
    }

    // --- 2. Flash JEDEC ID ---
    {
        uint8_t txbuf[4] = {0x9F, 0, 0, 0};
        uint8_t rxbuf[4] = {0};
        flash_do_cmd(txbuf, rxbuf, 4);
        uint8_t mfr = rxbuf[1], type = rxbuf[2], cap = rxbuf[3];
        bool ok = (mfr == 0xEF) && (type == 0x60) && (cap == 0x18);
        if (ok) pass++;
        snprintf(line, sizeof(line), " Flash %02X%02X%02X   %s",
                 mfr, type, cap, ok ? " OK" : "ERR");
        menu_str(0, 3 * CH, line, COLS, ok ? MC_OK : MC_ERR, MC_BG, S);
        printf("  Flash JEDEC %02X %02X %02X: %s\n", mfr, type, cap,
               ok ? "PASS" : "FAIL");
    }

    // --- 3. Flash QE bit ---
    {
        uint8_t sr_tx[2], sr_rx[2];
        sr_tx[0] = 0x35; sr_tx[1] = 0;
        flash_do_cmd(sr_tx, sr_rx, 2);
        bool qe = (sr_rx[1] & 0x02) != 0;
        if (qe) pass++;
        snprintf(line, sizeof(line), " Flash QE=%d SR2 %s",
                 qe ? 1 : 0, qe ? " OK" : "ERR");
        menu_str(0, 4 * CH, line, COLS, qe ? MC_OK : MC_ERR, MC_BG, S);
        printf("  Flash QE (SR2 bit1): %s\n", qe ? "SET (OK)" : "CLEAR (FAIL)");
    }

    // --- 4. PSRAM init ---
    {
        bool ok = psram_init();
        if (ok) pass++;
        snprintf(line, sizeof(line), " PSRAM Init QPI %s", ok ? " OK" : "ERR");
        menu_str(0, 5 * CH, line, COLS, ok ? MC_OK : MC_ERR, MC_BG, S);
        printf("  PSRAM Init: %s\n", ok ? "PASS" : "FAIL");

        // --- 5. PSRAM 4 KB pattern (only if init succeeded) ---
        if (ok) {
            volatile uint32_t *pm = (volatile uint32_t *)PSRAM_NOCACHE;
            for (int i = 0; i < 1024; i++) pm[i] = 0xA5000000u | (uint32_t)i;
            int errs = 0;
            for (int i = 0; i < 1024; i++)
                if (pm[i] != (0xA5000000u | (uint32_t)i)) errs++;
            bool pok = (errs == 0);
            if (pok) pass++;
            snprintf(line, sizeof(line), " PSRAM 4K pat  %s", pok ? " OK" : "ERR");
            menu_str(0, 6 * CH, line, COLS, pok ? MC_OK : MC_ERR, MC_BG, S);
            printf("  PSRAM 4KB pattern: %s (%d errors)\n",
                   pok ? "PASS" : "FAIL", errs);
        } else {
            menu_str(0, 6 * CH, " PSRAM 4K pat  SKIP", COLS, MC_HINT, MC_BG, S);
            printf("  PSRAM 4KB pattern: SKIP (init failed)\n");
        }
    }

    // --- Summary ---
    snprintf(line, sizeof(line), " Result: %d/%d pass", pass, total);
    menu_str(0, 8 * CH, line, COLS, pass == total ? MC_OK : MC_ERR, MC_BG, S);
    menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);

    printf("\n  === %d/%d tests OK ===\n", pass, total);

    while (!back_key_pressed()) sleep_ms(50);
}

// ---------------------------------------------------------------------------
// PSRAM Full 8 MB test with TFT progress display
// ---------------------------------------------------------------------------
void cmd_psram_full_tft(void) {
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / CW;

    menu_clear(MC_BG);
    menu_str(0, 0, " PSRAM Full Test    ", COLS, MC_TITLE, MC_TITBG, S);

    printf("\n--- PSRAM Full 8 MB Test (TFT) ---\n");

    // Check PSRAM init
    uint32_t gpio0_ctrl = *(volatile uint32_t *)0x40028004u;
    if ((gpio0_ctrl & 0x1Fu) != 9) {
        menu_str(0, 2 * CH, " PSRAM not init'd   ", COLS, MC_ERR, MC_BG, S);
        menu_str(0, 3 * CH, " Run MemDiag first  ", COLS, MC_HINT, MC_BG, S);
        menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
        printf("  PSRAM not initialized.\n");
        while (!back_key_pressed()) sleep_ms(50);
        return;
    }

    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_NOCACHE;
    char line[24];

    // Pass 1: Write
    menu_str(0, 2 * CH, " Writing...         ", COLS, MC_FG, MC_BG, S);
    printf("  Pass 1 — Write (8 MB):\n");
    for (uint32_t mb = 0; mb < PSRAM_SIZE_MB; mb++) {
        uint32_t base = mb * PSRAM_STEP_WORDS;
        for (uint32_t i = 0; i < PSRAM_STEP_WORDS; i++)
            psram[base + i] = 0xA5000000u | (base + i);

        snprintf(line, sizeof(line), " Write: %u/8 MB     ", (unsigned)(mb + 1));
        menu_str(0, 3 * CH, line, COLS, MC_FG, MC_BG, S);
        printf("    %u / 8 MB written\n", (unsigned)(mb + 1));

        if (back_key_pressed()) {
            menu_str(0, 8 * CH, " Cancelled          ", COLS, MC_HINT, MC_BG, S);
            menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
            while (!back_key_pressed()) sleep_ms(50);
            return;
        }
    }

    // Pass 2: Verify
    menu_str(0, 5 * CH, " Verifying...       ", COLS, MC_FG, MC_BG, S);
    printf("  Pass 2 — Verify (8 MB):\n");
    uint32_t errors = 0;
    for (uint32_t mb = 0; mb < PSRAM_SIZE_MB; mb++) {
        uint32_t base = mb * PSRAM_STEP_WORDS;
        for (uint32_t i = 0; i < PSRAM_STEP_WORDS; i++) {
            uint32_t expected = 0xA5000000u | (base + i);
            if (psram[base + i] != expected) errors++;
        }

        snprintf(line, sizeof(line), " Vfy: %u/8 err=%u",
                 (unsigned)(mb + 1), (unsigned)errors);
        menu_str(0, 6 * CH, line, COLS, MC_FG, MC_BG, S);
        printf("    %u / 8 MB verified  errors=%u\n",
               (unsigned)(mb + 1), (unsigned)errors);

        if (back_key_pressed()) {
            menu_str(0, 8 * CH, " Cancelled          ", COLS, MC_HINT, MC_BG, S);
            menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
            while (!back_key_pressed()) sleep_ms(50);
            return;
        }
    }

    // Result
    bool ok = (errors == 0);
    snprintf(line, sizeof(line), " Errors: %u         ", (unsigned)errors);
    menu_str(0, 7 * CH, line, COLS, MC_FG, MC_BG, S);
    snprintf(line, sizeof(line), " Result: %s        ", ok ? "PASS" : "FAIL");
    menu_str(0, 8 * CH, line, COLS, ok ? MC_OK : MC_ERR, MC_BG, S);
    menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);

    printf("\n  Result: %s (%u errors)\n", ok ? "PASS" : "FAIL", (unsigned)errors);

    while (!back_key_pressed()) sleep_ms(50);
}

// ---------------------------------------------------------------------------
// PSRAM Tuning — merged sweep + diag + flash_sweep, TFT summary
//
// Runs PSRAM speed sweep (CLKDIV 1-3 × RXDELAY 0-7) and Flash speed sweep
// (CLKDIV 1-4 × RXDELAY 0-3).  Displays best-case results on TFT.
// Full sweep table is printed to serial.
// ---------------------------------------------------------------------------
void cmd_psram_tuning(void) {
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / CW;

    menu_clear(MC_BG);
    menu_str(0, 0, " PSRAM/Flash Tuning ", COLS, MC_TITLE, MC_TITBG, S);

    // Check PSRAM init
    uint32_t gpio0_ctrl = *(volatile uint32_t *)0x40028004u;
    if ((gpio0_ctrl & 0x1Fu) != 9) {
        menu_str(0, 2 * CH, " PSRAM not init'd   ", COLS, MC_ERR, MC_BG, S);
        menu_str(0, 3 * CH, " Run MemDiag first  ", COLS, MC_HINT, MC_BG, S);
        menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
        printf("  PSRAM not initialized.\n");
        while (!back_key_pressed()) sleep_ms(50);
        return;
    }

    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t orig_timing = qmi_hw->m[1].timing;
    char line[24];

    menu_str(0, 2 * CH, " PSRAM sweep...     ", COLS, MC_FG, MC_BG, S);
    printf("\n--- PSRAM/Flash Tuning (TFT) ---\n");
    printf("  sys_clk = %u MHz\n", (unsigned)(sys_hz / 1000000u));

    // --- PSRAM sweep ---
    static const uint8_t clkdivs[]  = {3, 2, 1};
    static const uint8_t rxdelays[] = {0, 1, 2, 3, 4, 5, 6, 7};

    printf("\n  PSRAM Sweep (256 KB per test):\n");
    printf("  CLKDIV  RXDELAY  SCK_MHz  wr+rd_err  rdonly_err  result\n");
    printf("  ------  -------  -------  ---------  ----------  ------\n");

    uint8_t best_cd = 0;
    uint8_t best_rd = 0;
    uint32_t best_mhz = 0;
    int psram_pass_count = 0;

    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 8; r++) {
            uint8_t cd = clkdivs[c];
            uint8_t rd = rxdelays[r];
            uint32_t sck_mhz = sys_hz / (2u * cd) / 1000000u;

            // Write+read at candidate speed
            psram_set_timing(cd, rd);
            uint32_t wr_err = psram_sweep_pass();

            // Write at safe speed, read at candidate speed
            psram_set_timing(2, 0);
            volatile uint32_t *pm = (volatile uint32_t *)PSRAM_NOCACHE;
            for (uint32_t i = 0; i < SWEEP_WORDS; i++)
                pm[i] = 0xA5000000u | i;
            psram_set_timing(cd, rd);
            uint32_t rd_err = psram_verify_pass();

            bool ok = (wr_err == 0) && (rd_err == 0);
            if (ok) {
                psram_pass_count++;
                if (sck_mhz > best_mhz) {
                    best_mhz = sck_mhz;
                    best_cd = cd;
                    best_rd = rd;
                }
            }

            printf("  %6u  %7u  %7u  %9u  %10u  %s\n",
                   cd, rd, sck_mhz, (unsigned)wr_err, (unsigned)rd_err,
                   ok ? "PASS" : "FAIL");

            if (back_key_pressed()) goto tuning_done;
        }
    }

    // Restore PSRAM timing
    psram_set_full_timing(orig_timing);

    // Display PSRAM results on TFT
    if (best_mhz > 0) {
        snprintf(line, sizeof(line), " Best: CD%u RD%u %uMHz",
                 best_cd, best_rd, (unsigned)best_mhz);
        menu_str(0, 3 * CH, line, COLS, MC_OK, MC_BG, S);
    } else {
        menu_str(0, 3 * CH, " No PASS combo      ", COLS, MC_ERR, MC_BG, S);
    }
    snprintf(line, sizeof(line), " Pass: %d/24 combos ", psram_pass_count);
    menu_str(0, 4 * CH, line, COLS, MC_FG, MC_BG, S);

    // --- Flash sweep ---
    menu_str(0, 6 * CH, " Flash sweep...     ", COLS, MC_FG, MC_BG, S);
    printf("\n  Flash (M0) Speed Sweep:\n");

    #define TUNE_FLASH_COMBOS 16
    uint8_t cd_arr[TUNE_FLASH_COMBOS], rd_arr[TUNE_FLASH_COMBOS];
    struct flash_speed_result fres[TUNE_FLASH_COMBOS];
    int idx = 0;
    for (uint8_t cd = 1; cd <= 4; cd++)
        for (uint8_t rd = 0; rd <= 3; rd++) {
            cd_arr[idx] = cd;
            rd_arr[idx] = rd;
            idx++;
        }
    flash_speed_run(fres, TUNE_FLASH_COMBOS, cd_arr, rd_arr, sys_hz);

    printf("  CLKDIV  RXDELAY  SCK_MHz  read_val    result\n");
    int flash_pass_count = 0;
    uint32_t flash_best_mhz = 0;
    uint8_t flash_best_cd = 0, flash_best_rd = 0;
    for (int i = 0; i < TUNE_FLASH_COMBOS; i++) {
        printf("  %6u  %7u  %7u  0x%08X  %s\n",
               fres[i].clkdiv, fres[i].rxdelay, fres[i].sck_mhz,
               (unsigned)fres[i].read_val,
               fres[i].pass ? "PASS" : "FAIL");
        if (fres[i].pass) {
            flash_pass_count++;
            if (fres[i].sck_mhz > flash_best_mhz) {
                flash_best_mhz = fres[i].sck_mhz;
                flash_best_cd = fres[i].clkdiv;
                flash_best_rd = fres[i].rxdelay;
            }
        }
    }
    #undef TUNE_FLASH_COMBOS

    if (flash_best_mhz > 0) {
        snprintf(line, sizeof(line), " Best: CD%u RD%u %uMHz",
                 flash_best_cd, flash_best_rd, (unsigned)flash_best_mhz);
        menu_str(0, 7 * CH, line, COLS, MC_OK, MC_BG, S);
    } else {
        menu_str(0, 7 * CH, " No PASS combo      ", COLS, MC_ERR, MC_BG, S);
    }
    snprintf(line, sizeof(line), " Pass: %d/16 combos ", flash_pass_count);
    menu_str(0, 8 * CH, line, COLS, MC_FG, MC_BG, S);

tuning_done:
    psram_set_full_timing(orig_timing);
    menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
    printf("\n  M1.timing restored to 0x%08X\n", (unsigned)orig_timing);

    while (!back_key_pressed()) sleep_ms(50);
}

// ---------------------------------------------------------------------------
// PSRAM Debug — init + XIP sentinel + M1 timing info, TFT display
//
// Sub-tests:
//   1. PSRAM Init (psram_init: SPI reset, Read ID, QPI, XIP config)
//   2. XIP Sentinel (4 words write+readback via memory-mapped XIP)
// Low-level SPI probe is available via serial command 'psram_probe'.
// ---------------------------------------------------------------------------
void cmd_psram_debug(void) {
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / CW;

    menu_clear(MC_BG);
    menu_str(0, 0, " PSRAM Debug        ", COLS, MC_TITLE, MC_TITBG, S);

    printf("\n--- PSRAM Debug (init + sentinel) ---\n");
    char line[24];
    int pass = 0;
    int total = 2;

    // --- 1. PSRAM Init ---
    menu_str(0, 2 * CH, " Initializing...    ", COLS, MC_FG, MC_BG, S);

    bool init_ok = psram_init();
    if (init_ok) pass++;
    snprintf(line, sizeof(line), " Init QPI      %s", init_ok ? " OK" : "ERR");
    menu_str(0, 2 * CH, line, COLS, init_ok ? MC_OK : MC_ERR, MC_BG, S);
    printf("  PSRAM Init: %s\n", init_ok ? "PASS" : "FAIL");

    // --- 2. XIP Sentinel ---
    if (init_ok) {
        volatile uint32_t *pm = (volatile uint32_t *)PSRAM_NOCACHE;
        static const uint32_t sentinel[4] = {
            0xDEADBEEFu, 0xA5A5A5A5u, 0xCAFEBABEu, 0x12345678u
        };
        for (int i = 0; i < 4; i++) pm[i] = sentinel[i];

        bool all_ok = true;
        for (int i = 0; i < 4; i++) {
            if (pm[i] != sentinel[i]) all_ok = false;
        }
        if (all_ok) pass++;

        snprintf(line, sizeof(line), " XIP sentinel  %s", all_ok ? " OK" : "ERR");
        menu_str(0, 3 * CH, line, COLS, all_ok ? MC_OK : MC_ERR, MC_BG, S);
        printf("  XIP sentinel: %s\n", all_ok ? "PASS" : "FAIL");

        // Print J-Link instructions to serial
        printf("  J-Link command: mem32 0x%08X 4\n", (unsigned)PSRAM_NOCACHE);
        printf("  Expected: %08X %08X %08X %08X\n",
               (unsigned)sentinel[0], (unsigned)sentinel[1],
               (unsigned)sentinel[2], (unsigned)sentinel[3]);
    } else {
        menu_str(0, 3 * CH, " XIP sentinel  SKIP", COLS, MC_HINT, MC_BG, S);
        printf("  XIP sentinel: SKIP (init failed)\n");
        total = 1;
    }

    // M1 register info
    snprintf(line, sizeof(line), " M1t:%08X",
             (unsigned)qmi_hw->m[1].timing);
    menu_str(0, 5 * CH, line, COLS, MC_FG, MC_BG, S);
    snprintf(line, sizeof(line), " M1r:%08X",
             (unsigned)qmi_hw->m[1].rfmt);
    menu_str(0, 6 * CH, line, COLS, MC_FG, MC_BG, S);

    // Summary
    snprintf(line, sizeof(line), " Result: %d/%d pass", pass, total);
    menu_str(0, 8 * CH, line, COLS, pass == total ? MC_OK : MC_ERR, MC_BG, S);
    menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);

    printf("\n  === %d/%d tests OK ===\n", pass, total);

    while (!back_key_pressed()) sleep_ms(50);
}

// ---------------------------------------------------------------------------
// PSRAM Speed Test — CPU throughput measurement
//
// DMA access to PSRAM through XIP is unreliable (~37.5% address errors).
// This test uses CPU volatile access (proven correct by psram_full_test).
//
// Measures:
//   1. CPU Write 8 MB (fixed pattern, timed)
//   2. CPU Read+Verify 8 MB (timed)
//   3. Per-word timing (ns/word, sys_clk cycles)
// ---------------------------------------------------------------------------

void cmd_psram_dma_test(void) {
    const int S = 2;
    const int CW = 6 * S;
    const int CH = 8 * S;
    const int W  = menu_tft_width();
    const int COLS = W / CW;

    menu_clear(MC_BG);
    menu_str(0, 0, " PSRAM Speed Test   ", COLS, MC_TITLE, MC_TITBG, S);

    printf("\n--- PSRAM Speed Test (CPU XIP) ---\n");

    // Check PSRAM init
    uint32_t gpio0_ctrl = *(volatile uint32_t *)0x40028004u;
    if ((gpio0_ctrl & 0x1Fu) != 9) {
        menu_str(0, 2 * CH, " PSRAM not init'd   ", COLS, MC_ERR, MC_BG, S);
        menu_str(0, 3 * CH, " Run MemDiag first  ", COLS, MC_HINT, MC_BG, S);
        menu_str(0, 10 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
        while (!back_key_pressed()) sleep_ms(50);
        return;
    }

    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_NOCACHE;
    uint32_t pattern = 0xA5A5A5A5u;
    char line[24];
    uint32_t t0, t1;
    uint32_t sys_hz = clock_get_hz(clk_sys);

    // === 1. CPU Write 8 MB ===
    menu_str(0, 2 * CH, " Writing 8 MB...    ", COLS, MC_FG, MC_BG, S);

    t0 = to_ms_since_boot(get_absolute_time());
    for (uint32_t i = 0; i < PSRAM_FULL_WORDS; i++)
        psram[i] = pattern;
    t1 = to_ms_since_boot(get_absolute_time());
    uint32_t wr_ms = t1 - t0;
    uint32_t wr_kbps = wr_ms ? (8u * 1024u * 1000u / wr_ms) : 0;

    snprintf(line, sizeof(line), " Wr: %ums %uKB/s",
             (unsigned)wr_ms, (unsigned)wr_kbps);
    menu_str(0, 2 * CH, line, COLS, MC_FG, MC_BG, S);
    printf("  CPU Write 8MB: %u ms  (%u KB/s)\n",
           (unsigned)wr_ms, (unsigned)wr_kbps);

    if (back_key_pressed()) goto speed_done;

    // === 2. CPU Read+Verify 8 MB ===
    menu_str(0, 3 * CH, " Verifying 8 MB...  ", COLS, MC_FG, MC_BG, S);

    uint32_t errs = 0;
    t0 = to_ms_since_boot(get_absolute_time());
    for (uint32_t i = 0; i < PSRAM_FULL_WORDS; i++)
        if (psram[i] != pattern) errs++;
    t1 = to_ms_since_boot(get_absolute_time());
    uint32_t rd_ms = t1 - t0;
    uint32_t rd_kbps = rd_ms ? (8u * 1024u * 1000u / rd_ms) : 0;

    snprintf(line, sizeof(line), " Rd: %ums %uKB/s",
             (unsigned)rd_ms, (unsigned)rd_kbps);
    menu_str(0, 3 * CH, line, COLS,
             errs == 0 ? MC_FG : MC_ERR, MC_BG, S);
    printf("  CPU Read 8MB:  %u ms  (%u KB/s)  errors=%u\n",
           (unsigned)rd_ms, (unsigned)rd_kbps, (unsigned)errs);

    // === Per-word timing ===
    {
        uint32_t wr_ns = wr_ms ? (uint32_t)((uint64_t)wr_ms * 1000000ULL / PSRAM_FULL_WORDS) : 0;
        uint32_t rd_ns = rd_ms ? (uint32_t)((uint64_t)rd_ms * 1000000ULL / PSRAM_FULL_WORDS) : 0;
        uint32_t wr_clk = wr_ns * (sys_hz / 1000000u) / 1000u;
        uint32_t rd_clk = rd_ns * (sys_hz / 1000000u) / 1000u;

        snprintf(line, sizeof(line), " Wr:%uns %uclk",
                 (unsigned)wr_ns, (unsigned)wr_clk);
        menu_str(0, 7 * CH, line, COLS, MC_FG, MC_BG, S);
        snprintf(line, sizeof(line), " Rd:%uns %uclk",
                 (unsigned)rd_ns, (unsigned)rd_clk);
        menu_str(0, 8 * CH, line, COLS, MC_FG, MC_BG, S);

        printf("  Write: %u ns/word (%u sys_clk)\n",
               (unsigned)wr_ns, (unsigned)wr_clk);
        printf("  Read:  %u ns/word (%u sys_clk)\n",
               (unsigned)rd_ns, (unsigned)rd_clk);
    }

    // === Result summary ===
    snprintf(line, sizeof(line), " %s  err=%u",
             errs == 0 ? "PASS" : "FAIL", (unsigned)errs);
    menu_str(0, 10 * CH, line, COLS,
             errs == 0 ? MC_OK : MC_ERR, MC_BG, S);

speed_done:
    menu_str(0, 12 * CH, " BACK to return     ", COLS, MC_HINT, MC_BG, S);
    while (!back_key_pressed()) sleep_ms(50);
}

// ---------------------------------------------------------------------------
// PSRAM Read Diagnostic — investigate read vs write speed asymmetry
//
// Dumps QMI/XIP registers, then runs isolated read/write benchmarks
// without verify overhead to isolate the true DMA throughput.
// Serial-only output for debugging.
// ---------------------------------------------------------------------------
void psram_rd_diag(void) {
    printf("\n=== PSRAM Read Diagnostic ===\n");

    uint32_t gpio0_ctrl = *(volatile uint32_t *)0x40028004u;
    if ((gpio0_ctrl & 0x1Fu) != 9) {
        printf("  PSRAM not initialized. Run 'psram' first.\n");
        return;
    }

    uint32_t sys_hz = clock_get_hz(clk_sys);
    printf("\n  sys_clk = %u MHz\n", (unsigned)(sys_hz / 1000000u));

    // --- 1. QMI M1 register dump ---
    printf("\n--- QMI M1 Registers ---\n");
    printf("  M1.timing  = 0x%08X\n", (unsigned)qmi_hw->m[1].timing);
    uint32_t t = qmi_hw->m[1].timing;
    printf("    CLKDIV=%u  RXDELAY=%u  COOLDOWN=%u  MIN_DESELECT=%u\n",
           (unsigned)(t & 0xFF),
           (unsigned)((t >> 8) & 0x7),
           (unsigned)((t >> 30) & 0x3),
           (unsigned)((t >> 22) & 0x1F));
    printf("    SELECT_HOLD=%u  SELECT_SETUP=%u  MAX_SELECT=%u  PAGEBREAK=%u\n",
           (unsigned)((t >> 16) & 0x3),
           (unsigned)((t >> 18) & 0x3),
           (unsigned)((t >> 27) & 0x7),
           (unsigned)((t >> 20) & 0x3));

    uint32_t sck_mhz = sys_hz / (2u * (t & 0xFF)) / 1000000u;
    printf("    SCK = %u MHz\n", (unsigned)sck_mhz);

    printf("  M1.rfmt    = 0x%08X\n", (unsigned)qmi_hw->m[1].rfmt);
    uint32_t rf = qmi_hw->m[1].rfmt;
    printf("    PREFIX_LEN=%u  PREFIX_WIDTH=%u  ADDR_WIDTH=%u\n",
           (unsigned)(rf & 0x3),
           (unsigned)((rf >> 2) & 0x3),
           (unsigned)((rf >> 4) & 0x3));
    printf("    DUMMY_LEN=%u  DUMMY_WIDTH=%u  DATA_WIDTH=%u  SUFFIX_LEN=%u\n",
           (unsigned)((rf >> 16) & 0x7),
           (unsigned)((rf >> 10) & 0x3),
           (unsigned)((rf >> 8) & 0x3),
           (unsigned)((rf >> 6) & 0x3));
    printf("  M1.rcmd    = 0x%02X\n", (unsigned)qmi_hw->m[1].rcmd);

    printf("  M1.wfmt    = 0x%08X\n", (unsigned)qmi_hw->m[1].wfmt);
    printf("  M1.wcmd    = 0x%02X\n", (unsigned)qmi_hw->m[1].wcmd);

    // --- 2. QMI M0 register dump (for comparison) ---
    printf("\n--- QMI M0 Registers (Flash, reference) ---\n");
    printf("  M0.timing  = 0x%08X\n", (unsigned)qmi_hw->m[0].timing);
    uint32_t t0r = qmi_hw->m[0].timing;
    printf("    CLKDIV=%u  RXDELAY=%u  COOLDOWN=%u  MIN_DESELECT=%u  PAGEBREAK=%u\n",
           (unsigned)(t0r & 0xFF),
           (unsigned)((t0r >> 8) & 0x7),
           (unsigned)((t0r >> 30) & 0x3),
           (unsigned)((t0r >> 22) & 0x1F),
           (unsigned)((t0r >> 20) & 0x3));
    printf("  M0.rfmt    = 0x%08X\n", (unsigned)qmi_hw->m[0].rfmt);
    printf("  M0.rcmd    = 0x%02X\n", (unsigned)qmi_hw->m[0].rcmd);

    // --- 3. XIP_CTRL ---
    printf("\n--- XIP_CTRL ---\n");
    uint32_t xc = xip_ctrl_hw->ctrl;
    printf("  XIP_CTRL   = 0x%08X\n", (unsigned)xc);
    printf("    EN=%u  WRITABLE_M1=%u  POWER_DOWN=%u\n",
           (unsigned)(xc & 1),
           (unsigned)((xc >> 10) & 1),
           (unsigned)((xc >> 3) & 1));
    printf("    SPLIT_WAYS=%u  MAINT_NONSEC=%u\n",
           (unsigned)((xc >> 4) & 1),
           (unsigned)((xc >> 5) & 1));

    // Cache status
    printf("  XIP_STAT   = 0x%08X\n", (unsigned)xip_ctrl_hw->stat);

    // --- 4. ATRANS (address translation) for M1 ---
    printf("\n--- QMI ATRANS (address translation) ---\n");
    for (int i = 0; i < 8; i++) {
        uint32_t at = qmi_hw->atrans[i];
        printf("  ATRANS[%d]  = 0x%08X  (BASE=0x%06X, SIZE=%u)\n",
               i, (unsigned)at,
               (unsigned)((at >> 2) & 0x3FFFFF) << 12,
               (unsigned)(at & 0x3));
    }

    // --- 5. Pure DMA write (no verify) ---
    printf("\n--- Pure DMA Benchmarks (no verify) ---\n");
    volatile uint32_t *psram = (volatile uint32_t *)PSRAM_NOCACHE;
    static uint32_t pat = 0xA5A5A5A5u;
    #define RD_DIAG_CHUNK_WORDS (64u * 1024u / 4u)  // 64 KB in words
    static uint32_t rd_diag_buf[RD_DIAG_CHUNK_WORDS];

    int dma_ch = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);

    // DMA Write
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);

    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    dma_channel_configure(dma_ch, &cfg,
                          (void *)psram, &pat,
                          PSRAM_FULL_WORDS, true);
    dma_channel_wait_for_finish_blocking(dma_ch);
    uint32_t wr_ms = to_ms_since_boot(get_absolute_time()) - t0;
    uint32_t wr_kbps = wr_ms ? (8u * 1024u * 1000u / wr_ms) : 0;
    printf("  DMA Write 8MB:      %u ms  (%u KB/s)\n",
           (unsigned)wr_ms, (unsigned)wr_kbps);

    // DMA Read (pure — into SRAM, no verify, discard data)
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, true);

    // Read in 64 KB chunks (DMA dst = rd_diag_buf, just overwrite same buffer)
    t0 = to_ms_since_boot(get_absolute_time());
    for (uint32_t off = 0; off < PSRAM_FULL_WORDS; off += RD_DIAG_CHUNK_WORDS) {
        uint32_t count = RD_DIAG_CHUNK_WORDS;
        if (off + count > PSRAM_FULL_WORDS) count = PSRAM_FULL_WORDS - off;
        dma_channel_configure(dma_ch, &cfg,
                              rd_diag_buf, (void *)(psram + off),
                              count, true);
        dma_channel_wait_for_finish_blocking(dma_ch);
    }
    uint32_t rd_ms = to_ms_since_boot(get_absolute_time()) - t0;
    uint32_t rd_kbps = rd_ms ? (8u * 1024u * 1000u / rd_ms) : 0;
    printf("  DMA Read 8MB:       %u ms  (%u KB/s)\n",
           (unsigned)rd_ms, (unsigned)rd_kbps);

    // DMA Read single 64 KB block (no loop overhead)
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, true);
    t0 = to_ms_since_boot(get_absolute_time());
    dma_channel_configure(dma_ch, &cfg,
                          rd_diag_buf, (void *)psram,
                          RD_DIAG_CHUNK_WORDS, true);
    dma_channel_wait_for_finish_blocking(dma_ch);
    uint32_t rd64_ms = to_ms_since_boot(get_absolute_time()) - t0;
    uint32_t rd64_kbps = rd64_ms ? (64u * 1000u / rd64_ms) : 0;
    printf("  DMA Read 64KB:      %u ms  (%u KB/s)\n",
           (unsigned)rd64_ms, (unsigned)rd64_kbps);

    // Per-word timing
    printf("\n--- Per-word Timing ---\n");
    uint32_t wr_ns = wr_ms ? (uint32_t)((uint64_t)wr_ms * 1000000ULL / PSRAM_FULL_WORDS) : 0;
    uint32_t rd_ns = rd_ms ? (uint32_t)((uint64_t)rd_ms * 1000000ULL / PSRAM_FULL_WORDS) : 0;
    printf("  Write: %u ns/word  (%u sys_clk @ %u MHz)\n",
           (unsigned)wr_ns, (unsigned)(wr_ns * (sys_hz / 1000000u) / 1000u),
           (unsigned)(sys_hz / 1000000u));
    printf("  Read:  %u ns/word  (%u sys_clk @ %u MHz)\n",
           (unsigned)rd_ns, (unsigned)(rd_ns * (sys_hz / 1000000u) / 1000u),
           (unsigned)(sys_hz / 1000000u));
    printf("  Ratio: read/write = %u.%ux\n",
           (unsigned)(rd_ms * 10u / wr_ms / 10u),
           (unsigned)(rd_ms * 10u / wr_ms % 10u));

    // Expected QPI clocks (no burst)
    printf("\n--- Expected (no burst, per word) ---\n");
    printf("  Write: cmd(2)+addr(6)+data(8) = 16 QPI clk = %u ns\n",
           (unsigned)(16u * 1000u / sck_mhz));
    printf("  Read:  cmd(2)+addr(6)+dummy(7)+data(8) = 23 QPI clk = %u ns\n",
           (unsigned)(23u * 1000u / sck_mhz));
    printf("  Expected ratio: 1.44x (dummy overhead only)\n");
    printf("  Actual ratio:   %u.%ux\n",
           (unsigned)(rd_ms * 10u / wr_ms / 10u),
           (unsigned)(rd_ms * 10u / wr_ms % 10u));

    if (rd_ms > wr_ms * 2) {
        printf("\n  >>> Write appears to BURST, read does NOT <<<\n");
        printf("  Check M1.timing PAGEBREAK field:\n");
        uint32_t pb = (t >> 20) & 0x3;
        const char *pb_str[] = {"NONE (no break)", "256B", "512B", "1024B"};
        printf("    PAGEBREAK = %u (%s)\n", (unsigned)pb, pb_str[pb]);
        printf("  APS6404L page size = 1024 bytes → PAGEBREAK should be 3\n");
    }

    dma_channel_unclaim(dma_ch);
    printf("\n=== Done ===\n");
}
