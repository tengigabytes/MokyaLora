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

**Result: ⚠️ CONDITIONAL** (SPI, calibration, RX mode, RSSI verified; RF link and performance not yet tested)

| Item | Result | Notes |
|------|--------|-------|
| SX1262 SPI response | ✅ PASS | GetStatus: ChipMode=2 (STBY_RC), CmdStatus=5 (POR normal) |
| RegSyncWord (0x0740/41) | ✅ PASS | 0x14 0x24 — private network default |
| Calibrate + CalibrateImage | ✅ PASS | GetDeviceErrors = 0x0020 (XOSC flag only — cosmetic false positive, see note) |
| RxGain (0x08AC) | ✅ PASS | Write 0x96, readback 0x96 (boosted LNA confirmed) |
| SetRx → ChipMode=5 | ✅ PASS | RX mode entered successfully |
| RSSI (923.125 MHz, 5 samples) | ✅ PASS | −114.0 dBm consistent (indoor noise floor) |
| GetStats | ✅ PASS | RxOk=0 CrcErr=0 HdrErr=0 (no packets; no crash) |
| OCP (0x08E7) | ⚠️ NOTE | 0x18 → 105 mA (non-default; investigate before TX — default is 0x38 = 185 mA) |
| TX link test | ⏳ PENDING | Actual LoRa packet exchange not yet performed |
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
| BQ27441 fuel gauge I2C presence | ⚠️ CONDITIONAL | Readable only with battery installed or in charging mode |
| BQ27441 SOC / capacity readout | ⏳ PENDING | Detailed gauge characterisation not yet performed |

**BQ25622 register dump (at power-on, no battery, charging disabled by firmware):**

| Register | Value | Field decode |
|----------|-------|--------------|
| PART_INFO (0x38) | 0x0A | PN=1 (BQ25622 ✅), DEV_REV=2 |
| CHG_CTRL0 (0x14) | 0x06 | ICHG = 240 mA — POR default |
| VREG (0x06) | 0x00 | Charge voltage = 3504 mV — POR default; **must be set to 4200 mV for BL-4C** |
| IINDPM (0x00) | 0xFF | Input current limit = 6300 mA — POR default, not yet configured |
| VINDPM (0x01) | 0xFF | Input UVLO = 16600 mV — POR default, not yet configured |
| CTRL1 (0x16) | 0x80 | EN_CHG=0 (charging disabled at startup) |
| STATUS0 (0x1D) | 0x10 | VSYS_STAT=1 — no battery; system powered from VBUS only |
| STATUS1 (0x1E) | 0x04 | CHG=Not charging, VBUS=Adj. HV DCP (USB-C 5 V) |
| FAULT (0x1F) | 0x00 | No faults ✅ |

> ICHG and VREG are POR defaults. Production driver must set VREG=4200 mV, configure IINDPM/VINDPM, and enable charging via CTRL1[EN_CHG].

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
| APS6404L PSRAM | ❌ FAIL | No response on SPI or QPI — hardware fault; see Issue 8 |

**PSRAM diagnostics:**

Firmware and QMI configuration confirmed correct:

| Diagnostic item | Value | Conclusion |
|----------------|-------|-----------|
| GPIO0_CTRL FUNCSEL | 9 (`XIP_CS1`) | GPIO assignment correct ✅ |
| QMI M1 rfmt (post-flash bootrom) | 0x000492A8 | Bootrom pre-configured M1 for QPI |
| SPI write+readback | 0xFFFFFFFF | No response — write not persisted |
| QPI Quad I/O probe (rcmd=0xEB) | 0xFFFFFFFF | No response |

XIP (CS0) and PSRAM (CS1) channels are fully independent — XIP activity does not block CS1. Issue is hardware, not firmware.

**Next action (Issue 8):** DMM continuity: GPIO0 (RP2350B pin 77) → U3 CE# (pin 1); verify VCC_1V8 at U3; inspect solder joints under microscope. Bodge wire if trace is open.

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

---

## Pending Steps

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

**Result: ⏳ PENDING**

BQ27441 I2C presence confirmed (conditional on battery installed). Detailed gauge
characterisation not yet performed.

| Item | Method | Expected result |
|------|--------|----------------|
| SOC readout | Read `StateOfCharge()` (0x1C) | 0–100 % consistent with charge level |
| Voltage readout | Read `Voltage()` (0x08) | ~3.7 V for BL-4C at mid-charge |
| Remaining capacity | Read `RemainingCapacity()` (0x10) | mAh consistent with SOC |
| Average current | Read `AverageCurrent()` (0x14) | Positive during charge, negative during discharge |
| Full charge capacity | Read `FullChargeCapacity()` (0x12) | Should match BL-4C ~890 mAh after learning |
| Gauge learning cycle | Charge to full + discharge to empty | SOC tracking accuracy improves after first cycle |

> BQ27441 requires at least one full charge/discharge cycle for the Impedance Track algorithm to calibrate capacity accurately. First readout may show default design capacity.

### Step 13 — TFT LCD Fast Refresh

**Result: ✅ PASS**

| Item | Method | Result |
|------|--------|--------|
| TE pin frequency | Count rising edges on GPIO 22 over 2 s | ✅ PASS — 120 edges → **60.0 Hz** |
| Baseline FPS (CPU polling) | 10 full-screen fills, `pio_sm_put_blocking`, clkdiv=4 | ✅ MEASURED — **1.46 FPS** (682 ms/frame, 0.225 MB/s) |
| DMA solid fill — clkdiv=4 | DMA_SIZE_8 + 2-byte ring buf → PIO TX FIFO, 10 fills | ✅ PASS — **59.88 FPS** (16.7 ms/frame, 9.2 MB/s) — **41× faster than CPU polling** |
| DMA solid fill — clkdiv=3 | Same DMA path, 96 ns write cycle, 10 fills | ✅ PASS — **79.37 FPS** (12.6 ms/frame, 12.2 MB/s) — no visible pixel glitches |
| TE-gated DMA fill | Gate frame start on TE rising edge, clkdiv=4, 10 frames | ✅ PASS — **54.35 FPS**; transfer fits within one frame period — tear-free capable |

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

