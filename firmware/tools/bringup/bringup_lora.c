#include "bringup.h"
#include "bringup_menu.h"

// TFT layout constants (scale=2: 12x16 px/char, 26 cols on 320-wide screen)
#define LS  2
#define LCH (8 * LS)
#define LCOLS (menu_tft_width() / (6 * LS))

// ---------------------------------------------------------------------------
// SX1262 LoRa test (SPI1)
//
// SPI Mode 0 (CPOL=0 CPHA=0), MSB first, CS active-low, max 18 MHz.
// Datasheet: DS.SX1261-2.W.APP (Semtech)
//
// GPIO allocation (from mcu-gpio-allocation.md):
//   GPIO 23 = LORA_nRST   (SIO output, active-low reset)
//   GPIO 24 = LORA_MISO   (SPI1_RX)
//   GPIO 25 = LORA_nCS    (SIO output, manual CS — active-low)
//   GPIO 26 = LORA_SCK    (SPI1_SCK)
//   GPIO 27 = LORA_MOSI   (SPI1_TX)
//   GPIO 28 = LORA_BUSY   (SIO input — HIGH while chip is busy; wait LOW before each cmd)
//   GPIO 29 = LORA_DIO1   (SIO input — IRQ / wakeup; not used here)
//
// GetStatus (0xC0): send [0xC0, NOP]; status byte returned in 2nd MISO byte.
//   Status bits [6:4] = ChipMode:  2=STBY_RC 3=STBY_XOSC 4=FS 5=RX 6=TX
//   Status bits [3:1] = CmdStatus: 2=Data avail 3=Timeout 4=CmdError 5=ExecFail 6=TxDone
//
// ReadRegister (0x1D): send [0x1D, addrMSB, addrLSB, NOP, NOP]; data in 5th MISO byte.
//   RegSyncWord: 0x0740 MSB=0x14, 0x0741 LSB=0x24 (private network defaults)
// ---------------------------------------------------------------------------

static inline void sx1262_cs_assert(void)   { gpio_put(LORA_nCS_PIN, 0); }
static inline void sx1262_cs_deassert(void) { gpio_put(LORA_nCS_PIN, 1); }

// Returns false on timeout (BUSY stuck high).
static bool sx1262_wait_busy(uint32_t timeout_ms) {
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;
    while (gpio_get(LORA_BUSY_PIN)) {
        if (to_ms_since_boot(get_absolute_time()) > deadline) return false;
    }
    return true;
}

// GetStatus → returns status byte (bits[6:4]=ChipMode, bits[3:1]=CmdStatus).
static uint8_t sx1262_get_status(void) {
    uint8_t tx[2] = {SX1262_OP_GET_STATUS, 0x00};
    uint8_t rx[2] = {0, 0};
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 2);
    sx1262_cs_deassert();
    return rx[1];
}

// ReadRegister at 16-bit address → returns data byte.
static uint8_t sx1262_read_reg(uint16_t addr) {
    uint8_t tx[5] = {SX1262_OP_READ_REGISTER,
                     (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
                     0x00, 0x00};
    uint8_t rx[5] = {0};
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 5);
    sx1262_cs_deassert();
    return rx[4];
}

// Send a command with no data bytes beyond the opcode.
static void sx1262_cmd(uint8_t op) {
    sx1262_wait_busy(100);
    sx1262_cs_assert();
    spi_write_blocking(spi1, &op, 1);
    sx1262_cs_deassert();
}

// Send a command followed by a payload buffer.
static void sx1262_cmd_buf(uint8_t op, const uint8_t *payload, size_t len) {
    sx1262_wait_busy(100);
    sx1262_cs_assert();
    spi_write_blocking(spi1, &op, 1);
    spi_write_blocking(spi1, payload, len);
    sx1262_cs_deassert();
}

// GetIrqStatus → returns 16-bit IRQ flags.
// Bits: 0=TxDone 1=RxDone 2=PreambleDet 4=HeaderValid 5=HeaderErr 6=CrcErr 10=Timeout
static uint16_t sx1262_get_irq(void) {
    uint8_t tx[4] = {SX1262_OP_GET_IRQ_STATUS, 0, 0, 0};
    uint8_t rx[4] = {0};
    sx1262_wait_busy(50);
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 4);
    sx1262_cs_deassert();
    return (uint16_t)((rx[2] << 8) | rx[3]);
}

// GetRxBufferStatus → fills *payload_len and *buf_offset.
static void sx1262_get_rx_buf_status(uint8_t *payload_len, uint8_t *buf_offset) {
    uint8_t tx[4] = {SX1262_OP_GET_RX_BUFFER_STATUS, 0, 0, 0};
    uint8_t rx[4] = {0};
    sx1262_wait_busy(50);
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 4);
    sx1262_cs_deassert();
    *payload_len = rx[2];
    *buf_offset  = rx[3];
}

// ReadBuffer: read 'len' bytes starting at 'offset' from the SX1262 data buffer.
static void sx1262_read_buffer(uint8_t offset, uint8_t *out, uint8_t len) {
    uint8_t header[3] = {SX1262_OP_READ_BUFFER, offset, 0x00};
    sx1262_wait_busy(50);
    sx1262_cs_assert();
    spi_write_blocking(spi1, header, 3);
    for (uint8_t i = 0; i < len; i++) {
        uint8_t z = 0;
        spi_write_read_blocking(spi1, &z, &out[i], 1);
    }
    sx1262_cs_deassert();
}

// WriteBuffer: write 'len' bytes starting at 'offset' into the SX1262 data buffer.
static void sx1262_write_buffer(uint8_t offset, const uint8_t *data, uint8_t len) {
    uint8_t header[2] = {0x0Eu, offset};
    sx1262_wait_busy(50);
    sx1262_cs_assert();
    spi_write_blocking(spi1, header, 2);
    spi_write_blocking(spi1, data, len);
    sx1262_cs_deassert();
}

// GetDeviceErrors → 16-bit error flags (bit=1 means calibration failed).
// Bits: 0=RC64k 1=RC13M 2=PLL 3=ADC 4=IMG(image rejection) 5=XOSC_START 8=PA_ramp
static uint16_t sx1262_get_errors(void) {
    uint8_t tx[4] = {0x17, 0x00, 0x00, 0x00};
    uint8_t rx[4] = {0};
    sx1262_wait_busy(50);
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 4);
    sx1262_cs_deassert();
    return (uint16_t)((rx[2] << 8) | rx[3]);
}

