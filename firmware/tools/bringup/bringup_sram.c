#include "bringup.h"
#include "bringup_menu.h"
#include "hardware/clocks.h"

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