**Result: ⏳ PENDING**

Indoor test confirmed I2C streaming and NMEA output. RF chain performance requires outdoor
test with clear sky view.

| Item | Expected result |
|------|----------------|
| Cold start TTFF (open sky) | < 60 s (Teseo-LIV3FL datasheet: typ. 27 s) |
| Warm start TTFF | < 5 s |
| Position accuracy (static) | CEP50 < 2.5 m (Teseo-LIV3FL spec) |
| Satellite count | ≥ 6 SVs tracked (GPS + GLONASS) |
| SNR per satellite | > 30 dB-Hz for healthy signal |

> RF chain: Teseo-LIV3FL → BGA725L6 LNA → B39162B4327P810 SAW filter → chip antenna.
> Verify LNA enable/disable control and SAW passband.

### Step 15 — LoRa RF Performance

**Result: ⏳ PENDING**

SPI interface and RX mode entry confirmed. Actual RF link performance not yet verified.

| Item | Method | Expected result |
|------|--------|----------------|
| TX packet — loopback | Two boards; one TX, one RX | Packet received, CRC OK |
| RSSI at known distance | 10 m line-of-sight | RSSI consistent with path loss model |
| Sensitivity | Long-range test (SF12, BW125) | −148 dBm datasheet spec |
| OCP investigation | Read 0x08E7; confirm expected value | Determine if 105 mA setting is intentional or POR anomaly |
| Frequency accuracy | Compare TX frequency against reference | TCXO ±0.5 ppm spec |

### Step 16 — Core 1 Functionality

**Result: ⚠️ PARTIAL** (Stage A bare-metal ✅ PASS; Stage B-0 Core 1 REPL ✅ PASS; Stage B FreeRTOS ⏳ PENDING)

All bringup to date has run on Core 0. Core 1 functionality must be validated separately
before starting UI/application firmware development.

| Item | Method | Expected result | Actual result |
|------|--------|----------------|---------------|
| Core 1 boot | Launch Core 1 via `multicore_launch_core1()` | Core 1 enters its entry function | ✅ PASS — `C1_READY` token `0xC1B00700` received |
| Inter-core FIFO (SIO) | Core 0 pushes value; Core 1 reads and echoes back | Round-trip confirmed | ✅ PASS — 4/4 probe values echoed correctly |
| Shared SRAM access | Both cores read/write a shared buffer with spinlock | No data corruption | ✅ PASS — Core 1 read `0xBEEFC0DE` as written by Core 0 |
| Core 1 peripheral access | Core 1 controls an LED or GPIO | Peripheral access from Core 1 confirmed | ✅ PASS — motor pin (GPIO 9) toggled from Core 1 |
| Core 1 REPL (Stage B-0) | Run full bringup REPL on Core 1; Core 0 owns USB CDC init | All REPL commands work identically to Core 0 run | ✅ PASS — imu/baro/lora/sram confirmed on Core 1 |
| Core 1 FreeRTOS | Start FreeRTOS scheduler on Core 1; create test task | Task runs, prints heartbeat | ⏳ PENDING — Stage B |

**Firmware notes:**
- Stage A implemented in `firmware/tools/bringup/bringup_core1.c`; command: `core1`
- Protocol: Core 1 sends `C1_READY (0xC1B00700)` on boot; Core 0 sends command tokens (`C0_CMD_ECHO/SRAM/GPIO/SHUTDOWN`) via SIO FIFO
- All FIFO pops on Core 0 use `multicore_fifo_pop_timeout_us()` to prevent hangs
- `pico_multicore` + `hardware_sync` added to bringup CMakeLists
- `multicore_reset_core1()` called after shutdown to leave Core 1 in clean state
- Stage B prerequisite complete: `FreeRTOS-Kernel V11.3.0` added as submodule at `firmware/core1/freertos-kernel/` (MIT); nested submodule `Community-Supported-Ports` provides `RP2350_ARM_NTZ` Cortex-M33 port; `FreeRTOS_Kernel_import.cmake` auto-selects this port when `PICO_PLATFORM=rp2350-arm-s`
- Stage B-0 (`core1_bringup_test`): `bringup_repl_run()` split into `bringup_repl_init()` (Core 0; registers USB CDC IRQ on Core 0's NVIC) and `bringup_repl_loop()` (Core 1; REPL loop only). `multicore_reset_core1()` called before `multicore_launch_core1()` — required because J-Link only resets Core 0 when reflashing; Core 1 may still be in user code and will not respond to the FIFO handshake unless explicitly PSM-cycled first.
- Stage B remaining: write `FreeRTOSConfig.h`, implement heartbeat task on Core 1

> Core 1 will run FreeRTOS (LVGL + MIE integration) in the production firmware. The IPC
> protocol (`firmware/shared/ipc/ipc_protocol.h`) uses RP2350 HW FIFO as the doorbell
> mechanism — this must be validated before Core 1 development starts.

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
| 8 | 2026-04-03 | U3 (APS6404L PSRAM) | No response on SPI or QPI — returns 0xFFFFFFFF | Suspected open trace: GPIO0 (pin 77) → U3 CE# (pin 1), or VCC_1V8 open at U3 | None — requires physical inspection | Verify trace continuity + VCC; bodge wire if open; replace U3 if VCC shorted |

---

## Measurements

See `measurements/` directory for oscilloscope captures, spectrum plots, and current profiles.
