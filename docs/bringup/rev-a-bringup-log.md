# Rev A Bring-Up Log

**Board revision:** Rev A
**Submitted to manufacturer:** 2026-03-22
**Board received:** 2026-04-02
**Bring-up started:** 2026-04-02

For build/flash scripts, shell commands, and source layout see
[bringup-tooling.md](bringup-tooling.md).

---

## Pre-Power Checklist

- [x] Visual inspection: no solder bridges, correct component orientation.
- [x] Continuity check: VSYS, 1.8 V, 3.3 V rails to GND (expect open before power).
- [x] Resistance to GND: VSYS rail > 10 Ω, no short.
- [x] USB-C CC1 / CC2 pull-down resistors (5.1 kΩ) present — USB-C correctly negotiates 5 V.

---

## Bring-Up Sequence

### Step 1 — Power Rails

| Rail   | Expected  | Measured | Pass/Fail |
|--------|-----------|----------|-----------|
| VSYS   | 3.5–4.2 V | USB-C 5 V input OK | ✅ PASS |
| 1.8 V  | 1.80 V    | 1.8 V    | ✅ PASS |
| 3.3 V  | 3.30 V    | 3.3 V    | ✅ PASS |

**Quiescent current:** ~30 mA (measured with firmware loaded; original target < 20 mA was specified for no-firmware condition).

### Step 2 — MCU Boot

**Result: ✅ PASS**

- SWD connect and flash: OK via **J-Link** (Pi Debug Probe incompatible — requires 3.3 V VTREF; MCU logic is 1.8 V — see Issue 7).
- USB CDC / virtual COM port: OK.
- RP2350B USB Hello World example: runs correctly.

### Step 3 — I2C Bus Scan & Sensor Verification

#### Bus Scan

| Address | Device         | Bus  | Result | Notes |
|---------|----------------|------|--------|-------|
| 0x6B    | BQ25622        | I2C1 | ✅ PASS | After TP101 fix (Issue 2) |
| 0x55    | BQ27441        | I2C1 | ⚠️ CONDITIONAL | Cold boot NACK — see Issue 9 (BIE) + Issue 10 (latchup). Workaround: boot without battery → charge_on → insert battery |
| 0x6A    | LSM6DSV16X     | I2C0 | ✅ PASS | |
| 0x1E    | LIS2MDL        | I2C0 | ✅ PASS | After R54/R55 jumper fix (Issue 3) |
| 0x5D    | LPS22HH        | I2C0 | ✅ PASS | Address is **0x5D** (SA0 = 3.3 V); design docs incorrectly stated 0x5C — see Issue 4 |
| 0x3A    | Teseo-LIV3FL   | I2C0 | ✅ PASS | |
| 0x36    | LM27965        | I2C1 | ✅ PASS | |

#### Sensor Live Readings

**LSM6DSV16X IMU (0x6A)** — ✅ PASS

Init sequence per datasheet: SW reset → CTRL3=0x44 (BDU+IF_INC) → CTRL8=0x00 (±2g) → CTRL6=0x01 (±250dps) → CTRL1=0x06 (120 Hz HP) → CTRL2=0x06 (120 Hz HP). STATUS=0x07 (XLDA+GDA+TDA all set).

| Channel | Value | Note |
|---------|-------|------|
| Accel X/Y/Z | −0.038 / +0.020 / +0.911 g | Z≈1g confirms gravity on horizontal board |
| Gyro X/Y/Z  | +6.95 / +2.41 / −13.73 dps | Static offset; within normal range |
| Temperature | +28.1 °C | Consistent with ambient |

> **Note:** CTRL1/CTRL2 ODR field is `[3:0]` (not `[7:4]` as in older LSM6D devices). Writing `0x40` incorrectly selects ODR-triggered + power-down mode. Correct value for 120 Hz HP mode is `0x06`.

**LPS22HH Barometer (0x5D)** — ✅ PASS

Init sequence: SW reset → CTRL_REG2=0x10 (IF_ADD_INC) → CTRL_REG1=0x02 (BDU, ODR=power-down) → ONE_SHOT trigger → poll STATUS 0x27 until P_DA+T_DA set.

| Channel | Value |
|---------|-------|
| Pressure | 1004.12 hPa |
| Temperature | 28.12 °C |

> Conversion: `hPa = raw_24bit / 4096.0`; `°C = raw_int16 / 100.0`

**LIS2MDL Magnetometer (0x1E)** — ✅ PASS

Init sequence: SW reset → CFG_REG_A=0x80 (comp_temp_en, 10 Hz, continuous) → CFG_REG_B=0x02 (OFF_CANC every ODR) → CFG_REG_C=0x10 (BDU) → poll STATUS 0x67 bit3 (ZYXDA).

| Channel | Value |
|---------|-------|
| Mag X/Y/Z | +3.0 / −295.5 / +222.0 mGauss |

> Sensitivity fixed at 1.5 mGauss/LSB. Values consistent with Earth's magnetic field at this location.

**Teseo-LIV3FL GNSS (0x3A)** — ⚠️ CONDITIONAL (I2C streaming confirmed; outdoor RF test pending)

I2C read returns live NMEA sentences. `$PSTMGETSWVER` proprietary command ACKed. Indoor test only — no fix expected. 300-byte stream sample:

| NMEA sentence | Decode |
|---------------|--------|
| `$GNGSA,A,1,…,PDOP=99.0` | Fix=1 (no fix); all satellite slots empty |
| `$GPGLL,…,V` | Position void (no fix) |
| `$PSTMCPU,15.21,-1,98` | ST proprietary: chip temp 15.21 °C, freq ~98 MHz |
| `$GPRMC,033936,V` | Status void; UTC 03:39:36 |
| `$GPGGA,…,0 sats,Fix=0` | 0 satellites tracked |

Teseo-LIV3FL confirmed operational (I2C, NMEA streaming, proprietary commands). **RF chain performance (outdoor fix acquisition, TTFF, sensitivity) not yet tested** — requires outdoor test with clear sky view. See Step 14.

### Step 4 — Display

**Result: ✅ PASS** (via FPC adapter — Rev A connector pinout mismatch bypassed; see Issue 5)

| Item | Result | Notes |
|------|--------|-------|
| ST7789VI hardware reset | ✅ PASS | nRST GPIO21 toggled correctly |
| ST7789VI init sequence | ✅ PASS | SWRESET → SLPOUT → COLMOD(RGB565) → MADCTL → gamma → CASET/RASET → INVON → DISPON |
| LM27965 backlight (Bank A 40%) | ✅ PASS | brightness code 0x16, GP=0x21 |
| PIO 8080 write (pio1, clkdiv=4) | ✅ PASS | 128 ns write cycle — within ST7789VI 1.8 V VDDI spec |
| Solid colour fills (R/G/B/W/K) | ✅ PASS | Visual confirm |
| SMPTE colour bars (8 × 30 px) | ✅ PASS | Visual confirm |
| Hue gradient + scroll (90 frames) | ✅ PASS | Visible but response sluggish — see Step 13 |
| Checkerboard (32×32 px) | ✅ PASS | Visual confirm |
| Bouncing block (80 frames) | ✅ PASS | Motion smooth at 40 ms/frame |
| Backlight fade (LM27965 Bank A) | ✅ PASS | Smooth fade in/out across all 32 codes |

**Firmware notes:**
- pio1 used (not pio0) to avoid `gpio_base` conflict: pio0 has `gpio_base=16` (set by NAU8315 amp driver for GPIO 30–32); TFT data bus is GPIO 13–20, requiring `gpio_base ≤ 13` → only pio1 (default base=0) is compatible.
- side-set = nWR (GPIO 12); out = D[7:0] (GPIO 13–20); DCX (GPIO 11) and nCS (GPIO 10) CPU-controlled (SIO), as they change only at command/data phase boundaries.
- `tft_flush()` waits TX FIFO empty + 1 µs before toggling DCX, preventing data/command interleave glitches.
- Display inversion (`INVON`, 0x21) required for correct colour on this IPS panel — without it colours are inverted.
- Issue 5 (FPC connector pinout mismatch) bypassed using a FPC adapter board for Rev A validation. Rev B must correct the connector pinout.

### Step 5 — LoRa

**Result: ✅ PASS** (SPI, calibration, RX, TX, RF link all verified — see Step 23 for full TX/RX validation)

| Item | Result | Notes |
|------|--------|-------|
| SX1262 SPI response | ✅ PASS | GetStatus: ChipMode=2 (STBY_RC), CmdStatus=5 (POR normal) |
| RegSyncWord (0x0740/41) | ✅ PASS | Meshtastic 0x2B → reg 0x20 0xB0 (RadioLib encoding with controlBits=0x00) |
| Calibrate + CalibrateImage | ✅ PASS | GetDeviceErrors = 0x0020 (XOSC flag only — cosmetic false positive, see note) |
| RxGain (0x08AC) | ✅ PASS | Write 0x96, readback 0x96 (boosted LNA confirmed) |
| SetRx → ChipMode=5 | ✅ PASS | RX mode entered successfully |
| RSSI (923.125 MHz, 5 samples) | ✅ PASS | −114.0 dBm consistent (indoor noise floor) |
| GetStats | ✅ PASS | RxOk=0 CrcErr=0 HdrErr=0 (no packets; no crash) |
| OCP (0x08E7) | ✅ PASS | 0x38 = 140 mA (correct formula: value × 2.5 mA). Written after SetPaConfig+SetTxParams |
| TX link test | ✅ PASS | Meshtastic-format TX: TxDone 132 ms (SF9/BW250k); node `!4d4f4b59` visible on mesh — see Step 23 |
| RX link test | ✅ PASS | 25 packets received from node `538EEBE7` (SF9/BW250k, 920.125 MHz, continuous mode, 0 CRC errors) — see Step 23 |
| RF performance (sensitivity, range) | ⏳ PENDING | Requires outdoor test — see Step 15 |

**Firmware notes:**
- GPIO: nRST=23, MISO=24, nCS=25, SCK=26, MOSI=27, BUSY=28, DIO1=29 (matches schematic)
- SPI1, Mode 0, MSB first, 1 MHz; CS controlled manually (not via SPI1_CSn hardware)
- `SetDIO3AsTCXOCtrl` (0x97, voltage=0x02 1.8V, timeout=500 µs) must be called even though TCXO is always-on. Without it PLL calibration fails.
- `GetDeviceErrors = 0x0020` (XOSC_START bit): cosmetic false positive — SX1262 startup-detection flags XOSC even when always-on TCXO is running. Chip is fully functional.

### Step 6 — Keypad

**Result: ✅ PASS**

| Item | Result | Notes |
|------|--------|-------|
| Matrix scan direction | ✅ PASS | ROW = output (drive LOW), COL = input pull-up; diode Anode=COL / Cathode=ROW enforces this direction |
| All 36 keys detected | ✅ PASS | SW1–SW36 confirmed via key_monitor |
| 1 kΩ series resistors (R90–R101) | ✅ PASS | No impact at 1.8 V: Vf ~0.3 V < VIL 0.54 V with pull-up idle at 1.8 V |

**Firmware (r, c) matrix — confirmed by physical testing:**

| r \ c | C0 | C1 | C2 | C3 | C4 | C5 |
|-------|----|----|----|----|----|----|
| R0 | FUNC | BACK | LEFT | DEL | VOL- | UP |
| R1 | 1/2 | 3/4 | 5/6 | 7/8 | 9/0 | OK |
| R2 | Q/W | E/R | T/Y | U/I | O/P | DOWN |
| R3 | A/S | D/F | G/H | J/K | L | RIGHT |
| R4 | Z/X | C/V | B/N | M | ㄡㄥ | SET |
| R5 | MODE | TAB | SPACE | SYM | 。.？ | VOL+ |

> Note: this (r, c) order reflects the physical GPIO-to-switch wiring order in the schematic, not the logical Row/Col in hardware-requirements.md.

**Firmware notes:**
- Bringup scan: GPIO polling, ROW-major (ROW driven LOW one at a time, COL read). Not the production PIO+DMA scan.
- GPIO 36–47 are above the 32-bit GPIO bank boundary; verified functional on RP2350B.
- Initial firmware had scanning direction inverted (COL drive, ROW read = reverse bias = no detection). Corrected to ROW drive / COL read.

### Step 7 — Audio (NAU8315 Amplifier)

**Result: ✅ PASS**

| Item | Result | Notes |
|------|--------|-------|
| NAU8315 I2S format | ✅ PASS | FSL pin floating → Standard I2S mode confirmed |
| NAU8315 gain | ✅ PASS | GAIN pin floating → Mode 5 = 6 dB (valid, defined state per datasheet) |
| PIO I2S driver | ✅ PASS | 48 828 Hz sample rate (clkdiv=40.0); 16-bit stereo |
| Speaker output | ✅ PASS | Little Bee melody plays correctly at 40% amplitude |

**Firmware notes:**
- GPIO 32 (AMP_DAC_PIN) is above the 32-GPIO boundary. `pio_set_gpio_base(pio0, 16)` **must** be called before `pio_add_program_at_offset()`; calling it after causes `PICO_ERROR_INVALID_STATE` and the state machine silently fails to start.
- Click/pop suppression: transitions between notes push zero samples to FIFO rather than using `sleep_ms`, which would drain the FIFO abruptly.

### Step 8 — Battery & Motor

