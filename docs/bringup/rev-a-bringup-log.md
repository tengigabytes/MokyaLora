# Rev A Bring-Up Log

**Board revision:** Rev A
**Submitted to manufacturer:** 2026-03-22
**Board received:** 2026-04-02
**Bring-up started:** 2026-04-02

---

## Pre-Power Checklist

- [x] Visual inspection: no solder bridges, correct component orientation.
- [x] Continuity check: VSYS, 1.8 V, 3.3 V rails to GND (expect open before power).
- [x] Resistance to GND: VSYS rail > 10 Ω, no short.
- [x] USB-C CC1 / CC2 pull-down resistors (5.1 kΩ) present — USB-C correctly negotiates 5 V.

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
| 0x55    | BQ27441        | I2C1 | ⚠️ CONDITIONAL | Readable only with battery installed or in charging mode |
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

**Teseo-LIV3FL GNSS (0x3A)** — ✅ PASS (streaming confirmed)

I2C read returns live NMEA sentences. `$PSTMGETSWVER` proprietary command ACKed. 300-byte stream sample:

| NMEA sentence | Decode |
|---------------|--------|
| `$GNGSA,A,1,…,PDOP=99.0` | Fix=1 (no fix); all satellite slots empty; PDOP/HDOP undefined |
| `$GPGLL,…,V` | Position void (no fix) |
| `$PSTMCPU,15.21,-1,98` | ST proprietary: chip temp 15.21 °C, freq ~98 MHz |
| `$GPRMC,033936,V` | Status void; UTC 03:39:36; date from RTC default (not synced) |
| `$GPGGA,…,0 sats,Fix=0` | 0 satellites tracked; no position |

No GPS fix expected indoors. Teseo-LIV3FL confirmed operational. Device has no traditional register map; all status via NMEA proprietary sentences (`$PSTM…`).

### Step 4 — Display

**Result: ❌ FAIL**

LCD FPC connector pin order is incompatible with NHD-2.4-240320AF-CSXP — display cannot be driven.
No temporary workaround. See Issue 5. Pending Rev B correction.

### Step 5 — LoRa

**Result: ✅ PASS** (SPI, calibration, RX mode, and RSSI verified; TX link test pending)

| Item | Result | Notes |
|------|--------|-------|
| SX1262 SPI response | ✅ PASS | GetStatus returns valid status byte |
| ChipMode after reset | ✅ PASS | STBY_RC (mode=2) as expected per datasheet |
| RegSyncWord (0x0740/41) | ✅ PASS | 0x14 0x24 — POR default (private network) |
| Calibrate + CalibrateImage | ✅ PASS | GetDeviceErrors = 0x0020 (XOSC=1 only — see note) |
| RxGain write (0x08AC) | ✅ PASS | Readback 0x96 (boosted LNA) |
| SetRx → ChipMode=5 | ✅ PASS | RX mode entered successfully |
| RSSI (923.125 MHz, 5 samples) | ✅ PASS | −114.0 dBm consistent (indoor noise floor) |
| GetStats (8-byte frame) | ✅ PASS | RxOk=0 CrcErr=0 HdrErr=0 (no packets; no crash) |
| OCP (0x08E7) | ⚠️ NOTE | 0x18 → 105 mA (non-default; SX1262 default is 0x38 = 185 mA) |

**GetStatus raw:** `0x2A` — ChipMode=2 (STBY_RC), CmdStatus=5 (no command pending — normal at POR).

**lora_dump static registers:**

| Register | Value | Decode |
|----------|-------|--------|
| RegSyncWord 0x0740/41 | 0x14 0x24 | Private network default ✅ |
| OCP 0x08E7 | 0x18 | 105 mA — non-default; investigate before TX |
| XTATrim 0x0911 | 0x12 | Default |
| RxGain 0x08AC | 0x96 | Boosted LNA — write/readback OK ✅ |

**SPI configuration:** Mode 0 (CPOL=0 CPHA=0), MSB first, 1 MHz, manual CS on GPIO 25. BUSY (GPIO 28) asserted low after reset within 200 ms timeout.