// GetRssiInst → instantaneous RSSI (call while in RX mode).
// Returns raw value; dBm = -raw/2.
static int sx1262_get_rssi_inst(void) {
    uint8_t tx[3] = {0x15, 0x00, 0x00};
    uint8_t rx[3] = {0};
    sx1262_wait_busy(50);
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 3);
    sx1262_cs_deassert();
    return -(int)rx[2];  // return raw negated; caller divides by 2
}

// GetStats → numRxOk, numCrcErr, numHdrErr (16-bit each).
// Frame: opcode(1) + status-NOP(1) + RxOk(2) + CrcErr(2) + HdrErr(2) = 8 bytes
static void sx1262_get_stats(uint16_t *ok, uint16_t *crc, uint16_t *hdr) {
    uint8_t tx[8] = {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t rx[8] = {0};
    sx1262_wait_busy(50);
    sx1262_cs_assert();
    spi_write_read_blocking(spi1, tx, rx, 8);
    sx1262_cs_deassert();
    *ok  = (uint16_t)((rx[2] << 8) | rx[3]);
    *crc = (uint16_t)((rx[4] << 8) | rx[5]);
    *hdr = (uint16_t)((rx[6] << 8) | rx[7]);
}

// Initialise SPI1 and all LoRa GPIO, apply hardware reset, wait for BUSY.
// Returns false if BUSY timeout after reset.
static bool sx1262_hw_init(void) {
    spi_init(spi1, 1000 * 1000);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LORA_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LORA_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(LORA_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_init(LORA_nCS_PIN);  gpio_set_dir(LORA_nCS_PIN,  GPIO_OUT); sx1262_cs_deassert();
    gpio_init(LORA_BUSY_PIN); gpio_set_dir(LORA_BUSY_PIN, GPIO_IN);
    gpio_init(LORA_DIO1_PIN); gpio_set_dir(LORA_DIO1_PIN, GPIO_IN);
    gpio_init(LORA_nRST_PIN); gpio_set_dir(LORA_nRST_PIN, GPIO_OUT);
    gpio_put(LORA_nRST_PIN, 0); sleep_ms(10);
    gpio_put(LORA_nRST_PIN, 1); sleep_ms(10);
    return sx1262_wait_busy(200);
}

static void sx1262_hw_deinit(void) {
    spi_deinit(spi1);
    for (int p = LORA_nRST_PIN; p <= LORA_DIO1_PIN; p++)
        gpio_set_function(p, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// lora — basic SPI verification: reset → GetStatus → ReadRegister (SyncWord)
// ---------------------------------------------------------------------------

void lora_test(void) {
    printf("\n--- SX1262 LoRa Test (SPI1, 1 MHz) ---\n");
    char ln[28];
    int cols = LCOLS;

    menu_clear(MC_BG);
    menu_str(0, 0, " LoRa SPI Test      ", cols, MC_TITLE, MC_TITBG, LS);
    menu_str(0, 2*LCH, " Resetting...       ", cols, MC_FG, MC_BG, LS);

    // SPI1: MISO=GPIO24, SCK=GPIO26, MOSI=GPIO27; CS manual on GPIO25.
    spi_init(spi1, 1000 * 1000);
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LORA_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LORA_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(LORA_MOSI_PIN, GPIO_FUNC_SPI);

    gpio_init(LORA_nCS_PIN); gpio_set_dir(LORA_nCS_PIN, GPIO_OUT);
    sx1262_cs_deassert();

    gpio_init(LORA_BUSY_PIN); gpio_set_dir(LORA_BUSY_PIN, GPIO_IN);
    gpio_init(LORA_DIO1_PIN); gpio_set_dir(LORA_DIO1_PIN, GPIO_IN);

    gpio_init(LORA_nRST_PIN); gpio_set_dir(LORA_nRST_PIN, GPIO_OUT);
    gpio_put(LORA_nRST_PIN, 0);
    sleep_ms(10);
    gpio_put(LORA_nRST_PIN, 1);
    sleep_ms(10);

    printf("  BUSY after reset: ");
    bool busy_ok = sx1262_wait_busy(200);
    printf("%s\n", busy_ok ? "LOW (ready)" : "TIMEOUT — wiring issue?");
    snprintf(ln, sizeof(ln), " BUSY: %-18s", busy_ok ? "LOW (ready)" : "TIMEOUT");
    menu_str(0, 2*LCH, ln, cols, busy_ok ? MC_OK : MC_ERR, MC_BG, LS);

    uint8_t status = sx1262_get_status();
    uint8_t chip_mode  = (status >> 4) & 0x7;
    uint8_t cmd_status = (status >> 1) & 0x7;
    static const char *const mode_str[] =
        {"unused","RFU","STBY_RC","STBY_XOSC","FS","RX","TX","RFU"};
    printf("  GetStatus : 0x%02X  ChipMode=%s(%u)  CmdStatus=%u  %s\n",
           status, mode_str[chip_mode], chip_mode, cmd_status,
           chip_mode == 2 ? "(OK — STBY_RC as expected)" : "(unexpected mode)");
    snprintf(ln, sizeof(ln), " Mode: %s (%u)     ", mode_str[chip_mode], chip_mode);
    menu_str(0, 3*LCH, ln, cols, chip_mode == 2 ? MC_OK : MC_ERR, MC_BG, LS);

    if (!sx1262_wait_busy(50)) printf("  BUSY before ReadReg — timeout\n");
    uint8_t sw_msb = sx1262_read_reg(SX1262_REG_SYNC_WORD_MSB);
    if (!sx1262_wait_busy(50)) printf("  BUSY between regs — timeout\n");
    uint8_t sw_lsb = sx1262_read_reg(SX1262_REG_SYNC_WORD_LSB);
    printf("  RegSyncWord (0x0740/41): 0x%02X 0x%02X  %s\n",
           sw_msb, sw_lsb,
           (sw_msb == 0x14 && sw_lsb == 0x24) ? "(default OK)" :
           (sw_msb == 0x34 && sw_lsb == 0x44) ? "(LoRaWAN public)" :
           "(non-default — unexpected at POR)");
    bool sw_ok = (sw_msb == 0x14 && sw_lsb == 0x24);
    snprintf(ln, sizeof(ln), " Sync: %02X %02X %s   ", sw_msb, sw_lsb, sw_ok ? "OK" : "ERR");
    menu_str(0, 4*LCH, ln, cols, sw_ok ? MC_OK : MC_ERR, MC_BG, LS);

    snprintf(ln, sizeof(ln), " Status: 0x%02X Cmd=%u ", status, cmd_status);
    menu_str(0, 5*LCH, ln, cols, MC_FG, MC_BG, LS);

    bool pass = busy_ok && (chip_mode == 2) && sw_ok;
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");
    snprintf(ln, sizeof(ln), " Result: %-16s", pass ? "PASS" : "FAIL");
    menu_str(0, 7*LCH, ln, cols, pass ? MC_OK : MC_ERR, MC_BG, LS);

    spi_deinit(spi1);
    for (int p = LORA_nRST_PIN; p <= LORA_DIO1_PIN; p++)
        gpio_set_function(p, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// lora_rx — sniff LoRa packets for timeout_s seconds
// freq_hz: carrier frequency; sf: 5-12; bw_code: 0x05=250kHz; cr_code: 0x01=4/5
// ---------------------------------------------------------------------------

void lora_rx(uint32_t freq_hz, uint8_t sf, uint8_t bw_code,
             uint8_t cr_code, uint32_t timeout_s) {
    printf("\n--- SX1262 LoRa RX Sniff ---\n");
    printf("  freq=%lu Hz  SF=%u  BW_code=0x%02X  CR_code=0x%02X  timeout=%lus\n",
           (unsigned long)freq_hz, sf, bw_code, cr_code, (unsigned long)timeout_s);
    char ln[28];
    int cols = LCOLS;

    menu_clear(MC_BG);
    snprintf(ln, sizeof(ln), " RX %.3f MHz      ", freq_hz / 1e6);
    menu_str(0, 0, ln, cols, MC_TITLE, MC_TITBG, LS);
    snprintf(ln, sizeof(ln), " SF%u BW%uk Mesh    ", sf,
             bw_code == 0x05 ? 250 : bw_code == 0x04 ? 125 : 500);
    menu_str(0, LCH, ln, cols, MC_FG, MC_BG, LS);
    menu_str(0, 3*LCH, " Initializing...    ", cols, MC_FG, MC_BG, LS);

    if (!sx1262_hw_init()) {
        printf("  BUSY timeout after reset\n");
        menu_str(0, 3*LCH, " BUSY TIMEOUT       ", cols, MC_ERR, MC_BG, LS);
        menu_str(0, 9*LCH, " BACK to return     ", cols, MC_HINT, MC_BG, LS);
        while (!back_key_pressed()) sleep_ms(50);
        sx1262_hw_deinit(); return;
    }

    // --- RF init (RadioLib-exact order) ---

    // 1. Standby XOSC (RadioLib uses XOSC for TCXO boards)
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    // 2. DIO2 as RF switch (PE4259)
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_DIO2_RF_SWITCH, &p, 1); }

    // 3. SetDIO3AsTCXOCtrl: 1.8V, 500µs timeout
    { uint8_t p[4] = {0x02, 0x00, 0x00, 0x20};
      sx1262_cmd_buf(0x97u, p, 4); }
    sx1262_wait_busy(10);

    // 4. Calibrate all blocks
    { uint8_t p = 0x7F; sx1262_cmd_buf(SX1262_OP_CALIBRATE, &p, 1); }
    sx1262_wait_busy(50);

    // 5. Standby XOSC again after calibrate
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    // 6. DC-DC regulator
    { uint8_t p = 0x01; sx1262_cmd_buf(0x96u, &p, 1); }

    // 7. Packet type: LoRa
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_PACKET_TYPE, &p, 1); }

    // 8. SetRfFrequency
    {
        uint32_t freq_reg = (uint32_t)(((uint64_t)freq_hz << 25) / 32000000UL);
        uint8_t p[4] = {(uint8_t)(freq_reg >> 24), (uint8_t)(freq_reg >> 16),
                        (uint8_t)(freq_reg >> 8),  (uint8_t)(freq_reg)};
        sx1262_cmd_buf(SX1262_OP_SET_RF_FREQUENCY, p, 4);
        printf("  freq_reg=0x%08X\n", (unsigned)freq_reg);
    }

    // 9. CalibrateImage for 902-928 MHz
    { uint8_t p[2] = {0xE1, 0xE9}; sx1262_cmd_buf(SX1262_OP_CALIBRATE_IMAGE, p, 2); }
    sx1262_wait_busy(50);

    // SetModulationParams(SF, BW, CR, LDRO)
    // LDRO=1 when symbol time > 16.38 ms: (1<<SF) > 16 * BW_kHz
    uint8_t ldro = 0;
    {
        // SX1262 BW codes: 0=7.8k 1=15.6k 2=31.2k 3=62.5k 4=125k 5=250k 6=500k 8=10.4k 9=20.8k
        static const uint16_t bw_khz[] = {8,16,31,63,125,250,500,0,10,21,42};
        uint16_t bw = (bw_code < 11) ? bw_khz[bw_code] : 250;
        if ((1UL << sf) > 16UL * bw) ldro = 1;
        uint8_t p[4] = {sf, bw_code, cr_code, ldro};
        sx1262_cmd_buf(SX1262_OP_SET_MODULATION_PARAMS, p, 4);
        printf("  LDRO=%u\n", ldro);
    }

    // SetPacketParams: preamble=16 (Meshtastic default), explicit header, max payload=255, CRC on, IQ normal
    { uint8_t p[6] = {0x00, 0x10, 0x00, 0xFF, 0x01, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_PACKET_PARAMS, p, 6); }

    // SetSyncWord to 0x2B (Meshtastic private — RadioLibInterface.h:84).
    // RadioLib encoding (controlBits=0x00):
    //   MSB = (sw & 0xF0) | (ctrl >> 4)     = 0x20
    //   LSB = ((sw & 0x0F) << 4) | (ctrl)   = 0xB0
    // POR default 0x1424 = syncWord 0x12 (generic private) — wrong for Meshtastic.
    { uint8_t p[5] = {0x0Du, 0x07, 0x40, 0x20, 0xB0};
      sx1262_wait_busy(10);
      sx1262_cs_assert();
      spi_write_blocking(spi1, p, 5);
      sx1262_cs_deassert(); }
    printf("  SyncWord set: 0x2B → reg[0x0740/41]=0x20 0xB0 (Meshtastic)\n");

    // SetBufferBaseAddress(TX=0, RX=0)
    { uint8_t p[2] = {0x00, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_BUFFER_BASE_ADDR, p, 2); }

    // SetDioIrqParams — enable IRQ bits in IrqMask (POR default = 0x0000, nothing reported).
    // Without this, GetIrqStatus always returns 0 and RxDone is never seen.
    // IrqMask: bit1=RxDone, bit2=PreambleDet, bit5=HeaderErr, bit6=CrcErr, bit9=Timeout → 0x0266
    // DIO1Mask: same (DIO1 pulses on these events); DIO2/DIO3: 0 (RF switch / unused)
    { uint8_t p[8] = {0x02, 0x66,   // IrqMask
                      0x02, 0x66,   // DIO1Mask
                      0x00, 0x00,   // DIO2Mask
                      0x00, 0x00};  // DIO3Mask
      sx1262_cmd_buf(0x08u, p, 8); }

    // ClearIrqStatus(all)
    { uint8_t p[2] = {0xFF, 0xFF};
      sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }

    // Fix sensitivity (SX1262 errata 15.1): reg 0x0889 bit2=1 for BW<500k
    if (bw_code != 0x06) {
        uint8_t sens = sx1262_read_reg(0x0889);
        sens |= 0x04;
        uint8_t p[4] = {0x0Du, 0x08, 0x89, sens};
        sx1262_wait_busy(10);
        sx1262_cs_assert();
        spi_write_blocking(spi1, p, 4);
        sx1262_cs_deassert();
    }

    // SetRx(timeout) — timeout in units of 15.625 µs; 0xFFFFFF = continuous
    uint32_t rx_timeout = (timeout_s == 0) ? 0xFFFFFF :
                          (uint32_t)((uint64_t)timeout_s * 1000000 / 16);
    {
        uint8_t p[3] = {(uint8_t)(rx_timeout >> 16),
                        (uint8_t)(rx_timeout >> 8),
                        (uint8_t)(rx_timeout)};
        sx1262_cmd_buf(SX1262_OP_SET_RX, p, 3);
    }

    sx1262_wait_busy(500);
    sleep_ms(10);

    uint8_t st = sx1262_get_status();
    printf("  GetStatus after SetRx: 0x%02X  ChipMode=%u (%s)\n",
           st, (st >> 4) & 7, ((st >> 4) & 7) == 5 ? "RX OK" : "unexpected");

    bool continuous = (timeout_s == 0);
    if (continuous) {
        printf("  Listening continuously. Type 'exit' + Enter to stop.\n");
    } else {
        printf("  Listening for %lu s...\n", (unsigned long)timeout_s);
    }

    // TFT: show listening status
    menu_str(0, 3*LCH, " RSSI:              ", cols, MC_FG, MC_BG, LS);
    menu_str(0, 4*LCH, " Packets: 0         ", cols, MC_FG, MC_BG, LS);
    menu_str(0, 5*LCH, " CRC err: 0         ", cols, MC_FG, MC_BG, LS);
    menu_str(0, 7*LCH, " Listening...       ", cols, MC_OK, MC_BG, LS);
    menu_str(0, 9*LCH, " BACK to stop       ", cols, MC_HINT, MC_BG, LS);

    // Drain any residual serial bytes left from command dispatch
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}

    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_s * 1000;
    uint32_t rssi_deadline = to_ms_since_boot(get_absolute_time()) + 1000;
    int packets = 0;
    int crc_errs = 0;
    char ibuf[8] = {0};
    int ilen = 0;

    for (;;) {
        // Exit condition: timed mode deadline, or SX1262 HW timeout IRQ
        if (!continuous) {
            if (to_ms_since_boot(get_absolute_time()) >= deadline) break;
        }
        if (back_key_pressed()) break;

        // Check for "exit\r" or "exit\n" on serial (continuous mode only)
        if (continuous) {
            int ch = getchar_timeout_us(0);
            if (ch != PICO_ERROR_TIMEOUT) {
                if (ch == '\r' || ch == '\n') {
                    ibuf[ilen] = '\0';
                    if (strcmp(ibuf, "exit") == 0) break;
                    ilen = 0;
                } else if (ilen < 7) {
                    ibuf[ilen++] = (char)ch;
                }
            }
        }

        // Periodic RSSI heartbeat (every 1 s)
        if (to_ms_since_boot(get_absolute_time()) >= rssi_deadline) {
            int rssi_raw = sx1262_get_rssi_inst();
            printf("  [RSSI] %d dBm  pkts=%d\n", rssi_raw / 2, packets);
            snprintf(ln, sizeof(ln), " RSSI: %d dBm       ", rssi_raw / 2);
            menu_str(0, 3*LCH, ln, cols, MC_FG, MC_BG, LS);
            rssi_deadline = to_ms_since_boot(get_absolute_time()) + 1000;
        }

        uint16_t irq = sx1262_get_irq();
        if (irq & (1u << 1)) {  // RxDone
            uint8_t plen, poff;
            sx1262_get_rx_buf_status(&plen, &poff);
            packets++;
            printf("  [PKT %d] irq=0x%04X  len=%u bytes  buf_offset=%u\n",
                   packets, irq, plen, poff);
            snprintf(ln, sizeof(ln), " Packets: %-10d", packets);
            menu_str(0, 4*LCH, ln, cols, MC_OK, MC_BG, LS);
            if (plen > 0 && plen <= 255) {
                uint8_t buf[256];
                sx1262_read_buffer(poff, buf, plen);
                printf("    hex: ");
                for (uint8_t i = 0; i < plen && i < 32; i++) printf("%02X ", buf[i]);
                if (plen > 32) printf("...");
                printf("\n    ascii: ");
                for (uint8_t i = 0; i < plen && i < 32; i++) {
                    char c = (char)buf[i];
                    printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
                }
                printf("\n");
                if (irq & (1u << 6)) {
                    printf("    [CRC ERROR]\n");
                    crc_errs++;
                    snprintf(ln, sizeof(ln), " CRC err: %-10d", crc_errs);
                    menu_str(0, 5*LCH, ln, cols, MC_ERR, MC_BG, LS);
                }
                // Show first 8 bytes of last packet on TFT
                snprintf(ln, sizeof(ln), " %02X%02X%02X%02X %02X%02X%02X%02X",
                         buf[0], plen>1?buf[1]:0, plen>2?buf[2]:0, plen>3?buf[3]:0,
                         plen>4?buf[4]:0, plen>5?buf[5]:0, plen>6?buf[6]:0, plen>7?buf[7]:0);
                menu_str(0, 6*LCH, ln, cols, MC_FG, MC_BG, LS);
                snprintf(ln, sizeof(ln), " len=%u irq=%04X    ", plen, irq);
                menu_str(0, 7*LCH, ln, cols, MC_OK, MC_BG, LS);
            }
            { uint8_t p[2] = {0xFF, 0xFF};
              sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
            { uint8_t p[3] = {(uint8_t)(rx_timeout >> 16),
                               (uint8_t)(rx_timeout >> 8),
                               (uint8_t)(rx_timeout)};
              sx1262_cmd_buf(SX1262_OP_SET_RX, p, 3); }
        } else if (irq & (1u << 5)) {  // HeaderErr — preamble found but header undecodable
            printf("  [HDR_ERR] irq=0x%04X  (preamble detected; wrong SF or SyncWord?)\n", irq);
            menu_str(0, 7*LCH, " HDR_ERR (preamble) ", cols, MC_ERR, MC_BG, LS);
            { uint8_t p[2] = {0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
        } else if (irq & (1u << 2)) {  // PreambleDetected — RF activity on this freq/SF
            printf("  [PREAMBLE] irq=0x%04X  (LoRa preamble on air — SF/BW match)\n", irq);
            menu_str(0, 7*LCH, " PREAMBLE detected  ", cols, MC_FG, MC_BG, LS);
            { uint8_t p[2] = {0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
        } else if (!continuous && (irq & (1u << 10))) {  // HW Timeout (timed mode only)
            break;
        }
        sleep_ms(10);
    }
    printf("  Done. Packets received: %d\n", packets);
    snprintf(ln, sizeof(ln), " Done. pkts=%d crc=%d", packets, crc_errs);
    menu_str(0, 7*LCH, ln, cols, packets > 0 ? MC_OK : MC_FG, MC_BG, LS);

    sx1262_hw_deinit();
}

// ---------------------------------------------------------------------------
// lora_dump — full register/status snapshot
// ---------------------------------------------------------------------------

void lora_dump(void) {
    printf("\n--- SX1262 Register & Status Dump ---\n");
    char ln[28];
    int cols = LCOLS;

    menu_clear(MC_BG);
    menu_str(0, 0, " LoRa Register Dump ", cols, MC_TITLE, MC_TITBG, LS);
    menu_str(0, 2*LCH, " Reading...         ", cols, MC_FG, MC_BG, LS);

    if (!sx1262_hw_init()) {
        printf("  BUSY timeout after reset\n");
        menu_str(0, 2*LCH, " BUSY TIMEOUT       ", cols, MC_ERR, MC_BG, LS);
        menu_str(0, 9*LCH, " BACK to return     ", cols, MC_HINT, MC_BG, LS);
        while (!back_key_pressed()) sleep_ms(50);
        sx1262_hw_deinit(); return;
    }

    // --- Static state (no RF config needed) ---
    printf("\n  [Static — STBY_RC]\n");
    uint8_t st = sx1262_get_status();
    printf("    GetStatus      : 0x%02X  ChipMode=%u  CmdStatus=%u\n",
           st, (st >> 4) & 7, (st >> 1) & 7);
    snprintf(ln, sizeof(ln), " Stat: 0x%02X mode=%u ", st, (st>>4)&7);
    menu_str(0, 2*LCH, ln, cols, ((st>>4)&7)==2 ? MC_OK : MC_ERR, MC_BG, LS);

    uint8_t sync_msb = sx1262_read_reg(0x0740);
    uint8_t sync_lsb = sx1262_read_reg(0x0741);
    printf("    RegSyncWord    : 0x%02X 0x%02X  %s\n", sync_msb, sync_lsb,
           (sync_msb==0x14 && sync_lsb==0x24) ? "(private network default)" :
           (sync_msb==0x34 && sync_lsb==0x44) ? "(LoRaWAN public)" : "(custom)");
    snprintf(ln, sizeof(ln), " Sync: %02X %02X %s    ",
             sync_msb, sync_lsb,
             (sync_msb==0x14 && sync_lsb==0x24) ? "OK" : "??");
    menu_str(0, 3*LCH, ln, cols, MC_FG, MC_BG, LS);

    // OCP: register 0x08E7; Iocp = value × 2.5 mA; default 0x38 = 140 mA
    uint8_t ocp = sx1262_read_reg(0x08E7);
    printf("    OCP (0x08E7)   : 0x%02X  Ilim=%u mA (%s)\n", ocp,
           (unsigned)(ocp * 5u / 2u),
           ocp == 0x38 ? "140mA default" : "non-default");
    snprintf(ln, sizeof(ln), " OCP: 0x%02X %umA %s  ",
             ocp, (unsigned)(ocp * 5u / 2u), ocp==0x38 ? "OK" : "!!");
    menu_str(0, 4*LCH, ln, cols, ocp==0x38 ? MC_OK : MC_ERR, MC_BG, LS);

    uint8_t xta = sx1262_read_reg(0x0911);
    printf("    XTATrim (0x0911): 0x%02X\n", xta);

    // --- Enter RX to read RF-dependent status ---
    printf("\n  [RF state — entering RX for RSSI + stats]\n");

    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_PACKET_TYPE, &p, 1); }
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_DIO2_RF_SWITCH, &p, 1); }
    // SetDIO3AsTCXOCtrl: 1.8V, 500µs timeout
    { uint8_t p[4] = {0x02, 0x00, 0x00, 0x20}; sx1262_cmd_buf(0x97u, p, 4); }
    // SetRegulatorMode: DC-DC
    { uint8_t p = 0x01; sx1262_cmd_buf(0x96u, &p, 1); }
    {
        uint32_t fr = (uint32_t)(((uint64_t)923125000UL << 25) / 32000000UL);
        uint8_t p[4] = {(uint8_t)(fr>>24),(uint8_t)(fr>>16),(uint8_t)(fr>>8),(uint8_t)fr};
        sx1262_cmd_buf(SX1262_OP_SET_RF_FREQUENCY, p, 4);
    }
    { uint8_t p = 0x7F; sx1262_cmd_buf(SX1262_OP_CALIBRATE, &p, 1); }
    sx1262_wait_busy(50);
    { uint8_t p[2] = {0xE1, 0xE9}; sx1262_cmd_buf(SX1262_OP_CALIBRATE_IMAGE, p, 2); }
    sx1262_wait_busy(50);

    // GetDeviceErrors AFTER calibration (pre-calibration state is all-fail by POR default).
    // Note: XOSC_START error (bit 5) may appear with always-on TCXO — chip is functional
    // if RX entry succeeds (ChipMode=5) and RSSI is valid.
    uint16_t errs = sx1262_get_errors();
    printf("    GetDeviceErrors: 0x%04X  RC64k=%d RC13M=%d PLL=%d ADC=%d IMG=%d XOSC=%d PA=%d\n",
           errs,
           (errs>>0)&1, (errs>>1)&1, (errs>>2)&1, (errs>>3)&1,
           (errs>>4)&1, (errs>>5)&1, (errs>>8)&1);
    // Mask XOSC_START (bit5) — cosmetic with always-on TCXO
    uint16_t real_errs = errs & ~(1u << 5);
    snprintf(ln, sizeof(ln), " Err: 0x%04X %s     ", errs, real_errs ? "FAIL" : "OK");
    menu_str(0, 5*LCH, ln, cols, real_errs ? MC_ERR : MC_OK, MC_BG, LS);

    // SetModulationParams (SF11, BW250k(0x05), CR4/5, LDRO=0)
    { uint8_t p[4] = {11, 0x05, 0x01, 0x00}; sx1262_cmd_buf(SX1262_OP_SET_MODULATION_PARAMS, p, 4); }
    // SetPacketParams
    { uint8_t p[6] = {0x00, 0x08, 0x00, 0xFF, 0x01, 0x00}; sx1262_cmd_buf(SX1262_OP_SET_PACKET_PARAMS, p, 6); }
    // SetBufferBaseAddress
    { uint8_t p[2] = {0x00, 0x00}; sx1262_cmd_buf(SX1262_OP_SET_BUFFER_BASE_ADDR, p, 2); }

    // RxGain register 0x08AC: 0x94=power-save (default), 0x96=boosted LNA
    { uint8_t wtx[4] = {0x0Du, 0x08, 0xAC, 0x96};
      sx1262_wait_busy(50);
      sx1262_cs_assert();
      spi_write_blocking(spi1, wtx, 4);
      sx1262_cs_deassert(); }
    uint8_t rxgain = sx1262_read_reg(0x08AC);
    printf("    RxGain (0x08AC): 0x%02X  %s\n", rxgain,
           rxgain == 0x96 ? "(boosted LNA — write OK)" :
           rxgain == 0x94 ? "(power saving — write failed?)" : "(unexpected)");
    snprintf(ln, sizeof(ln), " RxGain: 0x%02X %s   ", rxgain,
             rxgain == 0x96 ? "boost" : rxgain == 0x94 ? "pwr" : "???");
    menu_str(0, 6*LCH, ln, cols, rxgain == 0x96 ? MC_OK : MC_ERR, MC_BG, LS);

    // Enter continuous RX (timeout=0xFFFFFF)
    { uint8_t p[2] = {0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
    { uint8_t p[3] = {0xFF, 0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_SET_RX, p, 3); }
    sx1262_wait_busy(500);
    sleep_ms(10);

    st = sx1262_get_status();
    printf("    GetStatus in RX: 0x%02X  ChipMode=%u (%s)\n",
           st, (st >> 4) & 7, ((st >> 4) & 7) == 5 ? "RX OK" : "unexpected");

    int last_rssi = 0;
    if (((st >> 4) & 7) == 5) {
        printf("    RSSI samples (5 × 100 ms):");
        for (int i = 0; i < 5; i++) {
            sleep_ms(100);
            int raw = sx1262_get_rssi_inst();
            printf("  %.1f", raw / 2.0f);
            last_rssi = raw;
        }
        printf(" dBm\n");
    }
    snprintf(ln, sizeof(ln), " RSSI: %d dBm       ", last_rssi / 2);
    menu_str(0, 7*LCH, ln, cols, MC_FG, MC_BG, LS);

    // GetStats: opcode + NOP(status) + RxOk(2) + CrcErr(2) + HdrErr(2) = 8 bytes
    {
        uint8_t tx[8] = {0x10,0,0,0,0,0,0,0};
        uint8_t rx[8] = {0};
        sx1262_wait_busy(50);
        sx1262_cs_assert();
        spi_write_read_blocking(spi1, tx, rx, 8);
        sx1262_cs_deassert();
        uint16_t ok  = (uint16_t)((rx[2]<<8)|rx[3]);
        uint16_t crc = (uint16_t)((rx[4]<<8)|rx[5]);
        uint16_t hdr = (uint16_t)((rx[6]<<8)|rx[7]);
        printf("    GetStats       : RxOk=%u  CrcErr=%u  HdrErr=%u\n", ok, crc, hdr);
        snprintf(ln, sizeof(ln), " Stat: ok%u crc%u hdr%u", ok, crc, hdr);
        menu_str(0, 8*LCH, ln, cols, MC_FG, MC_BG, LS);
    }

    // Return to standby
    { uint8_t p = 0x00; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    sx1262_hw_deinit();
}

// ---------------------------------------------------------------------------
// AES-128 (minimal implementation for Meshtastic CTR-mode encryption)
// ---------------------------------------------------------------------------

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t aes_rcon[10] = {
    0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

static void aes128_expand_key(const uint8_t key[16], uint8_t rk[176]) {
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, &rk[(i - 1) * 4], 4);
        if (i % 4 == 0) {
            uint8_t tmp = t[0];
            t[0] = aes_sbox[t[1]] ^ aes_rcon[i / 4 - 1];
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        for (int j = 0; j < 4; j++)
            rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
    }
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void aes128_encrypt_block(const uint8_t rk[176],
                                 const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];

    for (int r = 1; r <= 10; r++) {
        // SubBytes
        for (int i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];
        // ShiftRows
        uint8_t t;
        t=s[1];  s[1]=s[5];   s[5]=s[9];   s[9]=s[13];  s[13]=t;
        t=s[2];  s[2]=s[10];  s[10]=t;  t=s[6];  s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
        // MixColumns (skip on last round)
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                int j = c * 4;
                uint8_t a0=s[j],a1=s[j+1],a2=s[j+2],a3=s[j+3];
                s[j]   = xtime(a0) ^ xtime(a1)^a1 ^ a2 ^ a3;
                s[j+1] = a0 ^ xtime(a1) ^ xtime(a2)^a2 ^ a3;
                s[j+2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3)^a3;
                s[j+3] = xtime(a0)^a0 ^ a1 ^ a2 ^ xtime(a3);
            }
        }
        // AddRoundKey
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}

// AES-128-CTR: encrypt/decrypt 'len' bytes in-place.
// Nonce layout (Meshtastic): [packetId:8][fromNode:4][counter:4]
// Counter (bytes 12-15) incremented big-endian after each 16-byte block.
static void aes128_ctr(const uint8_t key[16], const uint8_t nonce[16],
                       const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t rk[176], ctr[16], ks[16];
    aes128_expand_key(key, rk);
    memcpy(ctr, nonce, 16);

    for (size_t off = 0; off < len; off += 16) {
        aes128_encrypt_block(rk, ctr, ks);
        size_t n = (len - off < 16) ? (len - off) : 16;
        for (size_t i = 0; i < n; i++)
            out[off + i] = in[off + i] ^ ks[i];
        // Increment counter (big-endian, bytes 12-15)
        for (int i = 15; i >= 12; i--)
            if (++ctr[i]) break;
    }
}

// Meshtastic default channel PSK (pskIndex=1, AES-128)
static const uint8_t mesh_default_psk[16] = {
    0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,
    0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01
};

// Channel hash for default channel: XOR("") ^ XOR(psk) = 0x02
#define MESH_DEFAULT_CH_HASH  0x6B

// Hardcoded sender node number (arbitrary, identifiable)
#define MESH_BRINGUP_NODE_ID  0x4D4F4B59u  /* "MOKY" */

// ---------------------------------------------------------------------------
// lora_tx — transmit one Meshtastic-format text message (TW LONG_FAST)
// ---------------------------------------------------------------------------

void lora_tx(void) {
    printf("\n--- SX1262 LoRa TX (Meshtastic MEDIUM_FAST, TW 920.125 MHz) ---\n");
    char ln[28];
    int cols = LCOLS;

    menu_clear(MC_BG);
    menu_str(0, 0, " LoRa TX Meshtastic ", cols, MC_TITLE, MC_TITBG, LS);
    menu_str(0, 2*LCH, " 920.125 SF11 17dBm ", cols, MC_FG, MC_BG, LS);
    menu_str(0, 3*LCH, " Initializing...    ", cols, MC_FG, MC_BG, LS);

    if (!sx1262_hw_init()) {
        printf("  BUSY timeout after reset\n");
        menu_str(0, 3*LCH, " BUSY TIMEOUT       ", cols, MC_ERR, MC_BG, LS);
        menu_str(0, 9*LCH, " BACK to return     ", cols, MC_HINT, MC_BG, LS);
        while (!back_key_pressed()) sleep_ms(50);
        sx1262_hw_deinit(); return;
    }

    // --- RF init (RadioLib-exact order: begin() then startTransmit()) ---

    // 1. Standby XOSC (RadioLib uses XOSC for TCXO boards, not RC)
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    // 2. DIO2 as RF switch
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_DIO2_RF_SWITCH, &p, 1); }

    // 3. TCXO 1.8V, 500µs timeout
    { uint8_t p[4] = {0x02, 0x00, 0x00, 0x20};
      sx1262_cmd_buf(0x97u, p, 4); }
    sx1262_wait_busy(10);

    // 4. Calibrate all blocks
    { uint8_t p = 0x7F; sx1262_cmd_buf(SX1262_OP_CALIBRATE, &p, 1); }
    sx1262_wait_busy(50);

    // 5. Standby XOSC again after calibrate (RadioLib does this)
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    // 6. DC-DC regulator
    { uint8_t p = 0x01; sx1262_cmd_buf(0x96u, &p, 1); }

    // 7. Packet type: LoRa
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_PACKET_TYPE, &p, 1); }

    // 8. Frequency: 920.125 MHz
    uint32_t freq_hz = 920125000UL;
    {
        uint32_t fr = (uint32_t)(((uint64_t)freq_hz << 25) / 32000000UL);
        uint8_t p[4] = {(uint8_t)(fr>>24),(uint8_t)(fr>>16),
                        (uint8_t)(fr>>8),(uint8_t)fr};
        sx1262_cmd_buf(SX1262_OP_SET_RF_FREQUENCY, p, 4);
    }

    // 9. CalibrateImage for 902-928 MHz band
    { uint8_t p[2] = {0xE1, 0xE9}; sx1262_cmd_buf(SX1262_OP_CALIBRATE_IMAGE, p, 2); }
    sx1262_wait_busy(50);

    // 10. Modulation: SF9, BW250k(0x05), CR4/5(0x01), LDRO=0 — MEDIUM_FAST
    { uint8_t p[4] = {9, 0x05, 0x01, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_MODULATION_PARAMS, p, 4); }

    // 11. PacketParams (initial, will update with actual payload len before TX)
    { uint8_t p[6] = {0x00, 0x10, 0x00, 0xFF, 0x01, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_PACKET_PARAMS, p, 6); }

    // 12. SyncWord 0x2B (Meshtastic): reg 0x0740=0x20, 0x0741=0xB0
    { uint8_t p[5] = {0x0Du, 0x07, 0x40, 0x20, 0xB0};
      sx1262_wait_busy(10);
      sx1262_cs_assert();
      spi_write_blocking(spi1, p, 5);
      sx1262_cs_deassert(); }

    // 13. SetBufferBaseAddress(TX=0, RX=0)
    { uint8_t p[2] = {0x00, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_BUFFER_BASE_ADDR, p, 2); }

    // 14. fixPaClamping (SX1262 errata 15.2 — PA clamping too aggressive)
    //     Read reg 0x08D8, OR 0x1E, write back. Without this TX may never complete.
    { uint8_t clamp = sx1262_read_reg(0x08D8);
      clamp |= 0x1E;
      uint8_t p[4] = {0x0Du, 0x08, 0xD8, clamp};
      sx1262_wait_busy(10);
      sx1262_cs_assert();
      spi_write_blocking(spi1, p, 4);
      sx1262_cs_deassert(); }

    // 15. SetPaConfig: paDutyCycle=4, hpMax=7, deviceSel=0(SX1262), paLut=1
    { uint8_t p[4] = {0x04, 0x07, 0x00, 0x01};
      sx1262_cmd_buf(0x95u, p, 4); }

    // 15. SetTxParams: power=17 dBm, rampTime=200µs (0x04)
    { uint8_t p[2] = {17, 0x04};
      sx1262_cmd_buf(0x8Eu, p, 2); }

    // 16. OCP 140 mA (0x38) — must write AFTER SetPaConfig+SetTxParams
    { uint8_t p[4] = {0x0Du, 0x08, 0xE7, 0x38};
      sx1262_wait_busy(10);
      sx1262_cs_assert();
      spi_write_blocking(spi1, p, 4);
      sx1262_cs_deassert(); }

    // Clear device errors from calibration
    { uint8_t p[2] = {0x00, 0x00};
      sx1262_cmd_buf(0x07u, p, 2); }

    printf("  OCP: 0x%02X (%u mA)\n",
           sx1262_read_reg(0x08E7), sx1262_read_reg(0x08E7) * 5 / 2);

    // --- Build Meshtastic packet ---

    // Unique packet ID from microsecond timer (avoids dedup rejection)
    uint32_t pkt_id = time_us_32();
    uint32_t from_node = MESH_BRINGUP_NODE_ID;
    uint32_t to_node = 0xFFFFFFFFu;  // broadcast

    // Protobuf-encode Data{portnum=TEXT_MESSAGE_APP(1), payload="MokyaLora"}
    // Field 1 (portnum): tag=0x08 varint=0x01
    // Field 2 (payload): tag=0x12 len=0x09 data="MokyaLora"
    static const uint8_t pb_data[] = {
        0x08, 0x01, 0x12, 0x09,
        'M','o','k','y','a','L','o','r','a'
    };
    const size_t pb_len = sizeof(pb_data);  // 13 bytes

    // AES-128-CTR nonce: [packetId:8 LE][fromNode:4 LE][counter:4 = 0]
    uint8_t nonce[16];
    memset(nonce, 0, sizeof(nonce));
    memcpy(&nonce[0], &pkt_id, 4);    // bytes 0-3: packetId low32
    // bytes 4-7: packetId high32 = 0
    memcpy(&nonce[8], &from_node, 4); // bytes 8-11: fromNode
    // bytes 12-15: CTR block counter = 0

    uint8_t enc_payload[sizeof(pb_data)];
    aes128_ctr(mesh_default_psk, nonce, pb_data, enc_payload, pb_len);

    // PacketHeader (16 bytes, little-endian on RP2350)
    typedef struct __attribute__((packed)) {
        uint32_t to;
        uint32_t from;
        uint32_t id;
        uint8_t  flags;
        uint8_t  channel;
        uint8_t  next_hop;
        uint8_t  relay_node;
    } mesh_hdr_t;

    mesh_hdr_t hdr;
    hdr.to         = to_node;
    hdr.from       = from_node;
    hdr.id         = pkt_id;
    hdr.flags      = 3 | (3 << 5);  // hop_limit=3, hop_start=3
    hdr.channel    = MESH_DEFAULT_CH_HASH;
    hdr.next_hop   = 0;
    hdr.relay_node = 0;

    // Total air packet: 16 header + pb_len encrypted
    uint8_t air_pkt[16 + sizeof(pb_data)];
    memcpy(air_pkt, &hdr, 16);
    memcpy(air_pkt + 16, enc_payload, pb_len);
    uint8_t total_len = (uint8_t)(16 + pb_len);

    printf("  to=%08lX  from=%08lX  id=%08lX\n",
           (unsigned long)to_node, (unsigned long)from_node, (unsigned long)pkt_id);
    printf("  flags=0x%02X  channel=0x%02X  payload=\"MokyaLora\" (%u bytes protobuf)\n",
           hdr.flags, hdr.channel, (unsigned)pb_len);
    printf("  Air packet (%u bytes): ", total_len);
    for (int i = 0; i < total_len; i++) printf("%02X ", air_pkt[i]);
    printf("\n");

    menu_str(0, 3*LCH, " to: FFFFFFFF bcast ", cols, MC_FG, MC_BG, LS);
    snprintf(ln, sizeof(ln), " from: %08lX      ", (unsigned long)from_node);
    menu_str(0, 4*LCH, ln, cols, MC_FG, MC_BG, LS);
    snprintf(ln, sizeof(ln), " id: %08lX        ", (unsigned long)pkt_id);
    menu_str(0, 5*LCH, ln, cols, MC_FG, MC_BG, LS);
    menu_str(0, 6*LCH, " msg: \"MokyaLora\"   ", cols, MC_FG, MC_BG, LS);

    // --- startTransmit sequence (RadioLib-exact order) ---

    // Standby before TX config
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    // IRQ: TxDone(bit0) + Timeout(bit9)
    { uint8_t p[8] = {0x02, 0x01,   // IrqMask: TxDone + Timeout
                      0x02, 0x01,   // DIO1Mask
                      0x00, 0x00,   // DIO2Mask
                      0x00, 0x00};  // DIO3Mask
      sx1262_cmd_buf(0x08u, p, 8); }

    // SetPacketParams with actual payload length
    { uint8_t p[6] = {0x00, 0x10, 0x00, total_len, 0x01, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_PACKET_PARAMS, p, 6); }

    // SetBufferBaseAddress(TX=0, RX=0)
    { uint8_t p[2] = {0x00, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_BUFFER_BASE_ADDR, p, 2); }

    // WriteBuffer
    sx1262_write_buffer(0x00, air_pkt, total_len);

    // ClearIrqStatus(all)
    { uint8_t p[2] = {0xFF, 0xFF};
      sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }

    // Fix sensitivity (SX1262 errata 15.1): reg 0x0889 bit2=1 for BW<500k
    { uint8_t sens = sx1262_read_reg(0x0889);
      sens |= 0x04;
      uint8_t p[4] = {0x0Du, 0x08, 0x89, sens};
      sx1262_wait_busy(10);
      sx1262_cs_assert();
      spi_write_blocking(spi1, p, 4);
      sx1262_cs_deassert(); }

    // SetTx: timeout=0 (no timeout, like RadioLib — single packet mode)
    printf("  Transmitting...\n");
    menu_str(0, 7*LCH, " Transmitting...    ", cols, MC_FG, MC_BG, LS);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    { uint8_t p[3] = {0x00, 0x00, 0x00};
      sx1262_cmd_buf(0x83u, p, 3); }

    // Wait for TxDone (poll up to 10 s)
    bool tx_done = false;
    uint32_t dt = 0;
    for (int i = 0; i < 1000; i++) {
        sleep_ms(10);
        uint16_t irq = sx1262_get_irq();
        if (irq & (1u << 0)) {  // TxDone
            dt = to_ms_since_boot(get_absolute_time()) - t0;
            printf("  TxDone! (%lu ms)\n", (unsigned long)dt);
            tx_done = true;
            break;
        }
        if (irq & (1u << 9)) {  // Timeout
            printf("  TX Timeout!\n");
            break;
        }
    }

    if (!tx_done) {
        printf("  Result: FAIL\n");
        menu_str(0, 7*LCH, " TX FAIL / Timeout  ", cols, MC_ERR, MC_BG, LS);
    } else {
        printf("  Result: PASS — packet sent on 920.125 MHz, SF9/BW250k\n");
        snprintf(ln, sizeof(ln), " TxDone! %lu ms     ", (unsigned long)dt);
        menu_str(0, 7*LCH, ln, cols, MC_OK, MC_BG, LS);
    }
    snprintf(ln, sizeof(ln), " Result: %-11s", tx_done ? "PASS" : "FAIL");
    menu_str(0, 8*LCH, ln, cols, tx_done ? MC_OK : MC_ERR, MC_BG, LS);

    // Return to standby
    { uint8_t p = 0x00; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    sx1262_hw_deinit();
}