| Item | Result | Notes |
|------|--------|-------|
| Vibration motor (HD-EMB1104-SM-2) | ✅ PASS | PWM drive via SSM3K56ACT OK |
| BQ25622 charger I2C + register dump | ✅ PASS | After TP101 fix (Issue 2); see register dump below |
| BQ25622 VREG/IINDPM write + charging enable | ✅ PASS | VREG=4100 mV, IINDPM=100 mA, EN_CHG=1 confirmed; see charging dump below |
| BQ25622 ADC (IBUS/IBAT/VBUS/VPMID/VBAT/VSYS) | ✅ PASS | ADC_EN=1, 12-bit continuous; all 6 channels read correctly |
| BQ27441 fuel gauge I2C presence | ❌ FAIL | NACK even with charging current flowing — Issue 9; SLEEP mode wakeup sequence required |
| BQ27441 SOC / capacity readout | ⏳ PENDING | Blocked by Issue 9 |

**BQ25622 register dump (at power-on, no battery, charging disabled by firmware):**

| Register | Value | Field decode |
|----------|-------|--------------|
| PART_INFO (0x38) | 0x0A | PN=1 (BQ25622 ✅), DEV_REV=2 |
| CTRL1 (0x16) | 0x80 | EN_CHG=0 (charging disabled at startup) |
| STATUS0 (0x1D) | 0x10 | VSYS_STAT=1 — no battery; system powered from VBUS only |
| STATUS1 (0x1E) | 0x04 | CHG=Not Charging, VBUS=Unknown Adapter |
| FAULT (0x1F) | 0x00 | No faults ✅ |

> **Register map correction (2026-04-04):** Earlier dump incorrectly listed VREG at 0x06 and IINDPM at 0x00. Correct addresses: VREG = REG0x04 (bytes 0x04/0x05, 10 mV/step, POR=4200 mV); IINDPM = REG0x06 (bytes 0x06/0x07, 20 mA/step, POR=3200 mA). Register 0x00 does not exist. STATUS1 VBUS field for BQ25622 only defines 000/100/111; "Adj. HV DCP" in original dump was the BQ25620 decode — corrected to "Unknown Adapter".

**BQ25622 charging operation dump (VREG=4100 mV, IINDPM=100 mA, battery installed):**

| Register | Value | Field decode |
|----------|-------|--------------|
| VREG (0x04/05) | lo=0xD0 hi=0x0C | 4100 mV ✅ |
| IINDPM (0x06/07) | lo=0x50 hi=0x00 | 100 mA ✅ |
| CTRL1 (0x16) | 0xA1 | EN_CHG=1, EN_HIZ=0, WATCHDOG=01 |
| STATUS0 (0x1D) | 0x08 | IINDPM_STAT=1 (100 mA limit active), all others 0 |
| STATUS1 (0x1E) | 0x0C | CHG=CC (Trickle/Pre/Fast), VBUS=Unknown Adapter |
| FAULT (0x1F) | 0x00 | No faults ✅ |

**BQ25622 ADC readings (12-bit continuous, battery installed, charging active):**

| Channel | Value | Notes |
|---------|-------|-------|
| VBUS | 5014 mV | USB-C 5 V input ✅ |
| VPMID | 5002 mV | Mid-point between VBUS and charge path ✅ |
| VBAT | 3966 mV | Battery voltage, BL-4C normal range ✅ |
| VSYS | 3981 mV | System output voltage ✅ |
| IBUS | +84 mA | Input current, within IINDPM=100 mA limit ✅ |
| IBAT | +52 mA | Battery charge current (system load ≈ 32 mA) ✅ |

**Firmware notes:**
- Correct register addresses confirmed from SLUSEG2D: VREG=REG0x04 (10 mV/step), IINDPM=REG0x06 (20 mA/step), ADC_CTRL=REG0x26, ADC_FUNC_DIS=REG0x27.
- `bq25622_enable_charge()` sets VREG and IINDPM before EN_CHG=1; both values confirmed via readback.
- ADC requires explicit enable: write 0x80 to REG0x26 (ADC_EN=1, continuous, 12-bit). POR=disabled. Wait ≥ 300 ms for full channel-scan cycle after enabling.
- REG0x27 (ADC_FUNC_DIS) POR=0x00; all channels enabled by default — no write needed.
- `charge_scan` command: enables charging + 500 ms delay + Bus B I2C scan (to probe BQ27441 wakeup).
- `adc` command: enables ADC (12-bit continuous) + 300 ms wait + reads all 6 channels.

> ICHG POR = 1040 mA (REG0x02, 80 mA/step). Production driver must also configure ICHG, VINDPM (REG0x08), and enable charging via CTRL1[EN_CHG].

**LM27965 register dump (at power-on, all LEDs off):**

| Register | Value | Field decode |
|----------|-------|--------------|
| GP (0x10) | 0x20 | All banks disabled ✅ (bit5 always 1 per datasheet) |
| BANK_A (0xA0) | 0xE0 | brightness code = 0 (TFT backlight, off) |
| BANK_B (0xB0) | 0xE0 | brightness code = 0 (keyboard BL + D3B green, off) |
| BANK_C (0xC0) | 0xFC | brightness code = 0 (D1C red indicator, off) |

### Step 9 — Memory (Flash & PSRAM)

| Item | Result | Notes |
|------|--------|-------|
| W25Q128JW Flash JEDEC ID | ✅ PASS | `EF 60 18` — Winbond W25Q128JW confirmed |
| Flash SR1/SR2/SR3 | ✅ PASS | SR2 QE bit set (QSPI enabled); boot config intact |
| Flash XIP read | ✅ PASS | First 32 bytes match programmed firmware image |
| Internal SRAM (16 KB pattern test) | ✅ PASS | 5 patterns including address-based — all PASS; static used 313 KB / 520 KB (60%), free 206 KB |
| APS6404L PSRAM Read ID | ✅ PASS | `0D 5D 4B 12 A4 31 D5 9F` — APS6404L confirmed (MF=0x0D AP Memory, KGD=0x5D) |
| APS6404L PSRAM SPI write+readback | ✅ PASS | Wrote `DE AD BE EF` @ 0x000000, read back `DE AD BE EF` — MATCH (`psram_probe` direct mode) |
| APS6404L PSRAM XIP write+readback | ✅ PASS | XIP pair: `0xA55A1234` + `0x12345678` — MATCH; 4 KB pattern test (1024 words) — PASS |
| APS6404L PSRAM full capacity (8 MB) | ✅ PASS | Two-pass write+verify (address pattern `0xA5000000\|i`): 2,097,152 words checked — **0 errors** |
| J-Link SWD read of PSRAM | ✅ PASS | `mem32 0x15000000 4` → `DEADBEEF A5A5A5A5 CAFEBABE 12345678` — matches firmware-written sentinel; QMI M1 XIP responds to AHB-AP while CPU halted |

**PSRAM diagnostics (Issue 8 resolved — 3 bugs found and fixed):**

| Bug | Root cause | Fix |
|-----|-----------|-----|
| SIO register addresses | Used RP2040 offsets; RP2350 has interleaved `GPIO_HI_*` regs | `OUT_SET=0x18, OUT_CLR=0x20, OE_SET=0x38` |
| QMI CS1N ≠ GPIO0 | `ASSERT_CS1N` only drives internal QSPI CS1 pad, not GPIO0 | SIO drives physical CE# + `ASSERT_CS1N` for clock generation |
| PSRAM XIP address | Used `+0x800000` (still M0/Flash); XIP bit 24 selects M0/M1 | Changed to `+0x1000000` (bit 24=1 → M1/CS1) |

**Boot-time PSRAM init** (`psram_init()` in `bringup_sram.c`, called at top of `bringup_repl_init()`):

| Step | Action |
|------|--------|
| 1 | GPIO0 = SIO output HIGH (FUNCSEL=5) + internal pull-up (PUE=1, PDE=0) |
| 2 | Enter QMI direct mode, set `ASSERT_CS1N` (for clock generation) |
| 3 | SIO CS toggle: Reset Enable (0x66), Reset (0x99), wait 100 µs, Read ID (0x9F) |
| 4 | Enter QPI (0x35); configure M1: timing (base from M0, CLKDIV=2, RXDELAY=0, COOLDOWN=2), rfmt/rcmd=0xEB (QPI Fast Read, 7 dummy clocks), wfmt/wcmd=0x38 (QPI Write) |
| 5 | Switch GPIO0 to XIP_CS1 (FUNCSEL=9) while still in direct mode (M1 idle) |
| 6 | Clear `ASSERT_CS1N`, exit direct mode → M1 XIP engine takes over GPIO0 |
| 7 | Enable `XIP_CTRL_WRITABLE_M1` for write access via uncached alias (0x15000000) |

**Additional firmware notes (2026-04-05):**
- `psram_full` bringup command: two-pass (write all 8 MB → verify all 8 MB) test using address-based pattern. Progress printed every 1 MB. Confirms full chip capacity and data retention across the entire 8 MB range.
- `psram_jlink` bringup command: writes four sentinel words (`DEADBEEF A5A5A5A5 CAFEBABE 12345678`) to PSRAM XIP uncached base, reads back via firmware, prints J-Link `mem32` command for out-of-band verification.
- RP2350 `XIP_NOCACHE_NOALLOC_BASE` = `0x14000000` (not `0x13000000` as on RP2040); PSRAM uncached XIP address = `0x15000000`.
- J-Link SWD PSRAM access: QMI M1 XIP engine continues to service AHB-AP bus requests while the CPU is halted via SWD. `mem32 0x15000000 N` works reliably for runtime PSRAM inspection and debug.

**XIP speed optimization (2026-04-05):**

PSRAM init upgraded from SPI mode to QPI mode (Enter QPI 0x35, read cmd 0xEB, write cmd 0x38).
DUMMY_LEN empirically determined as 28 bits (7 QPI clocks) — APS6404L needs one extra clock beyond the 6 specified in datasheet (mode byte slot consumed in QPI).

Systematic CLKDIV × RXDELAY sweep (`psram_sweep`, 256 KB per combination, RXDELAY 0–7):

| CLKDIV | RXDELAY | SCK (MHz) | Throughput | Result | Notes |
|--------|---------|-----------|------------|--------|-------|
| 1 | 0 | 75 | 300 Mbit/s | ❌ FAIL | ~150–260 errors / 256 KB (0.3 %), random bit errors |
| 1 | 1 | 75 | 300 Mbit/s | ❌ FAIL | ~190–240 errors / 256 KB (0.3 %) |
| 1 | 2–7 | 75 | — | ❌ FAIL | 100 % errors — QMI sampling past valid window |
| **2** | **0** | **37.5** | **150 Mbit/s** | **✅ PASS** | **0 errors / 256 KB; 8 MB full validation: 0 errors** |
| 2 | 1–7 | 37.5 | — | ❌ FAIL | 100 % errors |
| 3 | 0–7 | 25 | — | ❌ FAIL | 100 % errors — RXDELAY=0 already past valid window |

Flash (`W25Q128JW`) speed sweep (`flash_sweep`, single-word vector table read):

| CLKDIV | RXDELAY | SCK (MHz) | Throughput | Result | Notes |
|--------|---------|-----------|------------|--------|-------|
| 1 | 0–3 | 75 | 300 Mbit/s | ❌ FAIL | Reads 0x00000000 |
| 2 | 0 | 37.5 | 150 Mbit/s | ❌ FAIL | Byte-shifted: `0x82000200` |
| **2** | **1–3** | **37.5** | **150 Mbit/s** | **✅ PASS** | Expected `0x20082000` confirmed |
| 3–4 | 0–3 | 25/18.75 | 100/75 Mbit/s | ✅ PASS | Conservative, all pass |

**CLKDIV=1 (75 MHz) error analysis** (`psram_diag`, read-only at 75 MHz after safe 37.5 MHz write):
- 10-run reproducibility: 132–260 errors per 256 KB (0.2–0.4 %), non-deterministic across boot cycles.
- COOLDOWN (0–3), MIN_DESELECT (1–31), PAGEBREAK (none/256/1024/4096) tuning: no improvement — errors are not boundary-related.
- Write+read both at 75 MHz: only 38–64 errors (read/write phase offsets partially cancel), confirming consistent clock-phase shift rather than random noise.
- APS6404L-SQH is rated for 144 MHz — 75 MHz is 52 % of IC rating. Bottleneck is QMI sampling + PCB signal integrity, not PSRAM IC speed.
- Note: earlier sweep runs showed inflated error counts because COOLDOWN=0/MIN_DESELECT=1 test combos violated APS6404L tCPH ≥ 18 ns, corrupting PSRAM QPI state for subsequent reads.

**CLKDIV=3 (25 MHz) QPI failure root cause:**
- Original PSRAM init (SPI mode, cmd=0x03) used M0.timing copy (CLKDIV=3/RXDELAY=2) and worked at 25 MHz.
- QPI mode has higher QMI internal pipeline latency than SPI (simultaneous 4-line latch vs single-line). At CLKDIV=3, RXDELAY=0 already exceeds the valid sampling window — would need negative RXDELAY (impossible). SPI mode is unaffected because single-line capture has lower pipeline depth.
- M0 (Flash) boot2 timing: `0x60007203` = CLKDIV=3, RXDELAY=2, COOLDOWN=1. Flash uses Quad SPI (cmd=single-wire), not full QPI — same lower pipeline depth as SPI.
- Confirmed by diagnostic: RXDELAY phase-shift pattern shows each +1 CLKDIV shifts the baseline sample point by one RXDELAY step (3.33 ns), leaving no valid RXDELAY at CLKDIV=3.

**Final production configuration:**

| Bus | Device | Mode | CLKDIV | RXDELAY | SCK | Throughput | Source |
|-----|--------|------|--------|---------|-----|------------|--------|
| M0 (Flash) | W25Q128JW | Quad SPI | 3 | 2 | 25 MHz | 100 Mbit/s | boot2 default |
| M1 (PSRAM) | APS6404L | QPI | 2 | 0 | 37.5 MHz | 150 Mbit/s | `psram_init()` |