**Firmware notes:**
- GPIO: nRST=23, MISO=24, nCS=25, SCK=26, MOSI=27, BUSY=28, DIO1=29 (matches schematic)
- SPI1 peripheral used; CS controlled manually (not via SPI1_CSn hardware)
- Reset sequence: nRST low 10 ms → high 10 ms → wait BUSY low → GetStatus
- CmdStatus=5 at POR is expected (datasheet §13.1: "Failure to execute" maps to no prior command)
- `SetDIO3AsTCXOCtrl` (0x97, voltage=0x02 1.8V, timeout=500 µs) must be called even though TCXO is always-on (ECS-TXO-20CSMV4 powered from 1.8V rail). Without it PLL calibration fails.
- `GetDeviceErrors = 0x0020` (XOSC_START bit only): cosmetic false positive. The SX1262 startup-detection circuit flags XOSC even when the always-on TCXO is running. Chip is fully functional: enters RX (ChipMode=5), RSSI valid. No action required.
- `GetStats` frame is 8 bytes: opcode(1) + status-NOP(1) + RxOk(2) + CrcErr(2) + HdrErr(2). A prior firmware bug used 7 bytes causing out-of-bounds access on `rx[7]`; corrected.
- TX link test (actual LoRa packet exchange) pending.

### Step 6 — Keypad

**Result: ✅ PASS**

| Item | Result | Notes |
|------|--------|-------|
| Matrix scan direction | ✅ PASS | ROW = output (drive LOW), COL = input pull-up; diode Anode=COL / Cathode=ROW enforces this direction |
| All 36 keys detected | ✅ PASS | SW1–SW36 confirmed via key_monitor |
| 1 kΩ series resistors (R90–R101) | ✅ PASS | No impact at 1.8 V: Vf ~0.3 V < VIL 0.54 V with pull-up idle at 1.8 V |

**Firmware (r, c) matrix — confirmed by physical testing:**

SW numbering in the schematic is column-major (SW1–6 = GPIO ROW0 column, top-to-bottom). The firmware `key_names[r][c]` maps as follows:

| r \ c | C0 | C1 | C2 | C3 | C4 | C5 |
|-------|----|----|----|----|----|----|
| R0 | FUNC | BACK | LEFT | DEL | VOL- | UP |
| R1 | 1/2 | 3/4 | 5/6 | 7/8 | 9/0 | OK |
| R2 | Q/W | E/R | T/Y | U/I | O/P | DOWN |
| R3 | A/S | D/F | G/H | J/K | L | RIGHT |
| R4 | Z/X | C/V | B/N | M | ㄡㄥ | SET |
| R5 | MODE | TAB | SPACE | SYM | 。.？ | VOL+ |

> Note: this (r, c) order does not match the logical Row/Col in hardware-requirements.md — it reflects the physical GPIO-to-switch wiring order in the schematic.

**Firmware notes:**
- Bringup scan: GPIO polling, ROW-major (ROW driven LOW one at a time, COL read). Not the production PIO+DMA scan.
- GPIO 36–47 are above the 32-bit GPIO bank boundary; verified functional on RP2350B.
- Initial firmware had scanning direction inverted (COL drive, ROW read = reverse bias = no detection). Corrected to ROW drive / COL read.

### Step 7 — Audio

**Result: ✅ PASS**

| Item | Result | Notes |
|------|--------|-------|
| NAU8315 I2S format | ✅ PASS | FSL pin floating → Standard I2S mode confirmed |
| NAU8315 gain | ✅ PASS | GAIN pin floating → Mode 5 = 6 dB (valid, defined state per datasheet) |
| PIO I2S driver | ✅ PASS | 48 828 Hz sample rate (clkdiv=40.0); 16-bit stereo |
| Speaker output | ✅ PASS | Little Bee melody plays correctly at 40% amplitude |

**Firmware notes:**
- Standard I2S timing: LRCK transitions during last BCLK high of previous channel; MSB appears at first BCLK low of next channel. PIO program verified against NAU8315 datasheet timing diagram.
- RP2350B PIO: GPIO 32 (AMP_DAC_PIN) is above the 32-GPIO boundary. `pio_set_gpio_base(pio0, 16)` **must** be called before `pio_add_program_at_offset()`; calling it after causes `PICO_ERROR_INVALID_STATE` and the state machine silently fails to start (FIFO hangs full).
- Click/pop suppression: transitions between notes send zero samples via `pio_sm_put_blocking` rather than using `sleep_ms`, which would drain the FIFO abruptly.

### Step 8 — Battery & Motor

| Item | Result | Notes |
|------|--------|-------|
| Vibration motor (HD-EMB1104-SM-2) | ✅ PASS | PWM drive via SSM3K56ACT OK |
| BQ27441 fuel gauge I2C | ✅ PASS | Readable after TP101 fix; requires battery installed |
| BQ25622 charging | ✅ PASS | After TP101 fix (Issue 2) |

