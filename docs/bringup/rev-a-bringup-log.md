# Rev A Bring-Up Log

**Board revision:** Rev A
**Date started:** TBD

---

## Pre-Power Checklist

- [ ] Visual inspection: no solder bridges, correct component orientation.
- [ ] Continuity check: VSYS, 1.8 V, 3.3 V rails to GND (expect open before power).
- [ ] Resistance to GND: VSYS rail > 10 Ω, no short.
- [ ] USB-C CC1 / CC2 pull-down resistors (5.1 kΩ) present.

## Bring-Up Sequence

### Step 1 — Power Rails

1. Apply 5 V USB-C. Measure VSYS voltage (expect 3.5 V–4.2 V from charger).
2. Measure 1.8 V rail (TPS62840 output).
3. Measure 3.3 V rail (TPS7A2033 output).
4. Confirm quiescent current is < 20 mA with no firmware loaded.

| Rail   | Expected | Measured | Pass/Fail |
|--------|----------|----------|-----------|
| VSYS   |          |          |           |
| 1.8 V  | 1.80 V   |          |           |
| 3.3 V  | 3.30 V   |          |           |

### Step 2 — MCU Boot

1. Connect SWD debugger (SWCLK / SWDIO / GND).
2. Flash minimal test firmware via SWD.
3. Confirm UART debug output on DBG_TX (GPIO 4) at 115200 baud.

### Step 3 — I2C Bus Scan

Run I2C scan on both buses and confirm expected addresses:

| Address | Device         | Bus  | Expected |
|---------|----------------|------|----------|
| 0x6B    | BQ25620        | I2C1 |          |
| 0x55    | BQ27441        | I2C1 |          |
| 0x6A    | LSM6DSV16X     | I2C0 |          |
| 0x1E    | LIS2MDL        | I2C0 |          |
| 0x5C    | LPS22HH        | I2C0 |          |
| 0x3A    | Teseo-LIV3FL   | I2C0 |          |
| 0x36    | LM27965        | I2C0 |          |

### Step 4 — Display

1. Run LCD initialisation sequence.
2. Fill screen with solid red, green, blue — verify no pixel defects.
3. Test backlight PWM (LM27965 Bank A).

### Step 5 — LoRa

1. Read SX1262 device ID over SPI1.
2. Set to standby mode; verify BUSY pin deasserts.
3. Perform loopback or range test.

### Step 6 — Keypad

1. Enable PIO+DMA keypad scanner.
2. Press each key individually; verify correct row/column detection.
3. Test NKRO: press 3+ keys simultaneously.

### Step 7 — Audio

1. Play 1 kHz tone via NAU8315 / speaker.
2. Record via IM69D130 PDM mic; verify SNR.

### Step 8 — Battery

1. Connect Nokia BL-4C battery.
2. Read SoC and voltage from BQ27441.
3. Verify charging LED (LM27965) reflects charge state.

---

## Issues Log

| Date | Issue | Root Cause | Fix |
|------|-------|-----------|-----|
|      |       |           |     |

---

## Measurements

See `measurements/` directory for oscilloscope captures, spectrum plots, and current profiles.
