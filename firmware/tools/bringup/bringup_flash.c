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