**BQ25622 register dump (at power-on, no battery, charging disabled by firmware):**

| Register | Value | Field decode |
|----------|-------|--------------|
| PART_INFO (0x38) | 0x0A | PN=1 (BQ25622 ✅), DEV_REV=2 |
| CHG_CTRL0 (0x14) | 0x06 | ICHG = 240 mA (6 × 40 mA) — POR default |
| VREG (0x06) | 0x00 | Charge voltage = 3504 mV (base, code=0) — POR default; **must be set to 4200 mV for BL-4C** |
| IINDPM (0x00) | 0xFF | Input current limit = 6300 mA — POR default, not yet configured by firmware |
| VINDPM (0x01) | 0xFF | Input UVLO = 16600 mV — POR default, not yet configured by firmware |
| CTRL1 (0x16) | 0x80 | EN_CHG=0 (charging disabled at startup), EN_HIZ=0, WD=0 |
| CTRL2 (0x17) | 0x4F | — |
| CTRL3 (0x18) | 0x04 | BATFET normal, BATFET_DLY=1 |
| CTRL4 (0x19) | 0xC4 | — |
| NTC_CTRL0 (0x1A) | 0x3D | TS_IGNORE=0 (NTC active) |
| STATUS0 (0x1D) | 0x10 | **VSYS_STAT=1** — battery below VSYSMIN; system powered from VBUS only (no battery connected) |
| STATUS1 (0x1E) | 0x04 | CHG=Not charging, **VBUS=Adj. HV DCP** — USB-C 5 V input detected; normal |
| FAULT (0x1F) | 0x00 | No faults ✅ |

> **Firmware note:** ICHG (240 mA) and VREG (3504 mV) are POR defaults. The production charging driver must initialise VREG to 4200 mV (Nokia BL-4C max), set IINDPM/VINDPM to appropriate limits, and enable charging via CTRL1[EN_CHG]. VSYS_STAT=1 confirms no battery was installed during this test.

**LM27965 register dump (at power-on, all LEDs off):**

| Register | Value | Field decode |
|----------|-------|--------------|
| GP (0x10) | 0x20 | ENA=0 ENB=0 ENC=0 EN5A=0 EN3B=0 — all banks disabled ✅ (bit5 always 1 per datasheet) |
| BANK_A (0xA0) | 0xE0 | brightness code = 0 (TFT backlight, off) |
| BANK_B (0xB0) | 0xE0 | brightness code = 0 (keyboard BL + D3B green, off) |
| BANK_C (0xC0) | 0xFC | brightness code = 0 (D1C red indicator, off) |

LM27965 idle state correct — all outputs disabled at firmware startup.

### Step 9 — Memory (Flash & PSRAM)

| Item | Result | Notes |
|------|--------|-------|
| W25Q128JW Flash JEDEC ID | ✅ PASS | `EF 60 18` — Winbond W25Q128JW confirmed |
| Flash SR1/SR2/SR3 | ✅ PASS | SR2 QE bit set (QSPI enabled); boot config intact |
| Flash XIP read | ✅ PASS | First 32 bytes match programmed firmware image |
| APS6404L PSRAM QPI probe | ❌ FAIL | Returns 0xFFFFFFFF — no response in QPI mode |
| APS6404L PSRAM SPI probe | ❌ FAIL | Returns 0xFFFFFFFF — no response in SPI mode |

**Flash diagnostics:**

JEDEC: `EF 60 18` (Winbond W25Q128JW, 128 Mbit). SR1=0x00, SR2=0x02 (QE=1), SR3=0x60. XIP dump matches programmed firmware — flash interface healthy.

**PSRAM diagnostics:**

Firmware-side configuration confirmed correct across multiple test runs:

| Diagnostic item | Value | Conclusion |
|----------------|-------|-----------|
| GPIO0_CTRL FUNCSEL | 9 (`XIP_CS1`) | GPIO assignment correct ✅ |
| QMI M1 rfmt (1st run, post-flash) | 0x000492A8 | Bootrom pre-configured M1 for QPI (rcmd=0xEB) |
| QMI M1 rfmt (2nd run, warm) | 0x00001000 | Previous `psram_test()` call reset M1 to SPI single-lane |
| QPI probe (`rcmd=0xEB`) | 0xFFFFFFFF | No response |
| SPI probe (`rcmd=0x03`, single-lane) | 0xFFFFFFFF | No response |