PSRAM QPI at 37.5 MHz = 150 Mbit/s — a **6× improvement** over the original SPI 25 MHz (25 Mbit/s). Flash can be upgraded to CLKDIV=2/RXDELAY=1 (37.5 MHz, 150 Mbit/s) from RAM in future firmware.

**Rev B note:** Add external 4.7 kΩ pull-up to VCC_1V8 on **both** QSPI chip selects: QSPI_SS (Flash CS0) and GPIO0 (PSRAM CS1). GPIO0 boots with PDE=1 (pull-down), causing PSRAM CE# LOW at power-on → bus contention with Flash on shared QSPI bus. Both CS lines should be held HIGH during boot to prevent either device from being spuriously selected.

### Step 10 — Microphone (IM69D130 PDM)

| Item | Result | Notes |
|------|--------|-------|
| `mic` — PDM bit capture & 1-density check | ✅ PASS | 1-density = 49.8 % (silent room ≈ 50 % expected) |
| `mic_raw` — PDM density monitor (no amp, 10 s) | ✅ PASS | Density 50.0 %; responds to speech; omin/omax shift confirmed |
| `mic_loop` — mic → speaker real-time loopback | ⚠️ PARTIAL | Audio audible but background noise present; CLK-freeze hazard mitigated |
| `mic_rec` — record 3 s to SRAM then play back | ✅ PASS | Capture and playback both working; background noise under investigation |
| `mic_dump` — record 1 s, send raw PCM over serial | ✅ PASS | Binary int16 PCM received by `recv_pcm_dump.py`; WAV saved OK |

**mic_test output (1024 words = 32 768 PDM bits, 3.125 MHz):**

```
CLK = 3125.000 kHz (clkdiv=20)  SELECT=VDD (R-ch, falling edge)
1-density : 49.8 %  (silent room ≈ 50%)
Min word  : 0x0E959956
Max word  : 0xE8D4E4D5
Result: PASS
```

**Interface notes:**

The IM69D130 has no register interface (no I2C/SPI/UART). SELECT is hardware-tied to VDD → R channel (DATA2): data is valid after the CLK **falling edge** (tDV ≤ 100 ns) and before the rising edge; PIO samples on the rising edge.

Circuit: DATA line has a 100 Ω series resistor + 100 kΩ pull-down ∥ 47 pF to the MCU GPIO; VDD has a 100 nF decoupling capacitor. Internal pull-up not enabled (external 100 kΩ is pull-down; internal pull-up would cause ~1.2 V undefined voltage during HiZ window).

**PDM→PCM signal processing chain:**

- **Decimation:** 1-stage integrate-and-dump (CIC); every 2 × 32-bit FIFO words (= 64 PDM bits) → 1 PCM sample; `pcm = (ones−32) × 1024 × GAIN`
- **IIR LPF:** `filtered = (filtered×3 + pcm) >> 2`, α = 0.75, fc ≈ 2.25 kHz — attenuates high-frequency quantisation noise from PDM noise shaping
- **DC blocking HPF:** `dc_est += (filtered − dc_est) >> 10`, fc ≈ 7.6 Hz — removes DC offset
- **Warm-up:** 100 ms to settle state before recording, avoiding initial DC step
- **Output clamp:** ±MAX\_AMPLITUDE (50 % full scale) to protect the speaker

**PIO configuration:**

- PDM CLK = 3.125 MHz (clkdiv=20, SNR 69 dB mode); autopush = 32 bits
- 3,125,000 / 32 / 2 = 48,828 Hz — synchronised with I2S sample rate
- pio1 (PDM mic, GPIO 4/5) + pio0 (I2S amp, GPIO 30–32, gpio_base=16) run independently

**Current status:** Background noise audible in `mic_rec` and `mic_dump`. Likely cause: 1-stage CIC decimation has insufficient high-frequency attenuation (sinc¹ frequency response), allowing PDM noise-shaping energy to fold back into the passband. Candidates for improvement: 2-stage IIR LPF cascade or multi-stage CIC.

**OpenPDMFilter (sinc³) attempt — failed, reverted:**

Attempted to replace CIC+IIR with ST OpenPDMFilter (Apache-2.0, sinc³ cascade, LUT-accelerated). After integration, all PDM FIFO reads returned `0x00000000`; PCM output was a constant −1024. `mic_raw` (not using the filter) continued to show valid 49.8 % density — hardware is not at fault. Root cause not determined. Reverted at commit 1853c9e.

### Step 11 — J-Link Debug Validation

**Result: ✅ PASS**

Objective: validate the SWD debug interface beyond basic flash programming — confirm that
the full debug capability needed for Core 0/1 firmware development is functional.

| Item | Method | Result | Observations |
|------|--------|--------|--------------|
| Register read | J-Link Commander `regs` | ✅ PASS | PC=0x1000C53C (flash XIP, bringup REPL wait loop); SP=0x20081F50; XPSR=0x29000000 (no exception) |
| Memory read — SRAM | J-Link Commander `mem32 0x20001D38 16` | ✅ PASS | Address pattern residue `0xA5000000…0xA500000F` from last `sram_test` run |
| Memory read — flash XIP | J-Link Commander `mem32 0x10000000 8` | ✅ PASS | First word = `0x20082000` (MSP initial value = `__StackTop`) — ARM vector table correct |
| Memory read — peripheral | J-Link Commander `mem32 0xD0000000 8` | ✅ PASS | SIO CPUID = `0x000000C0` — Core 0 confirmed |
| Breakpoint + halt | GDB `break sram_test` + serial trigger | ✅ PASS | BP hit at `bringup_sram.c:25`; address `0x10003714` |
| Single-step execution | GDB `stepi` ×5 from reset | ✅ PASS | PC advanced correctly through TinyUSB startup code; source line shown at each step |
| Memory read via symbol | GDB `x/8xw &sram_test_buf` | ✅ PASS | GDB resolved symbol to `0x2001A67C` (Debug build layout); contents read correctly |
| Variable watch | GDB hardware watchpoint on `sram_test_buf[0]` | ✅ PASS | Watchpoint triggered at `bringup_sram.c:52` when `0xAAAAAAAA` written; `total_errors=0` confirmed |

**Firmware notes:**
- Debug build required (`-DCMAKE_BUILD_TYPE=Debug`); Release build optimises out local variables — GDB shows `<optimized out>`.
- `monitor reset halt` used to ensure clean board state before setting breakpoints.
- USB CDC disconnects while MCU is halted — expected behaviour; reconnects on `continue`.
- J-Link device must be specified as `RP2350_M33_0` (not `RP2350` — unknown alias). Use `RP2350_M33_1` for Core 1 debug (Step 16).
- Debug binary is larger; `sram_test_buf` moved from `0x20001D38` (Release) to `0x2001A67C` (Debug).
- Scripts: `jlink_step11a.jlink` (Commander), `step11b.gdb` / `step11c.gdb` (GDB batch).

**Debug toolchain (confirmed working):**
- J-Link Commander V9.32: `C:/Program Files/SEGGER/JLink_V932/JLink.exe`
- J-Link GDB Server: `JLinkGDBServerCL.exe -device RP2350_M33_0 -if SWD -speed 4000 -port 2331`
- GDB: `C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-gdb.exe`

**Future debug tooling improvements (planned):**
- VS Code + Cortex-Debug extension: GUI source-level debug with breakpoints, call stack, variable watch panel — eliminates need for GDB batch scripts.
- pyOCD or OpenOCD: alternative GDB server options (lower latency than J-Link GDB Server for iterative debug).
- Bringup debug script library: consolidated `.jlink` and `.gdb` scripts per component (SRAM, LoRa, sensors) for repeatable diagnostics.
- Dual-core debug: two J-Link GDB Server instances on ports 2331/2332 targeting `RP2350_M33_0` and `RP2350_M33_1` simultaneously — required for Step 16 IPC validation.

### Step 12 — BQ27441 Fuel Gauge Characterisation

**Result: ⚠️ CONDITIONAL** (I2C + CONFIG UPDATE + SOC/voltage validated; first charge/discharge cycle not yet performed)

| Item | Method | Result |
|------|--------|--------|
| I2C presence (battery installed) | scan_b after charge_on + power-cycle | ✅ PASS — 0x55 ACK |
| DEVICE_TYPE | CONTROL(0x0001) → read 0x00 | ✅ PASS — 0x0421 (BQ27441-G1) |
| CONFIG UPDATE — clear BIE | SET_CFGUPDATE → write OpConfig subclass 64 block 0 → SOFT_RESET | ✅ PASS — OpConfig 0x25F8 → 0x05F8; checksum 0x6A → 0x8A; exit OK |
| BAT_INSERT after BIE=0 | CONTROL(0x000C) | ✅ PASS — BAT_DET=1 confirmed |
| INITCOMP | Poll CTRL_STATUS bit7 after BAT_INSERT | ✅ PASS — set after 1600 ms |
| Voltage readout | `Voltage()` (0x04) | ✅ PASS — **3965 mV** (18650, reasonable) |
| Temperature | `Temperature()` (0x02) | ✅ PASS — **28.7 °C** (internal sensor) |
| SOC readout | `StateOfCharge()` (0x1C) | ✅ PASS — **77 %** (consistent with battery condition) |
| Remaining capacity | `RemainingCapacity()` (0x0C) | ✅ MEASURED — **932 mAh** (ROM default DesignCap=1000 mAh; will improve after learning) |
| Full charge capacity | `FullChargeCapacity()` (0x0E) | ✅ MEASURED — **1213 mAh** (first-boot OCV estimate; accurate after full cycle) |
| Average current | `AverageCurrent()` (0x10) | ⚠️ NOTE — +0 mA (charging disabled at boot; gauge averaging window not yet established) |
| State of Health | `StateOfHealth()` (0x20) | ⚠️ NOTE — 0 % / Unknown (expected on first boot; requires impedance data from full cycle) |
| Gauge learning cycle | Charge to full + discharge to empty | ⏳ PENDING — SOC and capacity accuracy improves after first cycle |

**Firmware notes:**
- BQ27441 in SHUTDOWN mode when battery first installed (I2C silent). Root cause: BIN pin unconnected in Rev A; BIE=1 (hardware detection default) → gauge never set BAT_DET → INITCOMP stuck at 0. Removing and re-inserting battery caused POR, exiting SHUTDOWN.
- Root cause of earlier NACK (Issue 9) revised: not a SLEEP wakeup issue. SLEEP and HIBERNATE both respond to I2C via ≤100 µs clock stretch. SHUTDOWN requires GPOUT wakeup; in this case POR (battery removal) resolved it.
- Fix: CONFIG UPDATE sequence clears BIE (OpConfig bit13) so BAT_INSERT command (0x000C) is accepted regardless of BIN pin state. SOFT_RESET (0x0042) applies the change and clears ITPOR.
- Production driver must run this CONFIG UPDATE once (check ITPOR=1 at POR) and also write DesignCapacity=890 mAh / DesignEnergy=3293 mWh (subclass 82) for accurate BL-4C SOC.
- GPOUT pin connection to MCU GPIO recommended in Rev B to enable clean SHUTDOWN wakeup without requiring battery removal.
- `bq27441` bringup command added to `bringup_power.c`; automatically performs CONFIG UPDATE + BAT_INSERT if BAT_DET=0; timeout extended to 15 s in `bringup_run.ps1`.
- **Issue 10 — Cold boot I2C latchup:** gauge permanently NACKs after any cold boot that includes battery insertion. I2C bus recovery (9 clocks + STOP) ineffective; other Bus B devices (0x36, 0x6B) respond normally on the same bus, confirming bus itself is not locked. Root cause under investigation (suspected ESD latchup on gauge I2C input pins during VBAT ramp). Rev A workaround: boot without battery → `charge_on` → insert battery. BIE=0 setting lost on every full POR (ITPOR=1 → ROM defaults reloaded), so CONFIG UPDATE must re-run each time.
- **Rev B consideration:** evaluate removing BQ27441 from BOM entirely. SOC estimation can be done in software from BQ25622 VBAT ADC + coulomb counting.

### Step 13 — TFT LCD Fast Refresh

**Result: ✅ PASS**

| Item | Method | Result |
|------|--------|--------|
| TE pin frequency | Count rising edges on GPIO 22 over 2 s | ✅ PASS — 120 edges → **60.0 Hz** |
| Baseline FPS (CPU polling) | 10 full-screen fills, `pio_sm_put_blocking`, clkdiv=4 | ✅ MEASURED — **1.46 FPS** (682 ms/frame, 0.225 MB/s) |
| DMA solid fill — clkdiv=4 | DMA_SIZE_8 + 2-byte ring buf → PIO TX FIFO, 10 fills | ✅ PASS — **59.88 FPS** (16.7 ms/frame, 9.2 MB/s) — **41× faster than CPU polling** |
| DMA solid fill — clkdiv=3 | Same DMA path, 96 ns write cycle, 10 fills | ✅ PASS — **79.37 FPS** (12.6 ms/frame, 12.2 MB/s) — no visible pixel glitches |
| TE-gated DMA fill | Gate frame start on TE rising edge, clkdiv=4, 10 frames | ✅ PASS — **54.35 FPS**; transfer fits within one frame period — tear-free capable |
| SRAM framebuffer DMA — clkdiv=4 | 150 KB SRAM buffer (gradient), read-increment DMA → PIO, 10 frames | ✅ PASS — **60.24 FPS** (16.6 ms/frame, 9.25 MB/s) — identical to solid fill |
| SRAM framebuffer DMA — clkdiv=3 | Same, 96 ns write cycle, 10 frames | ✅ PASS — **80.00 FPS** (12.5 ms/frame, 12.3 MB/s) — identical to solid fill |

**Conclusion:** PIO write speed is the sole bottleneck. SRAM framebuffer achieves the same FPS as solid fill — **PSRAM is not needed for display**. Production architecture: LVGL partial render to SRAM buffer → DMA → PIO → LCD.

