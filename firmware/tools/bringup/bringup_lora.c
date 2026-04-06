#include "bringup.h"

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

    uint8_t status = sx1262_get_status();
    uint8_t chip_mode  = (status >> 4) & 0x7;
    uint8_t cmd_status = (status >> 1) & 0x7;
    static const char *const mode_str[] =
        {"unused","RFU","STBY_RC","STBY_XOSC","FS","RX","TX","RFU"};
    printf("  GetStatus : 0x%02X  ChipMode=%s(%u)  CmdStatus=%u  %s\n",
           status, mode_str[chip_mode], chip_mode, cmd_status,
           chip_mode == 2 ? "(OK — STBY_RC as expected)" : "(unexpected mode)");

    if (!sx1262_wait_busy(50)) printf("  BUSY before ReadReg — timeout\n");
    uint8_t sw_msb = sx1262_read_reg(SX1262_REG_SYNC_WORD_MSB);
    if (!sx1262_wait_busy(50)) printf("  BUSY between regs — timeout\n");
    uint8_t sw_lsb = sx1262_read_reg(SX1262_REG_SYNC_WORD_LSB);
    printf("  RegSyncWord (0x0740/41): 0x%02X 0x%02X  %s\n",
           sw_msb, sw_lsb,
           (sw_msb == 0x14 && sw_lsb == 0x24) ? "(default OK)" :
           (sw_msb == 0x34 && sw_lsb == 0x44) ? "(LoRaWAN public)" :
           "(non-default — unexpected at POR)");

    bool pass = busy_ok && (chip_mode == 2) && (sw_msb == 0x14);
    printf("  Result: %s\n", pass ? "PASS" : "FAIL");

    spi_deinit(spi1);
    for (int p = LORA_nRST_PIN; p <= LORA_DIO1_PIN; p++)
        gpio_set_function(p, GPIO_FUNC_NULL);
}

// ---------------------------------------------------------------------------
// lora_rx — sniff LoRa packets for timeout_s seconds
// freq_hz: carrier frequency; sf: 5-12; bw_code: 0x08=250kHz; cr_code: 0x01=4/5
// ---------------------------------------------------------------------------