XIP occupying QMI CS0 does **not** block CS1 — the two chip-select channels are fully independent. Confirmed: the issue is not software or XIP contention.

Conclusion: firmware and QMI configuration are correct. PSRAM unresponsive to both QPI and SPI → **hardware connectivity issue**. See Issue 8. Physical investigation required: DMM continuity on GPIO0 (RP2350B pin 77) → U3 CE# (pin 1); 3.3V at U3 VDD; solder joint inspection under microscope.

---

## Issues Log

| # | Date | Component | Issue | Root Cause | Fix Applied | Rev B Action |
|---|------|-----------|-------|-----------|-------------|--------------|
| 1 | 2026-04-02 | L1 (Murata DLM0NSN900HY2D) | USB common mode choke assembled in wrong orientation → USB non-functional | Factory assembly error: orientation mark misread | Removed L1; bridged D+/D− lines with jumper wire → USB OK | Flag orientation in Assembly PDF; add assembly note |
| 2 | 2026-04-02 | U14 (BQ25622RYKR) | nCE pin has hardware pull-up → charging disabled by default | Design error: nCE (active-low) must be pulled to GND to enable charging | Shorted TP101 to GND → charging enabled; BQ27441 I2C readable | Connect nCE to GND or MCU GPIO (default low) in Rev B schematic |
| 3 | 2026-04-02 | U17 (LIS2MDL, I2C0 0x1E) | SCL and SDA lines connected in reverse → device unreachable | Schematic routing error | Removed R54, R55 (0 Ω series resistors); jumper wires to swap SCL/SDA → 0x1E readable | Fix I2C routing in Rev B schematic |
| 4 | 2026-04-02 | U18 (LPS22HH, I2C0) | SCL/SDA reversed; actual I2C address 0x5D (design stated 0x5C) | Routing error (same pattern as U17); SA0 connected to 3.3 V in schematic — contradicts documentation which stated SA0 = GND | Removed R59, R60 (0 Ω series resistors); jumper wires → 0x5D readable | Fix I2C routing; verify/correct SA0 net; update all docs to 0x5D |
| 5 | 2026-04-02 | LCD FPC (Molex 54132-4062) | Connector pin order incompatible with NHD-2.4-240320AF-CSXP → display non-functional | Pinout mismatch between selected connector and display datasheet | None — no practical temporary workaround | Re-verify FPC connector pinout against display datasheet; correct in Rev B |
| 6 | 2026-04-02 | U5 (LM27965) Bank B | Status indicator LED D37 and keypad backlight share Bank B → cannot be driven independently | Routing error: D37 should be on a separate Bank A channel | Accept Rev A limitation; D37 and keypad BL driven together | Move D37 to an unused Bank A channel in Rev B schematic |
| 7 | 2026-04-02 | U15 (TPS62840) / 1.8 V rail | Pi Debug Probe requires 3.3 V VTREF; MCU logic is 1.8 V → incompatible | 1.8 V design voltage vs 3.3 V-only debug tool | Using J-Link (supports 1.8 V VTREF) | Add VSET resistor option for switchable 3.3 V during development; requires re-evaluation of all 1.8 V net components and I2C pull-up resistor compatibility for both voltages |
| 8 | 2026-04-03 | U3 (APS6404L PSRAM) | PSRAM returns 0xFFFFFFFF on both QPI and SPI probe — no response | Suspected hardware connectivity issue: ~CS (GPIO0/pin 77) or VCC_1V8 to U3 may be open/unconnected. Firmware and QMI register configuration confirmed correct (FUNCSEL=9, bootrom QPI pre-config visible in M1 registers). | None — requires physical board inspection | Verify U3 ~CS trace continuity (GPIO0 → U3 pin 1); verify VCC_1V8 at U3; confirm PSRAM solder joints under microscope. If trace is open, bodge wire from RP2350B GPIO0 (pin 77) to U3 ~CS. Replace U3 if VCC shorted. |

### Step 10 — Microphone (IM69D130 PDM)

| Item | Result | Notes |
|------|--------|-------|
| `mic` — PDM bit capture & 1-density check | ✅ PASS | 1-density = 49.8 % (silent room ≈ 50 % expected) |
| `mic_loop` — mic → speaker loopback | ⏳ NOT YET TESTED | Firmware implemented; hardware validation pending |