**Firmware notes:**
- `tft_fast` bringup command added (`bringup_tft.c`: `tft_fast_test()`; `hardware_dma` added to CMakeLists).
- DMA method: `DMA_SIZE_8` byte transfers to `&pio1->txf[sm]`; DREQ = `pio_get_dreq(pio1, sm, true)`. 8-bit writes to the 32-bit FIFO register are promoted to 32-bit by the bus (byte replicated). PIO autopull threshold = 8 bits, shift-right → bits[7:0] of each FIFO word are consumed; correct value delivered. ✓
- Solid-colour fill uses a 2-byte ring buffer `{hi, lo}` with `channel_config_set_ring(false, 1)` (2^1 = 2 byte ring on read side). No framebuffer required for solid fills.
- TE enable: `TEON` command (0x35, mode=0) must be sent after `st7789_init()` — the standard init sequence does not include it. Without this command the TE pin is permanently low.
- clkdiv=3 (96 ns write cycle) is accepted by the ST7789VI at 1.8 V VDDI with no visible artefacts; datasheet recommends ~100 ns minimum at 1.8 V. **Production driver should use clkdiv=3** for comfortable frame budget headroom.
- At clkdiv=4 (16.7 ms/frame) the transfer time is nearly equal to one 60 Hz TE period (16.67 ms); TE-gated test shows occasional frame slip (54 FPS < 60 FPS). At clkdiv=3 (12.6 ms/frame) each transfer completes well within one period, leaving ~4 ms CPU time for rendering.
- CPU polling (`pio_sm_put_blocking`) is **not viable** for production: 682 ms/frame (1.46 FPS). DMA is mandatory.
- TE-gated pattern: wait for TE low → wait for TE rising edge → start DMA transfer. Synchronises frame start to V-blank for tear-free output. LVGL `flush_cb` should follow this pattern.

> The ST7789VI TE (tearing effect) pin is connected to GPIO 22. Production UI (LVGL) must synchronise `lv_display_flush_ready()` to TE to prevent tearing.

### Step 14 — GNSS Outdoor RF Test

**Result: ❌ FAIL**

Outdoor test performed with `gnss_tft_standalone.elf`. Teseo-LIV3FL NMEA streaming
confirmed over I2C; zero satellites tracked after >10 minutes with clear sky view.
RF chain issue suspected.

| Item | Result | Notes |
|------|--------|-------|
| Cold start TTFF | ❌ No fix | 0 satellites tracked after >10 min outdoors |
| Satellite count | ❌ 0 SVs | `$GPGGA` reports 0 sats; all `$GNGSA` slots empty; DOP = 99.0 |
| SNR per satellite | ❌ N/A | No `$GSV` output — nothing tracked |
| LNA enable state | ✅ PASS | BGA123N6 VPON = 1.8 V (ON-Mode, threshold ≥ 0.8 V) |
| Teseo NMEA / I2C | ✅ PASS | UTC incrementing; GGA/RMC/GSA streaming at 1 Hz |

**Firmware notes:**
- `gnss_tft_standalone.elf`: boots directly to live TFT display; no REPL required.
- New fixed-field layout: fix status, UTC, lat/lon/alt/HDOP, speed, TTFF, 12-row satellite SNR table sorted by C/N₀.
- `$PSTMSETPAR` sent at startup to enable NMEA_GNGSV_ENABLE + GLONASS + Galileo (ID 200, 227); saved to NVM via `$PSTMSAVEPAR`.
- RTC date = 2017-09-03 — stale; Teseo executing full cold start every boot.

**Root cause investigation (ongoing):**
- LNA (BGA123N6) confirmed powered and enabled; not the cause.
- 0 satellites tracked strongly suggests no RF signal reaching Teseo RF_IN.
- Planned: bypass BGA123N6 by jumper wire (SAW output → Teseo RF_IN direct) with VPON pulled to GND.
- Chip antenna ground clearance (open item in rf-matching.md) also under investigation.

> RF chain: Chip antenna (M830120) → B39162B4327P810 SAW → BGA123N6 LNA → Teseo-LIV3FL RF_IN.
> **Note:** design docs (rf-matching.md, hardware-requirements.md) incorrectly list BGA725L6; actual populated part is BGA123N6.

#### Step 14a — `$PSTMNOTCHSTATUS` runtime probe (Issue 11 investigation, 2026-04-11)

Attempted to retrieve ANF (Adaptive Notch Filter) status at runtime to identify
whether an in-band jammer is polluting the GPS path. Runtime probes were built
around `$PSTMNMEAREQUEST,<low>,<high>` (UM2229 §10.2.38), which per the manual is
a one-shot burst that should bypass the periodic NMEA scheduler.

**Result: ❌ runtime probe impossible on this firmware**

| Probe | Command | Target | Observed |
|-------|---------|--------|----------|
| P1 | `$PSTMNMEAREQUEST,0,1` | `$PSTMLOWPOWERDATA` (high bit 0) | ✗ echo only, no payload |
| P2 | `$PSTMNMEAREQUEST,0,2` | `$PSTMNOTCHSTATUS` (high bit 1)  | ✗ echo only, no payload |
| P3 | `$PSTMNMEAREQUEST,A0,0` | `$PSTMNOISE`+`$PSTMRF` (low bits 5/7) | ✅ both emit |
| P4 | `$PSTMNMEAREQUEST,A0,3` | all four combined | only NOISE+RF emit |

**Conclusion:** Teseo-LIV3FL firmware `BINIMG_4.6.15.1_CP_LIV3FL_ARM` **ignores
the high-word (`msglist_h`) argument of `$PSTMNMEAREQUEST` entirely.** Only
messages whose bit is in the low 32-bit word can be polled at runtime. Any
message at overall bit 32 or higher (`$PSTMLOWPOWERDATA`, `$PSTMNOTCHSTATUS`,
`$PSTMTM`, `$PSTMPV`, `$PSTMPVQ`, `$PSTMUTC`, `$PSTMADCDATA`, `$PSTMUSEDSATS`,
`$PSTMEPHEM`, `$PSTMALMANAC`, `$PSTMBIASDATA`, etc.) is unreachable via
`$PSTMNMEAREQUEST`.

**CDB state snapshot (read-only via `gnss_probe`):**

| CDB-ID | Purpose | Value |
|--------|---------|-------|
| 1200 | Application ON/OFF 1 | `0x096B165C` |
| 1227 | Application ON/OFF 2 | `0x0401048D` |
| 1231 | NMEA I2C msg list LOW  | `0x00980056` (default: GGA/GSA/VTG/RMC/GSV/GLL/PSTMCPU) |
| 1232 | NMEA I2C msg list HIGH | `0x00020000` (NOTCHSTATUS bit 0x2 is clear; LOWPOWERDATA bit 0x1 is clear) |

**Observed RF telemetry during probe (at desk, indoor):**

```
$PSTMNOISE,12500,12500    ← GPS / GLONASS noise floor, consistent
$PSTMRF,1,1,00,,,,,,,,,,,, ← sat_type=1, n_sat=0 (matches "0 SVs tracked")
```

**Implication for Issue 11:** `$PSTMNOTCHSTATUS` cannot be used as a diagnostic
without a destructive persistent CDB-1232 write (`$PSTMSETPAR,1232,…` →
`$PSTMSAVEPAR` → `$PSTMSRR`). Issue 11 must therefore be pursued via physical
RF measurements (antenna continuity, SAW insertion loss, LNA bias, shielding
experiments, BGA123N6 bypass) rather than firmware-side jammer detection.

### Step 15 — LoRa RF Performance

**Result: ✅ PARTIAL PASS** (validated via Meshtastic mesh — see Step 17)

SPI interface and RX mode entry confirmed in Step 12. RF link validated by running
Meshtastic firmware and successfully exchanging text messages with nearby nodes.

| Item | Method | Expected result | Actual result |
|------|--------|----------------|---------------|
| TX/RX packet | Meshtastic mesh exchange with nearby node (TNGBpicoC_ebe7) | Bidirectional text messages | ✅ PASS — messages sent and received via Web Console |
| Channel utilization | `meshtastic --info` | Non-zero airtime | ✅ PASS — channelUtilization=0.40%, airUtilTx=0.011% |
| RSSI at known distance | ⏳ | RSSI consistent with path loss model | PENDING — not yet measured |
| Sensitivity | ⏳ | Long-range test (SF12, BW125) | PENDING — outdoor range test required |
| OCP investigation | Reg read | 0x08E7 = 0x38 (140 mA) after SetPaConfig | ✅ PASS — formula is value × 2.5 mA; old "185 mA" was wrong formula |
| Frequency accuracy | ⏳ | TCXO ±0.5 ppm spec | PENDING — spectrum analyser required |

### Step 16 — Core 1 Functionality

**Result: ✅ PASS** (Stage A ✅; B-0 ✅; B SMP ✅; D Core 0 single ✅; C Core 1 USB ❌; B2 ✅; E ✅; F ✅; G ✅; H ✅)

All bringup to date has run on Core 0. Core 1 functionality must be validated separately
before starting UI/application firmware development.

#### Stage A — Bare-Metal Core 1

| Item | Method | Expected result | Actual result |
|------|--------|----------------|---------------|
| Core 1 boot | Launch Core 1 via `multicore_launch_core1()` | Core 1 enters its entry function | ✅ PASS — `C1_READY` token `0xC1B00700` received |
| Inter-core FIFO (SIO) | Core 0 pushes value; Core 1 reads and echoes back | Round-trip confirmed | ✅ PASS — 4/4 probe values echoed correctly |
| Shared SRAM access | Both cores read/write a shared buffer with spinlock | No data corruption | ✅ PASS — Core 1 read `0xBEEFC0DE` as written by Core 0 |
| Core 1 peripheral access | Core 1 controls an LED or GPIO | Peripheral access from Core 1 confirmed | ✅ PASS — motor pin (GPIO 9) toggled from Core 1 |
| Core 1 REPL (Stage B-0) | Run full bringup REPL on Core 1; Core 0 owns USB CDC init | All REPL commands work identically to Core 0 run | ✅ PASS — imu/baro/lora/sram confirmed on Core 1 |

#### Stage B — FreeRTOS SMP Heartbeat (Core 1 Affinity)

**Result: ✅ PASS** (2026-04-04)

- `configNUMBER_OF_CORES = 2` (SMP), `configTICK_CORE = 0`, `configUSE_CORE_AFFINITY = 1`
- Core 0 calls `stdio_init_all()` + `vTaskStartScheduler()` (owns USB + tick)
- Heartbeat task pinned to Core 1 via `xTaskCreateAffinitySet(mask = 1<<1)`
- Observed 5/5 heartbeats on Core 1, tick count incrementing correctly
- Firmware: `firmware/tools/bringup/core1_freertos_test.c`

#### Stage C — FreeRTOS + USB on Core 1 Only

**Result: ❌ FAIL** (2026-04-04)

- `configNUMBER_OF_CORES = 1`, Core 1 calls `stdio_init_all()` + `vTaskStartScheduler()`
- USB CDC briefly enumerates during `busy_wait_ms(2000)` but dies after scheduler starts
- Root cause: Pico SDK `runtime_init()` only runs on Core 0; default alarm pool's timer IRQ
  fires on Core 0's NVIC. `stdio_usb_init()` periodic `tud_task()` alarm triggers on Core 0,
  but `low_priority_worker_irq` registered on Core 1 → `tud_task()` never called → USB dies
- **Conclusion: USB CDC is architecturally tied to Core 0 in the Pico SDK**

#### Stage D — Single-Core FreeRTOS + USB on Core 0

**Result: ✅ PASS** (2026-04-04)

- `configNUMBER_OF_CORES = 1`, Core 0 calls `stdio_init_all()` + `vTaskStartScheduler()`
- FreeRTOSConfig aligned with official `pico-examples/freertos/FreeRTOSConfig_examples_common.h`:
  `configMINIMAL_STACK_SIZE = 512`, `configTIMER_TASK_STACK_DEPTH = 1024`,
  `configTOTAL_HEAP_SIZE = 64 KB`, `configUSE_RECURSIVE_MUTEXES = 1`
- Linked `pico_async_context_freertos` per official example
- SYNC/TIME interop auto-enabled (safe — scheduler on same core as USB)
- Infinite heartbeat confirmed stable (13+ iterations observed, Core 0, tick correct)

#### Stage B2 — FreeRTOS + Manual TinyUSB CDC on Core 1 (Plan B)

**Result: ✅ PASS** (2026-04-04)

- Bypasses `pico_stdio_usb` entirely (hard-asserts Core 0 in SDK 2.2.0)
- Core 1 calls `tusb_init()` directly → USBCTRL_IRQ on Core 1's NVIC
- Busy-polls `tud_task()` for 2 s to complete USB enumeration before scheduler
- High-priority FreeRTOS task continues `tud_task()` polling after scheduler starts
- CDC output via `tud_cdc_write()` / `tud_cdc_write_flush()` (not printf/stdio)
- `configNUMBER_OF_CORES = 1`, `SYNC/TIME_INTEROP = 0`
- `PICO_CORE1_STACK_SIZE = 0x1000` (4 KB, up from default 2 KB)
- USB descriptors from `stdio_usb_descriptors.c` (linked but `stdio_usb_init` never called)
- Infinite heartbeat confirmed stable (9+ iterations, Core 1, tick correct at 2000/2s)
- **Known issue:** after J-Link flash, USB may not enumerate until power cycle (USB re-plug).
  Root cause: J-Link resets Core 0 only; USB peripheral state machine left dirty.
  Workaround: re-plug USB after flashing.

#### Architecture Decision (2026-04-04, revised)