void lora_rx(uint32_t freq_hz, uint8_t sf, uint8_t bw_code,
             uint8_t cr_code, uint32_t timeout_s) {
    printf("\n--- SX1262 LoRa RX Sniff ---\n");
    printf("  freq=%lu Hz  SF=%u  BW_code=0x%02X  CR_code=0x%02X  timeout=%lus\n",
           (unsigned long)freq_hz, sf, bw_code, cr_code, (unsigned long)timeout_s);

    if (!sx1262_hw_init()) {
        printf("  BUSY timeout after reset\n");
        sx1262_hw_deinit(); return;
    }

    { uint8_t p = 0x00; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_PACKET_TYPE, &p, 1); }

    // SetDIO2AsRfSwitchCtrl: DIO2 drives PE4259 RF switch automatically
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_DIO2_RF_SWITCH, &p, 1); }

    // SetDIO3AsTCXOCtrl: required even for always-on TCXO (ECS-TXO-20CSMV4 on 1.8V rail).
    // Without this call the chip assumes a crystal and PLL calibration fails.
    // Voltage 0x02=1.8V, timeout 0x000020=500µs.
    { uint8_t p[4] = {0x02, 0x00, 0x00, 0x20};
      sx1262_cmd_buf(0x97u, p, 4); }
    sx1262_wait_busy(10);

    // SetRfFrequency: freq_reg = freq_hz * 2^25 / 32e6
    {
        uint32_t freq_reg = (uint32_t)(((uint64_t)freq_hz << 25) / 32000000UL);
        uint8_t p[4] = {(uint8_t)(freq_reg >> 24), (uint8_t)(freq_reg >> 16),
                        (uint8_t)(freq_reg >> 8),  (uint8_t)(freq_reg)};
        sx1262_cmd_buf(SX1262_OP_SET_RF_FREQUENCY, p, 4);
        printf("  freq_reg=0x%08X\n", (unsigned)freq_reg);
    }

    // Calibrate(0x7F): all internal blocks (RC64k/RC13M/PLL/ADC/Image).
    { uint8_t p = 0x7F; sx1262_cmd_buf(SX1262_OP_CALIBRATE, &p, 1); }
    sx1262_wait_busy(50);

    // CalibrateImage: band pair for 902-928 MHz = {0xE1, 0xE9}
    { uint8_t p[2] = {0xE1, 0xE9}; sx1262_cmd_buf(SX1262_OP_CALIBRATE_IMAGE, p, 2); }
    sx1262_wait_busy(50);

    // SetModulationParams(SF, BW, CR, LDRO)
    // LDRO=1 when symbol time > 16.38 ms: (1<<SF) > 16 * BW_kHz
    uint8_t ldro = 0;
    {
        static const uint16_t bw_khz[] = {8,10,16,21,31,42,63,125,250,500};
        uint16_t bw = (bw_code < 10) ? bw_khz[bw_code] : 250;
        if ((1UL << sf) > 16UL * bw) ldro = 1;
        uint8_t p[4] = {sf, bw_code, cr_code, ldro};
        sx1262_cmd_buf(SX1262_OP_SET_MODULATION_PARAMS, p, 4);
        printf("  LDRO=%u\n", ldro);
    }

    // SetPacketParams: preamble=16 (Meshtastic default), explicit header, max payload=255, CRC on, IQ normal
    { uint8_t p[6] = {0x00, 0x10, 0x00, 0xFF, 0x01, 0x00};
      sx1262_cmd_buf(SX1262_OP_SET_PACKET_PARAMS, p, 6); }

    // SetSyncWord to 0x2B (Meshtastic private — RadioLibInterface.h:84).
    // SX1262 2-byte encoding: reg[0x0740] = (sw & 0xF0)|0x04 = 0x24
    //                         reg[0x0741] = ((sw<<4)&0xF0)|0x04 = 0xB4
    // Default POR value 0x1424 = syncWord 0x12 (generic private) — WRONG for Meshtastic.
    { uint8_t p[5] = {0x0Du, 0x07, 0x40, 0x24, 0xB4};
      sx1262_wait_busy(10);
      sx1262_cs_assert();
      spi_write_blocking(spi1, p, 5);
      sx1262_cs_deassert(); }
    printf("  SyncWord set: 0x2B → reg[0x0740/41]=0x24 0xB4 (Meshtastic)\n");

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

    // Drain any residual serial bytes left from command dispatch
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}

    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_s * 1000;
    uint32_t rssi_deadline = to_ms_since_boot(get_absolute_time()) + 1000;
    int packets = 0;
    char ibuf[8] = {0};
    int ilen = 0;

    for (;;) {
        // Exit condition: timed mode deadline, or SX1262 HW timeout IRQ
        if (!continuous) {
            if (to_ms_since_boot(get_absolute_time()) >= deadline) break;
        }

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

        // Periodic RSSI heartbeat (continuous mode, every 5 s)
        if (continuous && to_ms_since_boot(get_absolute_time()) >= rssi_deadline) {
            uint8_t tx[2] = {SX1262_OP_GET_RSSI_INST, 0x00};
            uint8_t rx[2] = {0};
            sx1262_wait_busy(10);
            sx1262_cs_assert();
            spi_write_read_blocking(spi1, tx, rx, 2);
            sx1262_cs_deassert();
            printf("  [RSSI] %d dBm  pkts=%d\n", -(int)rx[1] / 2, packets);
            rssi_deadline = to_ms_since_boot(get_absolute_time()) + 1000;
        }

        uint16_t irq = sx1262_get_irq();
        if (irq & (1u << 1)) {  // RxDone
            uint8_t plen, poff;
            sx1262_get_rx_buf_status(&plen, &poff);
            packets++;
            printf("  [PKT %d] irq=0x%04X  len=%u bytes  buf_offset=%u\n",
                   packets, irq, plen, poff);
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
                if (irq & (1u << 6)) printf("    [CRC ERROR]\n");
            }
            { uint8_t p[2] = {0xFF, 0xFF};
              sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
            { uint8_t p[3] = {(uint8_t)(rx_timeout >> 16),
                               (uint8_t)(rx_timeout >> 8),
                               (uint8_t)(rx_timeout)};
              sx1262_cmd_buf(SX1262_OP_SET_RX, p, 3); }
        } else if (irq & (1u << 5)) {  // HeaderErr — preamble found but header undecodable
            printf("  [HDR_ERR] irq=0x%04X  (preamble detected; wrong SF or SyncWord?)\n", irq);
            { uint8_t p[2] = {0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
        } else if (irq & (1u << 2)) {  // PreambleDetected — RF activity on this freq/SF
            printf("  [PREAMBLE] irq=0x%04X  (LoRa preamble on air — SF/BW match)\n", irq);
            { uint8_t p[2] = {0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
        } else if (!continuous && (irq & (1u << 10))) {  // HW Timeout (timed mode only)
            break;
        }
        sleep_ms(10);
    }
    printf("  Done. Packets received: %d\n", packets);

    sx1262_hw_deinit();
}

// ---------------------------------------------------------------------------
// lora_dump — full register/status snapshot
// ---------------------------------------------------------------------------

void lora_dump(void) {
    printf("\n--- SX1262 Register & Status Dump ---\n");

    if (!sx1262_hw_init()) {
        printf("  BUSY timeout after reset\n");
        sx1262_hw_deinit(); return;
    }

    // --- Static state (no RF config needed) ---
    printf("\n  [Static — STBY_RC]\n");
    uint8_t st = sx1262_get_status();
    printf("    GetStatus      : 0x%02X  ChipMode=%u  CmdStatus=%u\n",
           st, (st >> 4) & 7, (st >> 1) & 7);

    uint8_t sync_msb = sx1262_read_reg(0x0740);
    uint8_t sync_lsb = sx1262_read_reg(0x0741);
    printf("    RegSyncWord    : 0x%02X 0x%02X  %s\n", sync_msb, sync_lsb,
           (sync_msb==0x14 && sync_lsb==0x24) ? "(private network default)" :
           (sync_msb==0x34 && sync_lsb==0x44) ? "(LoRaWAN public)" : "(custom)");

    // OCP: register 0x08E7; Iocp = 45 + 2.5×code mA; default 0x38 = 185 mA
    uint8_t ocp = sx1262_read_reg(0x08E7);
    printf("    OCP (0x08E7)   : 0x%02X  Ilim=%u mA (%s)\n", ocp,
           (unsigned)((90u + 5u * ocp) / 2u),
           ocp == 0x38 ? "185mA — SX1262 TX default" : "non-default");

    uint8_t xta = sx1262_read_reg(0x0911);
    printf("    XTATrim (0x0911): 0x%02X\n", xta);

    // --- Enter RX to read RF-dependent status ---
    printf("\n  [RF state — entering RX for RSSI + stats]\n");

    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_PACKET_TYPE, &p, 1); }
    { uint8_t p = 0x01; sx1262_cmd_buf(SX1262_OP_SET_DIO2_RF_SWITCH, &p, 1); }
    // SetDIO3AsTCXOCtrl: 1.8V, 500µs timeout
    { uint8_t p[4] = {0x02, 0x00, 0x00, 0x20}; sx1262_cmd_buf(0x97u, p, 4); }
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

    // SetModulationParams (SF11, BW250k, CR4/5, LDRO=0)
    { uint8_t p[4] = {11, 0x08, 0x01, 0x00}; sx1262_cmd_buf(SX1262_OP_SET_MODULATION_PARAMS, p, 4); }
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

    // Enter continuous RX (timeout=0xFFFFFF)
    { uint8_t p[2] = {0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_CLEAR_IRQ_STATUS, p, 2); }
    { uint8_t p[3] = {0xFF, 0xFF, 0xFF}; sx1262_cmd_buf(SX1262_OP_SET_RX, p, 3); }
    sx1262_wait_busy(500);
    sleep_ms(10);

    st = sx1262_get_status();
    printf("    GetStatus in RX: 0x%02X  ChipMode=%u (%s)\n",
           st, (st >> 4) & 7, ((st >> 4) & 7) == 5 ? "RX OK" : "unexpected");

    if (((st >> 4) & 7) == 5) {
        printf("    RSSI samples (5 × 100 ms):");
        for (int i = 0; i < 5; i++) {
            sleep_ms(100);
            int raw = sx1262_get_rssi_inst();
            printf("  %.1f", raw / 2.0f);
        }
        printf(" dBm\n");
    }

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
    }

    // Return to standby
    { uint8_t p = 0x00; sx1262_cmd_buf(SX1262_OP_SET_STANDBY, &p, 1); }

    sx1262_hw_deinit();
}