**mic_test output (1024 words = 32 768 PDM bits):**

```
CLK = 3.125 MHz (clkdiv=20)  SELECT=GND (L-ch, rising edge)
1-density : 49.8 %  (silent room ≈ 50%)
Min word  : 0x0E959956
Max word  : 0xE8D4E4D5
Result: PASS
```

GPIO 4 (MIC_CLK) 和 GPIO 5 (MIC_DATA) 連線正常。PDM bit stream 密度 49.8 % ≈ 50 %，符合靜音時的理論值。Min/Max word 各異，確認資料隨時間變化（麥克風有在收音）。

**介面說明：**

IM69D130 沒有任何暫存器介面（無 I2C/SPI/UART）。唯一的互動是：提供 PDM CLK → 麥克風輸出 PDM bit stream。SELECT 腳位硬體接 GND → L channel → 資料在 CLK 上升沿有效（tDV ≤ 100 ns）。1-density 檢查是能做到最接近 dump 的診斷方式。

**mic_loopback 設計：**

- PDM CLK 3.125 MHz，decimation ratio = 64 → PCM 48 828 Hz（與 I2S 完全同步）
- 每 2 個 PDM FIFO word（64 bits）→ 1 個 I2S stereo sample，無需 DMA
- Integrate-and-dump（1-stage CIC）decimation，增益 ×4（12 dB）
- 輸出限制在 MAX_AMPLITUDE（50 % full scale）保護喇叭
- pio1（PDM mic）＋ pio0（I2S amp）獨立運作，無 gpio_base 衝突

---

## Bringup Firmware & Tooling

### Source layout (post-refactor)

The bringup shell was originally a single 1880-line `i2c_custom_scan.c`. It has been split into focused compilation units:

| File | Lines | Contents |
|------|-------|----------|
| `bringup.h` | ~160 | Shared header: all `#include`, pin `#define`, register `#define`, public function declarations |
| `i2c_custom_scan.c` | ~225 | Main REPL loop, keypad monitor, command dispatch |
| `bringup_power.c` | ~280 | LM27965, BQ25622, motor PWM, Bus B init/deinit |
| `bringup_sensors.c` | ~290 | LSM6DSV16X, LIS2MDL, LPS22HH, Teseo-LIV3FL, Bus A scan |
| `bringup_audio.c` | ~185 | PIO I2S driver, NAU8315 tone/breathe/melody |
| `bringup_flash.c` | ~180 | W25Q128JW JEDEC/UID, APS6404L QMI probe |
| `bringup_lora.c` | ~475 | SX1262 SPI helpers, `lora_test`, `lora_rx`, `lora_dump` |

### Bug fixes applied during bringup

| Bug | Fix |
|-----|-----|
| `lora_dump`: GetDeviceErrors called before calibration (returned all-fail POR default) | Moved to after `Calibrate(0x7F)` + `CalibrateImage({0xE1,0xE9})` |
| `lora_dump`: RxGain WriteRegister used wrong address 0x06B8 | Corrected to **0x08AC** (confirmed: readback = 0x96) |
| `lora_dump` / `lora_rx`: GetStats used 7-byte SPI transfer; `rx[7]` out-of-bounds | Corrected to **8 bytes** (opcode + status + RxOk×2 + CrcErr×2 + HdrErr×2) |
| `bq25622_print_status`: VBUS_STAT decode table had wrong string at index 4 ("Unknown Adapter" → "Adj. HV DCP") | Unified into a single shared `bq_vbus_str[]` array used by both status and dump functions |

### Scripts

| Script | Purpose |
|--------|---------|
| `build_and_flash_bringup.sh` | Build via MSVC + CMake ninja, flash via J-Link SWD |
| `serial_monitor.ps1` | Interactive serial terminal (COM4, 115200). Usage: `.\serial_monitor.ps1 [cmd]` |
| `bringup_run.ps1` | **Automated command runner.** Handles COM port retry, boot banner wait, per-command timeouts. Usage: `.\bringup_run.ps1 [-Flash] [-PortName COM4] cmd1 cmd2 …` |

**`bringup_run.ps1` example invocations:**
```powershell
.\bringup_run.ps1 psram                           # single command
.\bringup_run.ps1 scan_a scan_b status lora_dump  # multiple commands
.\bringup_run.ps1 -Flash scan_a lora_dump psram   # build+flash then test
```

---

## Measurements

See `measurements/` directory for oscilloscope captures, spectrum plots, and current profiles.
