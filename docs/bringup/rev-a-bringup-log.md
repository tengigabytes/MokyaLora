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

### Step 3 — I2C Bus Scan

| Address | Device         | Bus  | Result | Notes |
|---------|----------------|------|--------|-------|
| 0x6B    | BQ25622        | I2C1 | ✅ PASS | After TP101 fix (Issue 2) |
| 0x55    | BQ27441        | I2C1 | ⚠️ CONDITIONAL | Readable only with battery installed or in charging mode |
| 0x6A    | LSM6DSV16X     | I2C0 | ✅ PASS | |
| 0x1E    | LIS2MDL        | I2C0 | ✅ PASS | After R54/R55 jumper fix (Issue 3) |
| 0x5D    | LPS22HH        | I2C0 | ✅ PASS | Address is **0x5D** (SA0 = 3.3 V); design docs incorrectly stated 0x5C — see Issue 4 |
| 0x3A    | Teseo-LIV3FL   | I2C0 | ✅ PASS | |
| 0x36    | LM27965        | I2C1 | ✅ PASS | |

### Step 4 — Display

**Result: ❌ FAIL**

LCD FPC connector pin order is incompatible with NHD-2.4-240320AF-CSXP — display cannot be driven.
No temporary workaround. See Issue 5. Pending Rev B correction.

### Step 5 — LoRa

**Result: 🔲 NOT TESTED**

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

---

## Measurements

See `measurements/` directory for oscilloscope captures, spectrum plots, and current profiles.