Based on Stages B–D and B2 findings, the production architecture preserves the
original dual-core license boundary:

```
Core 0 (GPL-3.0):   Meshtastic modem (bare-metal / Arduino-Pico)
                     SPI → SX1262, USB serial API for phone connection
                     No FreeRTOS, no RTOS interop

Core 1 (Apache-2.0): FreeRTOS + manual TinyUSB CDC + LVGL + MIE
                     tusb_init() on Core 1; tud_task() polled by FreeRTOS task
                     Display (PIO 8080), keyboard (PIO matrix), sensors (I2C)

IPC (MIT):           ipc_protocol.h + HW FIFO + shared SRAM
```

Key findings:
1. **`pico_stdio_usb` is Core 0-only** — hard assert in SDK 2.2.0 (Stage C)
2. **Manual `tusb_init()` + `tud_task()` works on Core 1** — Plan B bypasses
   the SDK's alarm-pool limitation (Stage B2)
3. **FreeRTOS SYNC/TIME interop must be disabled** — Core 0 is bare-metal with
   no scheduler; interop would redirect SDK calls to non-existent FreeRTOS
4. **License boundary preserved** — separate compilation units, MIT IPC header
   as sole crossing point; no GPL-3.0 contamination of Apache-2.0 code

#### Stage E — Multi-Task FreeRTOS on Core 1

**Result: ✅ PASS** (2026-04-04)

| Item | Method | Expected result | Actual result |
|------|--------|----------------|---------------|
| Multi-task CDC | 2 tasks (Writer-A / Writer-B) write CDC concurrently via mutex, 15 iters × 2 s | Interleaved output, no corruption, stable 30 s | ✅ PASS — both writers completed 15/15, lines interleaved cleanly, no corruption |

#### Stage F — SPI from Core 1 FreeRTOS Task

**Result: ✅ PASS** (2026-04-04)

| Item | Method | Expected result | Actual result |
|------|--------|----------------|---------------|
| SX1262 BUSY after reset | Hardware reset + poll BUSY | BUSY LOW within 200 ms | ✅ PASS — BUSY LOW |
| GetStatus | SPI GetStatus (0xC0) | ChipMode=2 (STBY_RC) | ✅ PASS — Status=0x2A; ChipMode=2, CmdStatus=5 |
| SyncWord | ReadRegister 0x0740/41 | 0x14 0x24 (private net default) | ✅ PASS — 0x14 0x24 |

#### Stage G — HW FIFO IPC Between Cores

**Result: ✅ PASS** (2026-04-04)

| Item | Method | Expected result | Actual result |
|------|--------|----------------|---------------|
| FIFO round-trip (100 msg) | Core 0 sends `C0_EFGH_IPC_BASE\|seq`; Core 1 FreeRTOS task echoes `C1_EFGH_IPC_BASE\|seq` | 100/100 OK, no token errors | ✅ PASS — 100/100; FIFO-timeout=0; token-err=0 |
| Shared SRAM integrity | Core 0 writes `0xABCD0000\|seq` before each push; Core 1 reads and verifies | 0 SRAM mismatches | ✅ PASS — sram-err=0 |

#### Stage H — I2C Sensor from FreeRTOS Task

**Result: ✅ PASS** (2026-04-04)

| Item | Method | Expected result | Actual result |
|------|--------|----------------|---------------|
| LSM6DSV16X IMU | Core 1 FreeRTOS task: SW reset → configure → one-shot read | STATUS=0x07; Z≈1g | ✅ PASS — STATUS=0x07; X=−0.022g Y=+0.017g Z=+0.908g |
| LPS22HH Baro | Core 1 FreeRTOS task: SW reset → ONE_SHOT → burst read | STATUS=0x03; plausible P/T | ✅ PASS — STATUS=0x03; P=1007.67 hPa; T=30.42 °C |
| LIS2MDL Mag | Core 1 FreeRTOS task: SW reset → continuous → poll → burst read | STATUS=0x0F; Earth-field values | ✅ PASS — STATUS=0x0F; X=−66.0 Y=−268.5 Z=+318.0 mGauss |

**Firmware notes:**
- Stage A implemented in `firmware/tools/bringup/bringup_core1.c`; command: `core1`
- Protocol: Core 1 sends `C1_READY (0xC1B00700)` on boot; Core 0 sends command tokens (`C0_CMD_ECHO/SRAM/GPIO/SHUTDOWN`) via SIO FIFO
- All FIFO pops on Core 0 use `multicore_fifo_pop_timeout_us()` to prevent hangs
- `pico_multicore` + `hardware_sync` added to bringup CMakeLists
- `multicore_reset_core1()` called after shutdown to leave Core 1 in clean state
- FreeRTOS-Kernel V11.3.0 added as submodule at `firmware/core1/freertos-kernel/` (MIT); nested submodule `Community-Supported-Ports` provides `RP2350_ARM_NTZ` Cortex-M33 port
- Stage B-0 (`core1_bringup_test`): `bringup_repl_run()` split into `bringup_repl_init()` (Core 0; registers USB CDC IRQ on Core 0's NVIC) and `bringup_repl_loop()` (Core 1; REPL loop only). `multicore_reset_core1()` called before `multicore_launch_core1()` — required because J-Link only resets Core 0 when reflashing; Core 1 may still be in user code and will not respond to the FIFO handshake unless explicitly PSM-cycled first
- Stage B/D FreeRTOSConfig: aligned with official `pico-examples/freertos/FreeRTOSConfig_examples_common.h`; linked `pico_async_context_freertos`
- Stage B2: bypasses `pico_stdio_usb`; calls `tusb_init()` + `tud_task()` directly on Core 1; `PICO_CORE1_STACK_SIZE=0x1000`; `configSUPPORT_PICO_SYNC_INTEROP=0`, `configSUPPORT_PICO_TIME_INTEROP=0`
- Stages E–H: `firmware/tools/bringup/core1_rtos_efgh.c` (new ELF target `core1_rtos_efgh`); uses Plan B architecture; inline SPI/I2C driver helpers; yield-friendly FIFO polling (`multicore_fifo_rvalid()` + `vTaskDelay(1)`) in Stage G task; `busy_wait_ms()` used in sensor init (not `sleep_ms()`) to avoid SDK alarm-pool dependency on Core 1; `INCLUDE_vTaskDelete = 1` enabled in `FreeRTOSConfig.h`; Stage E reduced to 3 iter × 200 ms for fast validation
- Stage G IPC: Core 0 writes shared SRAM (`g_ipc_sram = 0xABCD0000|seq`), issues `__dmb()` barrier, then pushes FIFO; Core 1 reads SRAM after FIFO pop; integrity verified by Core 1 and reported via CDC
- USB auto-enumerate fix: `multicore_reset_core1()` in Core 0 `main()` PSM-cycles Core 1 before relaunch; Core 1 `core1_entry()` calls `reset_block(RESETS_RESET_USBCTRL_BITS)` + `busy_wait_ms(20)` + `unreset_block_wait()` before `tusb_init()` — holds USB peripheral in reset long enough (>2.5 ms) for the Host OS to detect disconnect and process a clean reconnect without physical replug
- Charger startup (`charger_startup_init()`): runs before `tusb_init()` on Core 1 boot; disables BQ25622 charging immediately (CTRL1=0x80), probes BQ27441 at 0x55 (3× retry, read transaction), re-enables charging if ACK received — suppresses inductor noise when no battery is installed. BQ27441 probe currently always returns NACK (see Issue 9); battery detection via STATUS0 VSYS_STAT pending
- Firmware: `firmware/tools/bringup/core1_rtos_efgh.c`, `FreeRTOSConfig.h`, `CMakeLists.txt`

> Production architecture fully validated on real hardware: Core 0 = bare-metal
> Meshtastic (GPL-3.0), Core 1 = FreeRTOS + manual TinyUSB + UI (Apache-2.0).
> IPC via HW FIFO + shared SRAM with MIT-licensed `ipc_protocol.h` as sole
> crossing point. Step 16 complete.

### Step 17 — Meshtastic Firmware Integration

**Result: ✅ PASS** (2026-04-06)

Meshtastic firmware (v2.7.15 dev branch) compiled and running on MokyaLora Rev A
as a minimal LoRa-only modem. Text messaging with nearby mesh nodes confirmed.

#### Build Configuration

- **Source:** `tengigabytes/firmware` branch `feat/rp2350b-mokya`, based on Meshtastic `v2.7.15.d18f3f7a6` (develop)
- **PlatformIO env:** `rp2350b-mokya` (extends `rp2350_base`, board=`rpipico2`)
- **Variant:** `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/`
- **hwModel:** `RPI_PICO2` (79)
- **Build size:** Flash 286 KB (7.8%), RAM 62 KB (12.0%)

#### Variant Configuration Summary

| Setting | Value | Notes |
|---------|-------|-------|
| SPI1 (LoRa) | SCK=26, MISO=24, MOSI=27, CS=25 | `HW_SPI1_DEVICE` |
| SX1262 control | RESET=23, DIO1=29 (IRQ), BUSY=28 | DIO0=NC, DIO2=NC (HW RF switch) |
| TCXO | `SX126X_DIO3_TCXO_VOLTAGE 1.8` | DIO3 drives 1.8 V TCXO supply |
| RF switch | `SX126X_DIO2_AS_RF_SWITCH` | DIO2 → PE4259 directly, no MCU GPIO |
| I2C | Excluded entirely | `MESHTASTIC_EXCLUDE_I2C=1` |
| Display | Excluded | `MESHTASTIC_EXCLUDE_SCREEN=1` |
| GPS | Excluded | `MESHTASTIC_EXCLUDE_GPS=1` |
| Bluetooth | Excluded | `MESHTASTIC_EXCLUDE_BLUETOOTH=1` |
| WiFi/MQTT | Excluded | `MESHTASTIC_EXCLUDE_WIFI=1`, `MESHTASTIC_EXCLUDE_MQTT=1` |
| Battery ADC | Disabled | `#undef BATTERY_PIN` (BQ25622 handles via I2C on Core 1) |
| Serial | USB CDC (`DEBUG_RP2040_PORT=Serial`) | Protobuf API over USB |

#### Excluded Modules

All optional modules disabled for minimal LoRa modem build:
`AUDIO`, `DETECTIONSENSOR`, `ENVIRONMENTAL_SENSOR`, `HEALTH_TELEMETRY`,
`EXTERNALNOTIFICATION`, `PAXCOUNTER`, `POWER_TELEMETRY`, `RANGETEST`,
`REMOTEHARDWARE`, `STOREFORWARD`, `ATAK`, `CANNEDMESSAGES`, `INPUTBROKER`,
`SERIAL`, `POWERSTRESS`, `POWERMON`, `POWER_FSM`, `TZ`.

**Retained:** Admin (serial config), TextMessage, PKI/PKC, Traceroute, NeighborInfo, Waypoint.

#### Functional Test Results

| Item | Method | Result | Notes |
|------|--------|--------|-------|
| USB CDC serial | Connect via COM11, 115200 | ✅ PASS | VID 2E8A, PID 000F |
| Protobuf API (read) | `meshtastic --port COM11 --info` | ✅ PASS | All config sections readable |
| Protobuf API (write) | Web Console set region/channels | ✅ PASS | Config persisted across reboot |
| Node discovery | Nearby node TNGBpicoC_ebe7 (`!538eebe7`) | ✅ PASS | Appeared in NodeDB automatically |
| Text message TX/RX | Web Console bidirectional messaging | ✅ PASS | Messages exchanged with nearby node |
| Channel utilization | `meshtastic --info` | ✅ PASS | channelUtilization=0.40%, airUtilTx=0.011% |
| PKI encryption | `hasPKC: true` in metadata | ✅ PASS | Public key generated and advertised |
| Multi-channel | 4 channels configured (Primary + 3 secondary) | ✅ PASS | All channels operational |

#### Bug Found and Fixed: Wire1 / SPI1 GPIO Conflict (Issue 12)

**Symptom:** SX1262 radio returns `RADIOLIB_ERR_CHIP_NOT_FOUND` (Error 4, NO_INTERFACE) during `lora.begin()`.

**Root cause:** `rpipico2` board defaults define `PIN_WIRE1_SDA=26`, `PIN_WIRE1_SCL=27`.
Meshtastic `main.cpp` calls `Wire1.begin()` at startup, which reconfigures GPIO 26/27
funcsel from SPI (funcsel=1) to I2C (funcsel=3). This destroys SPI1 SCK and MOSI
used by LoRa, because `Wire1.begin()` runs before `SPI1.begin()`.

**Diagnosis:** SWD register read of GPIO 26 CTRL confirmed funcsel=3 (I2C) instead
of funcsel=1 (SPI). Non-destructive SWD read (no MCU reset) used to preserve USB
CDC session during inspection.

**Fix (initial):** Added `I2C_SDA1=6`, `I2C_SCL1=7` to `variant.h` — redirects Wire1 to
power-bus pins (away from SPI1).

**Fix (final):** Replaced with `MESHTASTIC_EXCLUDE_I2C=1` in `platformio.ini` — disables
all Wire init entirely. Cleaner solution: Core 0 has no I2C peripherals to manage;
I2C will be handled by Core 1.

#### Known Issues

| Issue | Impact | Notes |
|-------|--------|-------|
| Protobuf parse error on first `FromRadio` message | Low | Python CLI (2.7.8) and Web Console see `DecodeError` on one message; CLI recovers gracefully. Caused by firmware (2.7.15 dev) protobuf schema being newer than client. Does not affect functionality |
| Web Console settings page fails to load | Medium | Settings page spins indefinitely; messaging works fine. Same root cause: protobuf version mismatch between firmware (2.7.15 dev) and Web Console (stable). Workaround: use `meshtastic` Python CLI for config |
| Config reset on reflash | Expected | J-Link flash erases NVS; region and channels must be reconfigured. Not a bug |

#### Integration Notes for Core 1

When Core 1 firmware is developed, the following applies:

1. **Core 0 owns USB CDC** — Meshtastic uses `pico_stdio_usb` (Serial). Core 1 must use
   manual TinyUSB (Plan B architecture from Step 16 Stage B2) or defer USB to Core 0.
2. **I2C is Core 1's domain** — all I2C peripherals (sensors, power ICs, GNSS) excluded
   from Core 0 via `MESHTASTIC_EXCLUDE_I2C`. Core 1 manages both buses.
3. **IPC required for:** GPS position (Core 1 → Core 0 via `IPC_MSG_POSITION`), device
   status, text message display, and config forwarding.
4. **No GPIO conflicts** with current Core 0 config — SPI1 (GPIO 24-28) and DIO1 (GPIO 29)
   are the only pins used. All other GPIOs are free for Core 1.
5. **SPI1 bus sharing:** if Core 1 needs SPI1 access (e.g., for additional peripherals),
   a mutex or IPC arbitration will be required. Currently Core 0 exclusively owns SPI1.

**Firmware files:**
- `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/variant.h`
- `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/platformio.ini`

### Step 18 — Interactive Menu System (LCD + Keyboard)

**Result: ✅ PASS** (2026-04-06)

Standalone interactive menu system on TFT display with physical keyboard navigation.
Device can now be operated without a serial connection.

#### Features Implemented

| Feature | Description | Status |
|---------|-------------|--------|
| LCD menu | 8 category pages (Sensors, Power, Audio, Memory, LoRa, Display, Keyboard, Core 1) with all 51+ bringup commands accessible | ✅ Working |
| Keyboard nav | UP/DOWN/OK/BACK key navigation with 80 ms debounce, edge detection | ✅ Working |
| Serial priority | Any serial byte switches to MS_SERIAL_ACTIVE; returns to menu after 2 s idle | ✅ Working |
| Screen rotation | MADCTL-based 0°/90°/180°/270° rotation with dynamic layout (CASET/RASET swap) | ✅ Working |
| TFT lifecycle | Menu claims PIO1; tests claim/release their own; menu_tft_reinit() restores after test return | ✅ Working |

**Architecture:** State machine (MS_MENU / MS_TEST_RUNNING / MS_SERIAL_ACTIVE) integrated into main REPL loop. 50 Hz polling of serial + keyboard. Shared 5×8 font and TFT drawing primitives extracted from `bringup_gnss_tft.c`.

**Firmware files:**
- `firmware/tools/bringup/bringup_menu.c` (NEW)
- `firmware/tools/bringup/bringup_menu.h` (NEW)
- `firmware/tools/bringup/i2c_custom_scan.c` (main loop rewrite)

**Backward compatibility:** `bringup_run.ps1` serial automation fully preserved — all commands produce identical output.

---

### Step 19 — Automated Regression Test Suite

**Result: ⚠️ PARTIAL** (2026-04-06)

PowerShell test harness (`bringup_test_all.ps1`) executes all bringup commands over serial and validates output against regex patterns.

#### Test Results

| Group | Tests | Result | Notes |
|-------|-------|--------|-------|
| Sensors | 5 (imu, baro, mag, gnss_info, scan_a) | ✅ 5/5 PASS | |
| Power | 4 (scan_b, status, adc, bq27441) | ✅ 4/4 PASS | |
| Audio | 2 (amp_test, mic) | ✅ 2/2 PASS | |
| Memory | 4 (sram, flash, psram, psram_full) | ⚠️ 2/4 PASS | psram and psram_full return empty output in full run (Issue 13) |
| LoRa | 2 (lora, lora_dump) | ✅ 2/2 PASS | |
| Display | 2 (tft, tft_fast) | ✅ 2/2 PASS | |
| Core 1 | 1 (core1) | ✅ 1/1 PASS | |
| **Total** | **20** | **18/20 reliable** | Memory group alone: 4/4 PASS |

**Key finding:** PSRAM tests pass 100% when run in isolation (`-Group memory`) or via `bringup_run.ps1` with identical command sequence. Failure is specific to `bringup_test_all.ps1` when running 12+ commands before PSRAM — USB CDC serial timing issue, not a firmware bug.

**TFT test restructure:** `tft_test` now performs only 5 solid color fills (Red/Green/Blue/White/Black). All pattern tests (colour bars, checkerboard, bouncing block, backlight fade) moved to `tft_fast_test` as DMA sub-tests.

**Firmware file:** `firmware/tools/bringup/bringup_tft.c` (restructured)
**Test script:** `bringup_test_all.ps1` (NEW)

---

### Step 20 — Menu Diagnostic Consolidation & PIO Fix

**Result: ✅ PASS** (2026-04-07)

Consolidated redundant bringup menu commands, fixed a PIO state-machine leak that caused TFT black-screen hangs, and added BACK key interrupt support to all long-running test functions.

#### Changes

| Item | Before | After | Result |
|------|--------|-------|--------|
| `scan_a` + `dump_a` | Two separate commands (serial-only output) | Single `scan_a` / "Bus A Diag" — I2C scan + WHO_AM_I probe + register dump; results on TFT (green/red per device) and serial | ✅ PASS |
| Bus A Diag TFT display | N/A | Title, 4 device rows (addr + name + ID status), summary "Result: X/4 pass", BACK hint; waits for BACK key before returning to menu | ✅ PASS |
| PIO double-claim fix | `gnss_tft_test`, `tft_test`, `tft_fast_test` each called `menu_tft_start()` without releasing the menu's existing PIO SM → two SMs driving same TFT data pins → bus contention, SM leak, black screen | Added `menu_tft_stop()` at entry of all three functions; menu reclaims PIO via `menu_tft_reinit()` on return | ✅ PASS |
| BACK key interrupt | Long-running tests (audio, LoRa RX, LED cycle, motor breathe, mic) could only be stopped via serial | Added `back_key_pressed()` checks to: `amp_test`, `amp_breathe`, `amp_bee`, `mic_raw`, `mic_loopback`, `lora_rx`, `lm27965_cycle`, `motor_breathe` | ✅ PASS |
| Power page TFT consolidation | `status`, `adc`, `bq27441`, `scan_b`, `dump_b` — 5 serial-only commands | 3 TFT diagnostic screens: "Bus B Diag" (scan+probe), "Charger Diag" (status+ADC, 1 Hz live), "Gauge Diag" (V/I/SOC/Temp/Cap/SOH, 1 Hz live) | ✅ PASS |
| Bus B Diag TFT display | N/A | I2C scan + per-device probe (BQ25622 Part Info, BQ27441 Device Type, LM27965 GP read); TFT green/red + serial register dump | ✅ PASS |
| Charger Diag TFT display | N/A | VBUS/VBAT/VSYS/IBUS/IBAT + charge state, 1 Hz refresh (overwrite values only, no full-screen clear to avoid flicker) | ✅ PASS |
| Gauge Diag TFT display | N/A | VBAT/Current/SOC/Temp/Capacity/SOH, 1 Hz refresh; NACK path shows red error on TFT with BACK wait | ✅ PASS |
| LED control rewrite | `lm27965_cycle()` — fixed cycle demo, no individual control | `led_control()` — interactive TFT UI: UP/DN select bank, LT/RT adjust duty, OK toggle on/off; per-bank A (TFT-BL, 0-31), B (Kbd+Grn, 0-31), C (Red, 0-3) | ✅ PASS |
| BQ25622 IINDPM | 100 mA — too low for system without battery, caused VSYS droop and screen flicker on Charge ON | 500 mA — sufficient headroom for system load + charge current | ✅ PASS |
| Power menu cleanup | 10 entries (5 readback + 5 control) | 8 entries: Bus B Diag, Charger Diag, Gauge Diag, Charge ON/OFF/scan, LED control, Motor breathe | ✅ PASS |
| Memory menu consolidation | 9 serial-only commands (`sram`, `psram`, `psram_full`, `psram_speed`, `psram_diag`, `flash_speed`, etc.) | 5 TFT entries: Memory Diag, PSRAM full 8MB, PSRAM Speed, PSRAM Tuning, PSRAM Debug — all display results on TFT with BACK key | ✅ PASS |
| Memory Diag (TFT) | N/A | Single-screen 5-test diagnostic: SRAM 16KB 5-pattern, Flash JEDEC, Flash QE bit, PSRAM Init QPI, PSRAM 4KB pattern; per-test PASS/FAIL rows | ✅ PASS |
| PSRAM full 8MB (TFT) | Serial-only | 8MB write+verify with per-MB TFT progress, BACK key cancellation | ✅ PASS |
| PSRAM Speed (CPU XIP) | DMA-based speed test (unreliable, see Issue 14) | CPU volatile access only: Write 8MB 4041 ms (2027 KB/s), Read+Verify 8MB 5019 ms (1632 KB/s), 0 errors | ✅ PASS |
| PSRAM Tuning (TFT) | Serial-only | Merged psram_speed + psram_diag + flash_speed; TFT shows best CLKDIV/RXDELAY/MHz for PSRAM and Flash | ✅ PASS |
| PSRAM Debug (TFT) | 3 tests (SPI ID, SPI Wr/Rd, XIP Sentinel) | Simplified to 2 tests: Init QPI + XIP Sentinel (SPI probe redundant if XIP works) | ✅ PASS |
| TFT Test result page | `tft_test`: no timing, serial-only | Each fill shows ms + FPS on serial; LCD result summary (5 colours × ms/FPS table, scale=2, BACK wait) | ✅ PASS |
| TFT Fast result page | `tft_fast_test`: serial-only FPS | LCD result summary after all sub-tests (7 modes × FPS table: TE freq, CPU cd=4, DMA cd=4/3, TE-gate, FB cd=4/3; scale=2, BACK wait) | ✅ PASS |
| TFT backlight-off fix | `tft_test` / `tft_fast_test` turned backlight off at exit → black screen when returning to menu after result page | Removed backlight-off — menu system manages backlight lifecycle | ✅ PASS |

TFT screen layouts for all diagnostic screens: see [tft-layouts.md](tft-layouts.md).

**Firmware files:**
- `firmware/tools/bringup/bringup_sensors.c` — `scan_bus_a()` replaces `dump_bus_a()`
- `firmware/tools/bringup/bringup_menu.c` — `cmd_scan_a()` wrapper with BACK wait; "Bus A Diag" menu entry; Power page updated: 5 readback → 3 TFT diag, LED cycle → LED control
- `firmware/tools/bringup/bringup_gnss_tft.c` — `menu_tft_stop()` at entry
- `firmware/tools/bringup/bringup_tft.c` — `menu_tft_stop()` at entry of `tft_test()` and `tft_fast_test()`
- `firmware/tools/bringup/bringup_menu.h` — `MC_OK` (green) and `MC_ERR` (red) colour constants
- `firmware/tools/bringup/bringup_amp.c` — `back_key_pressed()` in `amp_test`, `amp_breathe`, `amp_bee` (split from bringup_audio.c in Step 21)
- `firmware/tools/bringup/bringup_mic.c` — `back_key_pressed()` in `mic_raw`, `mic_loopback` (split from bringup_audio.c in Step 21)
- `firmware/tools/bringup/bringup_lora.c` — `back_key_pressed()` in `lora_rx`
- `firmware/tools/bringup/bringup_power.c` — `scan_bus_b()`, `motor_breathe()` with `back_key_pressed()`; IINDPM 100→500 mA (split from original in Step 21)
- `firmware/tools/bringup/bringup_charger.c` — `charger_diag()`, `bq25622_enable/disable_charge()` (split from bringup_power.c in Step 21)
- `firmware/tools/bringup/bringup_gauge.c` — `gauge_diag()`, `bq27441_read()` with BIE fix (split from bringup_power.c in Step 21)
- `firmware/tools/bringup/bringup_led.c` — `led_control()` (replaces `lm27965_cycle()`) (split from bringup_power.c in Step 21)
- `firmware/tools/bringup/bringup.h` — umbrella header; added `scan_bus_b`, `charger_diag`, `gauge_diag`, `led_control` declarations
- `firmware/tools/bringup/bringup_psram.c` — `psram_init()`, `psram_test()`, speed/diag sweep (split from bringup_sram.c in Step 21)
- `firmware/tools/bringup/bringup_memory_tft.c` — `cmd_memory_diag()`, `cmd_psram_full_tft()`, `cmd_psram_tuning()`, `cmd_psram_debug()`, `cmd_psram_dma_test()` (CPU-only speed test), `psram_rd_diag()` (split from bringup_sram.c in Step 21)
- `firmware/tools/bringup/bringup_flash.c` — `flash_test()`, `flash_speed_test()`, `flash_speed_run()` (expanded from 51→145 lines in Step 21)
- `firmware/tools/bringup/i2c_custom_scan.c` — `dump_a` removed; `scan_a` calls `scan_bus_a()`; `scan_b` calls `scan_bus_b()`; `led` calls `led_control()`; added `mem_diag`, `psram_full_tft`, `psram_tuning`, `psram_debug`, `psram_dma`, `psram_rd_diag` serial commands

---

### Step 21 — Bringup Code Modularisation

**Result: ✅ PASS** (2026-04-07)

Systematic split of overgrown bringup source files into per-subsystem modules aligned with the planned Core 1 HAL architecture. Umbrella header pattern preserves backward compatibility — no `.c` file `#include` changes required. Build verified; firmware flashed and running.

#### Changes (5 phases)

| Phase | Original File (lines) | Split Into | Retained (lines) |
|-------|----------------------|------------|-------------------|
| 1 — Headers | `bringup.h` (340) | `bringup_pins.h` (100), `bringup_lm27965.h` (25), `bringup_bq25622.h` (68), `bringup_bq27441.h` (54), `bringup_sx1262.h` (32), `bringup_power.h` (17) | `bringup.h` (127, umbrella) |
| 2 — Power | `bringup_power.c` (1076) | `bringup_led.c` (158), `bringup_charger.c` (330), `bringup_gauge.c` (440) | `bringup_power.c` (168) |
| 3 — Memory | `bringup_sram.c` (1721) | `bringup_psram.c` (804), `bringup_memory_tft.c` (733), `bringup_flash.c` expanded (145) | `bringup_sram.c` (86) |
| 4 — Audio | `bringup_audio.c` (765) | `bringup_amp.c` (316), `bringup_mic.c` (467), `bringup_amp.h` (6) | deleted |
| 5 — Menu/TFT | `bringup_menu.c` (778) | `bringup_tft_draw.c` (385) | `bringup_menu.c` (414) |

**Summary:** 7 new headers + 8 new `.c` files. Longest file reduced from 1721 → 804 lines. Core source total ~8580 lines across 21 `.c` files and 10 headers. Zero behaviour change; binary output identical modulo linker order.

**Firmware notes:**
- Step 20 firmware file references updated: `bringup_audio.c` → `bringup_amp.c` + `bringup_mic.c`; `bringup_power.c` functions split into `bringup_led.c`, `bringup_charger.c`, `bringup_gauge.c`; `bringup_sram.c` memory/TFT functions split into `bringup_psram.c`, `bringup_memory_tft.c`, `bringup_flash.c`; `bringup_menu.c` TFT drawing split into `bringup_tft_draw.c`
- `CMakeLists.txt` updated for all 3 build targets (`i2c_custom_scan`, `gnss_tft_standalone`, `core1_bringup_test`)
- Cross-file dependencies resolved: `amp_pio_start/stop`, `bee_note` (amp→mic); `bq25622_reg_read/write`, `fg_read16/ctrl_write/ctrl_read`, `lm_read` (power subsystem); `psram_set_timing`, `psram_sweep_pass`, `flash_speed_run` (memory→TFT); `menu_tft_active()` getter (tft_draw→menu)

---

### Step 23 — LoRa Standalone TX/RX Verification

**Result: ✅ PASS** (2026-04-07)

Standalone bringup LoRa tests aligned with Meshtastic/RadioLib SX1262 configuration.
Six firmware bugs fixed (BW code, SyncWord encoding, DC-DC regulator, PA clamping errata,
sensitivity errata, init ordering). New `lora_tx` function sends Meshtastic-format packets
(AES-128-CTR encrypted protobuf). All LoRa tests now display results on TFT.

#### Bugs Fixed

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| BW code wrong | `0x08` = 10.42 kHz (not 250 kHz); correct SX1262 code for 250 kHz is `0x05` | Changed all lora_rx/tx/dump callers and hardcoded values |
| SyncWord encoding | Used controlBits=0x04 → reg 0x24,0xB4; RadioLib uses controlBits=0x00 → 0x20,0xB0 | Fixed WriteRegister values for reg 0x0740/0x0741 |
| Missing SetRegulatorMode(DC-DC) | Meshtastic uses DC-DC (opcode 0x96, payload 0x01); bringup used default LDO | Added to lora_rx, lora_tx, lora_dump init sequences |
| Missing fixPaClamping | SX1262 errata §15.2: PA clamping too aggressive → TX never completes (TxDone IRQ never fires) | Read-modify-write reg 0x08D8, OR 0x1E |
| Missing sensitivity fix | SX1262 errata §15.1: reg 0x0889 bit 2 must be set for BW < 500 kHz | Applied in lora_rx and lora_tx |
| Init sequence order | SetStandby(RC) instead of XOSC; no standby after calibrate; wrong command ordering | Restructured to RadioLib-exact order: XOSC standby → DIO2 → TCXO → calibrate → XOSC standby → DC-DC → packet type → frequency → calibrate image |

#### Test Results

| Test | Config | Result | Notes |
|------|--------|--------|-------|
| lora_tx (MEDIUM_FAST) | SF9, BW250k, 920.125 MHz, ch=0x6B | ✅ PASS | TxDone 132 ms; node `!4d4f4b59` ("MOKY") visible on Meshtastic mesh |
| lora_tx (LONG_FAST) | SF11, BW250k, 920.125 MHz | ✅ PASS | TxDone 484 ms (matches theoretical ~477 ms airtime) |
| lora_rx (MEDIUM_FAST) | SF9, BW250k, 920.125 MHz, continuous | ✅ PASS | 25 packets received from node `538EEBE7`, 0 CRC errors |
| lora_rx (LONG_FAST) | SF11, BW250k, 920.125 MHz, 30 s | ⚠️ N/A | No LONG_FAST traffic on air during test window; RSSI/preamble detection confirmed working |
| OCP register | 0x08E7 readback after SetPaConfig+SetTxParams | ✅ PASS | 0x38 = 140 mA (formula: value × 2.5 mA) |

#### New Features

| Feature | Description |
|---------|-------------|
| `lora_tx` function | Meshtastic-format TX: 16-byte PacketHeader + AES-128-CTR encrypted protobuf payload ("MokyaLora" text message, broadcast to 0xFFFFFFFF) |
| `lora_tx` serial command | Added to REPL; `bringup_run.ps1` timeout 15 s |
| TFT display for all LoRa tests | `lora_test`, `lora_rx`, `lora_dump`, `lora_tx` show results on TFT (title, params, status, PASS/FAIL) |
| Menu wrappers | `cmd_lora_test`, `cmd_lora_tx`, `cmd_lora_dump` add BACK key wait; raw functions return immediately (no serial REPL blocking) |
| AES-128-CTR implementation | Standalone AES-128 encrypt (sbox, key expansion, CTR mode) for Meshtastic packet encryption |

**Firmware files:**
- `firmware/tools/bringup/bringup_lora.c` — All 6 bug fixes, `lora_tx()` function, AES-128-CTR, TFT output for all functions, RadioLib-exact init sequences
- `firmware/tools/bringup/bringup_sx1262.h` — Added `lora_tx()` declaration
- `firmware/tools/bringup/bringup_menu.c` — Added `cmd_lora_test`, `cmd_lora_tx`, `cmd_lora_dump` wrappers; BW code fix in all `lora_rx()` calls
- `firmware/tools/bringup/i2c_custom_scan.c` — Added `lora_tx` command dispatch; BW code fix in all `lora_rx()` calls
- `bringup_run.ps1` — Added `'lora_tx' = 15` to CmdTimeout table

---

### Step 24 — Core 1 TFT Output & Menu Wrap-Around

**Result: ✅ PASS** (2026-04-07)

UI polish for Core 1 IPC test and menu navigation. Core 1 test now displays results on TFT with colour-coded PASS/FAIL and waits for BACK key. Menu cursor wraps around in all directory levels.

#### Changes

| Change | Description |
|--------|-------------|
| Core 1 TFT output | `core1_test()` displays title + 4 test lines (Boot, FIFO, SRAM, GPIO) + overall result on TFT using standard colour scheme (green=PASS, red=FAIL) |
| Core 1 BACK wait | Test result screen stays visible until user presses BACK key, then returns to menu |
| Menu wrap-around | UP at first item → jumps to last item; DOWN at last item → jumps to first item; scroll window adjusts correctly; applies to all menu levels |

#### Test Results

| Test | Result | Notes |
|------|--------|-------|
| `core1` serial command | ✅ PASS | 4/4 (Boot, FIFO, SRAM, GPIO); TFT output confirmed |
| Menu wrap-around | ✅ PASS | Verified on root menu and sub-pages |

**Firmware files:**
- `firmware/tools/bringup/bringup_core1.c` — Added `bringup_menu.h` include, TFT layout macros (LS/LCH/LCOLS), TFT output for each test step + overall result, BACK key wait after completion
- `firmware/tools/bringup/bringup_menu.c` — `menu_handle_key()` KEY_UP/KEY_DOWN wrap-around with scroll window adjustment

### Step 25 — DMA-to-PSRAM Error Root Cause Investigation

**Result: ⚠️ CONDITIONAL** (2026-04-07)

Deep investigation of Issue 14 (DMA reads from PSRAM produce word errors). Five diagnostic sub-tests (`psram_dma_diag` serial command) identified the precise failure mechanism.

#### Diagnostic Results

| Test | Description | Result | Finding |
|------|-------------|--------|---------|
| A | DMA write 8 words + CPU verify | ✅ 0 errors | DMA writes work |
| B | DMA write 4 KB + CPU verify | ✅ 0 errors | DMA writes work at scale |
| C | CPU write + DMA read 4 KB | ❌ 25% errors | **DMA reads corrupt data** |
| D | Address-pattern DMA read 256 B | ❌ 15/64 errors | Error pattern identified |
| E | Single-word DMA reads (1 at a time) | ✅ 0 errors | **Burst is the trigger** |
| F | Burst-size sweep (2–256 words) | ❌ errors start at burst=8 | **Threshold = 8 words = 1 XIP cache line** |

#### Burst-Size Error Rate

| Burst (words) | Errors / 256 | Rate |
|---------------|-------------|------|
| 1 | 0 | 0% |
| 2 | 0 | 0% |
| 4 | 0 | **0% ← max safe burst** |
| 8 | 32 | 12.5% |
| 16 | 53 | 20.7% |
| 32 | 56 | 21.9% |
| 64 | 60 | 23.4% |
| 256 | 63 | 24.6% |

#### Error Mechanism (confirmed via address-pattern test)

The lowest byte of corrupted words has its **high nibble replaced by its low nibble**: `XY` → `YY`.

```
expected 0x00000002 → got 0x00000022   (0x02 → 0x22)
expected 0x00000006 → got 0x00000066   (0x06 → 0x66)
expected 0x0000000A → got 0x000000AA   (0x0A → 0xAA)
expected 0x12345678 → got 0x12345688   (0x78 → 0x88)
```

In QPI mode each byte = 2 nibbles on consecutive QPI clocks. The 7th QPI data nibble (byte 0 high) latches the value of the 8th nibble (byte 0 low) — **QMI data capture is 1 QPI clock late** on specific words during burst reads.

#### Observed Behaviour

Scope of the symptom — described only from measurement; root cause is not yet isolated:

1. Even with XIP cache disabled (EN_SEC=0, EN_NONSEC=0), the XIP controller appears to fetch M1 data in 8-word (32-byte, cache-line-sized) bursts from QMI.
2. When the DMA bus master triggers these burst reads, corruption appears on specific word positions within the burst (predominantly positions 2 and 6 in each 8-word group at 37.5 MHz).
3. CPU reads are unaffected.
4. DMA writes to PSRAM are completely unaffected (0 errors).
5. At 37.5 MHz, DMA reads with burst ≤ 4 words are unaffected (0 errors).

#### Register State During Test

```
XIP_CTRL      = 0x00000800  (EN_SEC=0, EN_NONSEC=0, WRITABLE_M1=1)
M1.timing     = 0xA0007002  (CLKDIV=2, RXDELAY=0, SCK=37.5 MHz)
```

#### Workarounds

| Approach | Throughput | Errors | Notes |
|----------|-----------|--------|-------|
| CPU volatile read (current) | 1632 KB/s | 0 | Ties up CPU |
| DMA burst ≤ 4 words (chained) | TBD | 0 | DMA chaining overhead |
| DMA write + CPU read | Write: DMA, Read: CPU | 0 | Best for framebuffer use case |

> **Note:** DMA burst=4 produces 0 errors in isolated tests (256 words, `dma_channel_configure` per chunk) but **25% errors under rapid back-to-back transfers** (pre-computed config, tight loop). The inter-transfer gap from `dma_channel_configure()` overhead masked the issue. Only DMA burst ≤ 2 is safe in all conditions.

#### Framerate Benchmark (150 KB = 240×320 RGB565)

Full-frame PSRAM read throughput measured via `psram_fps` serial command:

| Method | ms/frame | FPS | Errors | Notes |
|--------|---------|-----|--------|-------|
| CPU volatile read | 78 ms | 12.8 | 0 | |
| DMA burst=4 (pre-computed) | 47 ms | 21.1 | 25% ❌ | Errors under rapid-fire — not usable |
| DMA burst=2 (pre-computed) | 80 ms | 12.4 | 0 | No advantage over CPU |
| DMA full burst (reference) | 27 ms | 36.6 | 25% ❌ | Theoretical max with errors |

SRAM framebuffer comparison (via `tft_fast` sub-tests J/K, DMA → PIO → LCD):

| Method | ms/frame | FPS | Notes |
|--------|---------|-----|-------|
| SRAM framebuffer DMA, clkdiv=4 | 16.6 ms | **60.2** | Identical to solid fill |
| SRAM framebuffer DMA, clkdiv=3 | 12.5 ms | **80.0** | Identical to solid fill |

#### Architecture Decision

**PSRAM is not needed for display.** SRAM single framebuffer achieves 60–80 FPS (PIO-limited), vs PSRAM read at 12.8 FPS (QMI-limited). Revised memory plan:

| Memory | Contents |
|--------|----------|
| SRAM (~10 KB) | LVGL partial render buffer (10–20 lines) → DMA → PIO → LCD |
| PSRAM (4 MB) | MIE dictionary DAT + values (CPU random read, binary search) |
| PSRAM (4 MB) | Application heap (message history, node cache) |

MIE dictionary lookup is inherently random-access (~600 bytes/query, <0.4 ms via CPU read). DMA provides no benefit for this access pattern. The DMA-to-PSRAM read issue has **zero impact** on the production firmware architecture.

**Bug fix (2026-04-07):** `psram_rd_diag()` XIP_CTRL decode used bit 10 for WRITABLE_M1; correct position is bit 11 (SDK `XIP_CTRL_WRITABLE_M1_MSB=11`, mask `0x00000800`). Display showed `WRITABLE_M1=0` when the actual value was 1. Fixed — no functional impact (decode-only bug, read path unaffected).

**Regression test (2026-04-07):** All memory commands re-run after Step 25 changes — sram, flash, psram, psram_full, mem_diag, psram_rd_diag, psram_dma_diag: results identical to initial Step 25 findings. No regression.

**Firmware files:**
- `firmware/tools/bringup/bringup_memory_tft.c` — Added `psram_dma_diag()` (6 sub-tests A–F), `psram_fps_bench()` (CPU vs DMA framerate), `shared_sram_buf[]` (150 KB shared buffer); fixed WRITABLE_M1 decode (bit 10 → bit 11)
- `firmware/tools/bringup/bringup_tft.c` — Added sub-tests J (SRAM framebuffer DMA clkdiv=4) and K (clkdiv=3) to `tft_fast_test()`
- `firmware/tools/bringup/i2c_custom_scan.c` — Registered `psram_dma_diag` and `psram_fps` serial commands
- `bringup_run.ps1` — Added timeout entries for `psram_dma_diag` (30 s), `psram_rd_diag` (15 s), `psram_fps` (120 s)

---

### Step 26 — PSRAM 75 MHz Validation on Board #2

**Result: ✅ PASS** (2026-04-11)

Second Rev A PCB assembled; PSRAM QPI max clock re-characterised. Board #2's APS6404L passes every test at CLKDIV=1 (75 MHz) — a doubling of the Board #1 validated default (CLKDIV=2, 37.5 MHz). The difference is attributed to per-unit APS6404L / PCB routing timing margin; no single root cause is claimed.

#### PSRAM Speed Sweep (256 KB per combo, write+read / read-only)

| CLKDIV | SCK | RXDELAY=0 | RXDELAY=1 | RXDELAY=2..7 |
|--------|-----|-----------|-----------|--------------|
| 3 | 25 MHz | FAIL | FAIL | FAIL |
| 2 | 37.5 MHz | **PASS** | FAIL | FAIL |
| **1** | **75 MHz** | ✅ **PASS** | ✅ **PASS** | FAIL |

#### Stability Diagnostic (`psram_diag`)

| Phase | Setting | Runs | Errors |
|-------|---------|------|--------|
| [1] | CLKDIV=1/RXDELAY=0 (write@safe, read@75 MHz) | 10 | 0 |
| [2] | CLKDIV=1/RXDELAY=1 (write@safe, read@75 MHz) | 10 | 0 |
| [3] | CLKDIV=2/RXDELAY=0 (37.5 MHz baseline) | 5 | 0 |
| [4] | CLKDIV=1/RXDELAY=0 (write+read both @75 MHz) | 5 | 0 |

#### Full 8 MB Test at 75 MHz (`psram_full_75`)

```
M1.timing = 0xA0007001 (CLKDIV=1, RXDELAY=0, SCK=75 MHz)
Capacity  : 8 MB (2097152 words)
Write     : 8/8 MB complete
Verify    : 8/8 MB, errors=0
Result    : PASS — full 8 MB accessible and data-correct
```

Also re-run after `psram_init()` default was promoted to CLKDIV=1 — regular `psram_full` passes with 0 errors at the new default.

#### Issue 14 Re-verification at 75 MHz (`psram_dma_diag`)

The DMA read corruption observed at 37.5 MHz is still present at 75 MHz and the safe burst threshold halves:

| Test | Description | 37.5 MHz (Board #1) | **75 MHz (Board #2)** |
|------|-------------|---------------------|-----------------------|
| A | DMA write 8 words + CPU verify | ✅ 0 errors | ✅ 0 errors |
| B | DMA write 4 KB + CPU verify | ✅ 0 errors | ✅ 0 errors |
| C | CPU write + DMA read 4 KB | ❌ 25% | ❌ 24.7% |
| D | Address-pattern DMA read 256 B | ❌ 15/64 | ❌ 16/64 |
| E | Single-word DMA reads | ✅ 0 errors | ✅ 0 errors |
| F | Burst-size sweep threshold | errors start @ burst=8 | errors start @ burst=4 |

Burst-size error rate comparison:

| Burst (words) | 37.5 MHz | 75 MHz |
|---------------|----------|--------|
| 1 | 0% | 0% |
| 2 | 0% | **0% ← safe** |
| 4 | 0% ← previously safe | **25%** ❌ |
| 8 | 12.5% | 25% |
| ≥ 16 | 20–24% | 25% |

Error signature unchanged — `XY`→`YY` byte-0 nibble duplication. Mod-8 histogram at 75 MHz shows positions 0/2/4/6 all affected (pos 0 and 4 newly degraded), while 1/3/5/7 remain mostly clean.

#### Architecture Impact

- DMA **writes** to PSRAM: safe at 75 MHz
- CPU reads from PSRAM: safe at 75 MHz
- DMA **reads** from PSRAM: safe burst tightened from ≤ 4 words to ≤ 2 words
- Production memory plan unchanged: SRAM partial-render framebuffer (60–80 FPS, PIO-limited) + CPU random-access PSRAM for MIE dictionary and application heap. Neither use case triggers Issue 14.

#### Firmware Changes (commit `ccd6441`)

- `firmware/tools/bringup/bringup_psram.c` — `psram_init()` default CLKDIV 2 → 1 (75 MHz); added `psram_full_test_75()` helper that switches to CLKDIV=1 around `psram_full_test()` and restores original M1.timing.
- `firmware/tools/bringup/i2c_custom_scan.c` — registered `psram_full_75` serial command.
- `firmware/tools/bringup/bringup.h` — declared `psram_full_test_75()`.

> **Note:** Only one Board #2 sample has been characterised. The 75 MHz default may not hold on Board #1 or on future production units. If future samples fail, per-board detection or a reversion to CLKDIV=2 will be required.

---

## Issues Log

| # | Date | Component | Issue | Root Cause | Fix Applied | Rev B Action |
|---|------|-----------|-------|-----------|-------------|--------------|
| 1 | 2026-04-02 | L1 (Murata DLM0NSN900HY2D) | USB common mode choke wrong orientation → USB non-functional | Factory assembly error | Removed L1; bridged D+/D− with jumper wire | Flag orientation in Assembly PDF |
| 2 | 2026-04-02 | U14 (BQ25622RYKR) | nCE pull-up → charging disabled by default | Design error: nCE must be pulled to GND | Shorted TP101 to GND | Connect nCE to GND or MCU GPIO in Rev B |
| 3 | 2026-04-02 | U17 (LIS2MDL, 0x1E) | SCL/SDA reversed → device unreachable | Schematic routing error | Removed R54/R55; jumper wires swap SCL/SDA | Fix I2C routing in Rev B |
| 4 | 2026-04-02 | U18 (LPS22HH) | SCL/SDA reversed; address 0x5D not 0x5C | Routing error; SA0=3.3V contradicts docs | Removed R59/R60; jumper wires | Fix routing; correct SA0 net; update docs |
| 5 | 2026-04-02 | LCD FPC (Molex 54132-4062) | FPC pinout incompatible with NHD-2.4-240320AF-CSXP | Pinout mismatch | FPC adapter board for Rev A | Re-verify FPC pinout; correct in Rev B |
| 6 | 2026-04-02 | U5 (LM27965) Bank B | LED D37 and keypad backlight share Bank B → not independently controllable | Routing error | Accept Rev A limitation | Move D37 to unused Bank A channel in Rev B |
| 7 | 2026-04-02 | U15 (TPS62840) / 1.8 V rail | Pi Debug Probe incompatible (requires 3.3 V VTREF) | 1.8 V logic vs 3.3 V-only debug tool | Using J-Link (supports 1.8 V VTREF) | Evaluate switchable VTREF option |
| 8 | 2026-04-03 | U3 (APS6404L PSRAM) | GPIO0 (CS1) stuck LOW; XIP reads return 0xFFFFFFFF | Three firmware bugs: (1) SIO register addresses used RP2040 offsets; (2) QMI `ASSERT_CS1N` does not control GPIO0; (3) PSRAM XIP address used `+0x800000` (M0) instead of `+0x1000000` (M1) | All three fixed. `psram_init()` runs at boot: SIO CS + QMI clock for init, then XIP_CS1 handoff. Direct-mode probe PASS, XIP 4 KB pattern PASS | Rev B: add 4.7k–10kΩ pull-up on GPIO0 to VCC_1V8 |
| 9 | 2026-04-04 | U16 (BQ27441DRZR, 0x55) | BIN pin unconnected → BIE=1 → BAT_DET never set → INITCOMP stuck at 0 | BIN pin not connected in Rev A; BIE default=1 (hardware detection mode) | CONFIG UPDATE clears BIE (OpConfig 0x25F8→0x05F8); BAT_INSERT (0x000C) sets BAT_DET=1; INITCOMP completes in ~1.6 s. Bringup command `bq27441` handles this automatically | Connect BIN pin or remove fuel gauge from BOM (see Issue 10) |
| 10 | 2026-04-04 | U16 (BQ27441DRZR, 0x55) | Cold boot I2C NACK — gauge completely unresponsive after any power-on that includes battery insertion. 9-clock bus recovery ineffective. Observations: (1) USB on + no battery + charger toggle → gauge responds (charger supplies VSYS → gauge POR with clean bus); (2) while gauge already running, insert battery → still responds; (3) cold boot with battery (USB+battery both removed then reinserted) → permanent NACK; (4) USB on + charge off + insert battery → permanent NACK, even after charger re-toggle | Under investigation. Suspected ESD latchup on I2C input pins: battery insertion causes fast VBAT edge → internal 1.8V LDO ramps from 0V while external SDA/SCL pull-ups already at 1.8V → ESD protection diodes forward-bias → I2C input circuitry latch. Not yet confirmed with scope measurement | Rev A workaround: boot without battery → charge_on → insert battery (gauge already running, no POR). Standard I2C bus recovery (9 clocks + STOP) does not resolve the condition | **Consider removing BQ27441 from Rev B BOM.** If retained: add 1MΩ pull-down on SDA/SCL (TI recommendation), ensure power sequencing (1.8V pull-up rail not before gauge VDD), connect GPOUT to MCU GPIO |
| 11 | 2026-04-05 | U10/U11 GNSS RF chain | 0 satellites tracked after >10 min outdoor cold start; GNSS completely non-functional | Unknown. LNA (BGA123N6) ON confirmed (VPON = 1.8 V). Candidates: (1) chip antenna ground clearance insufficient (open item in rf-matching.md); (2) SAW → LNA or LNA → Teseo impedance mismatch; (3) BOM part incorrect (design docs listed BGA725L6; actual part is BGA123N6) | Under investigation. Runtime `$PSTMNOTCHSTATUS` probe ruled out — LIV3FL `4.6.15.1` ignores high-word of `$PSTMNMEAREQUEST` (see Step 14a); firmware-side jammer detection unavailable without destructive CDB-1232 write. Next step: bypass BGA123N6 (jumper SAW output → Teseo RF_IN; pull VPON to GND) | Fix antenna ground clearance; verify SAW and LNA BOM vs schematic; correct rf-matching.md and hardware-requirements.md (BGA725L6 → BGA123N6) |
| 12 | 2026-04-06 | Meshtastic / SPI1 | SX1262 radio init fails (Error 4, NO_INTERFACE) under Meshtastic firmware | `rpipico2` board defaults `PIN_WIRE1_SDA=26, PIN_WIRE1_SCL=27`; Meshtastic `Wire1.begin()` reconfigures GPIO 26/27 funcsel from SPI to I2C before `SPI1.begin()` runs | `MESHTASTIC_EXCLUDE_I2C=1` disables all Wire init on Core 0 (I2C peripherals are Core 1's domain). Initial workaround was `I2C_SDA1=6, I2C_SCL1=7` redirect | No HW change needed; firmware-only fix. Document SPI1/Wire1 pin conflict in variant README |
| 13 | 2026-04-06 | PSRAM / USB CDC serial | `psram` and `psram_full` commands return empty output in `bringup_test_all.ps1` when run after 12+ prior commands; pass 100% in isolation or via `bringup_run.ps1` | USB CDC serial timing: after many sequential commands, trailing CDC packet bytes from previous responses pollute or delay the next command's buffer. PSRAM tests (which involve QSPI bus reconfiguration) may have longer response latency that exceeds the script's read window | Under investigation. Firmware confirmed working — issue is host-side script timing only. Workaround: skip memory group (`-Skip memory`) for 16/16 PASS, or run memory group separately (`-Group memory`) for 4/4 PASS | No HW change needed; script-side fix required |
| 14 | 2026-04-07 | U3 (APS6404L PSRAM) / DMA | DMA **reads** from PSRAM produce ~25% word errors. DMA writes and CPU access are 100% reliable. Corrupted words have byte 0 high nibble replaced by low nibble (`XY`→`YY`). Safe burst threshold is speed-dependent: ≤ 4 words at 37.5 MHz, ≤ 2 words at 75 MHz (board #2, verified 2026-04-11) | Not yet isolated. Observed pattern suggests QMI read-data latch timing error on specific word positions within each 8-word XIP cache-line burst triggered by the DMA bus master. CPU reads use a different internal path and are unaffected. Needs further investigation before assigning a definitive cause | Workaround: CPU volatile read or DMA burst ≤ 2 words. DMA writes to PSRAM are safe. Diagnostic command: `psram_dma_diag` (see Step 25) | No HW change needed. For display framebuffer: DMA write + CPU read. Consider Raspberry Pi forum inquiry or errata check for authoritative explanation |

---

## Measurements

See `measurements/` directory for oscilloscope captures, spectrum plots, and current profiles.
