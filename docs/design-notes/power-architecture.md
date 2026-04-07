# Power Architecture

**Project:** MokyaLora
**Revision:** Rev 5.2

---

## 1. Overview

The system uses a dual-rail voltage architecture with intelligent path management (NVDC) provided by the TI BQ25620 charger. All MCU and digital logic runs at 1.8 V; sensitive RF/analogue rails run at 3.3 V.

---

## 2. Power Tree

```
USB-C VBUS ──┐
             │
        BQ25620 (Charger / Path Manager)
             │
        VSYS (Battery-backed system rail)
        ├── TPS62840  →  1.8 V (Rail A)
        ├── TPS7A2033 →  3.3 V (Rail B)
        ├── LCD Backlight (via LM27965 / TPS22919 load switch)
        ├── Vibration Motor (via SSM3K56ACT low-side MOSFET)
        └── BQ25620 PMID → OTG 5 V Boost (Rail D)
```

---

## 3. Rail Definitions

### Rail A — 1.8 V (TPS62840 Buck)

| Parameter        | Value                         |
|------------------|-------------------------------|
| Regulator        | TI TPS62840DLCR (VSON-8)      |
| Quiescent current| 60 nA                         |
| Loads            | RP2350B IOVDD/DVDD, W25Q128JW Flash, APS6404L PSRAM, SX1262 VCC, IM69D130 VDD, NHD LCD VDDI (logic), Teseo-LIV3FL VCC_IO |
| Behaviour        | Always-on; Dormant mode draws < 10 µA system-wide |

### Rail B — 3.3 V (TPS7A2033 LDO)

| Parameter        | Value                         |
|------------------|-------------------------------|
| Regulator        | TI TPS7A2033 (WSON-6)         |
| Output current   | 300 mA max                    |
| PSRR             | Ultra-low noise                |
| Loads            | Teseo-LIV3FL VCC (RF), NHD LCD VDD (analogue), LIS2MDL, LPS22HH |
| Enable           | **Rev A: EN tied to VSYS — always-on (no software control).** Rev B: route EN to GPIO for DORMANT power-gating. See §8.2 Issue A |

### Rail C — VSYS (BQ25620 Direct)

- Source: BQ25620 NVDC output (battery voltage ≈ 3.5 V–4.2 V; VBUS when charging).
- Loads: Buck input, LDO input, Audio Amp NAU8315, LCD backlight string, Vibration motor.
- Dead-battery support: BQ25620 powers VSYS from VBUS before battery charges.

### Rail D — OTG 5 V (BQ25620 PMID Boost)

- Activated via I2C command: `charger.enableOTG(true)`.
- Powers USB VBUS output for reverse charging (power bank mode).
- PMID bulk capacitor must be placed close to BQ25620.
- VBUS trace must support ≥ 2 A current.

---

## 4. Charger — TI BQ25620

| Parameter          | Value / Setting                               |
|--------------------|-----------------------------------------------|
| Part               | BQ25622RWMR (WQFN-18, no D+/D− detection)    |
| Input current limit| Hardware ILIM resistor sets 500 mA default; I2C raises to 1 A–2 A after enumeration |
| Charge control     | I2C (I2C1, 0x6B)                             |
| ADC                | 16-bit monitoring of VBUS, VBAT, IBAT, TS    |
| Inductor           | Murata DFE322520F-1R5M (1.5 µH, metal alloy) |
| OTG                | Boost to 5 V via PMID; 2 A capable           |
| Interrupt          | Open-drain INT → GPIO 8 (PWR_INT), 1.8 V pull-up |

**Rationale for BQ25622 (no D+/D−):** eliminates charger interference with MCU USB enumeration on shared lines.

---

## 5. Fuel Gauge — TI BQ27441-G1A

- Algorithm: Impedance Track™ — accurate remaining time estimate.
- Sense path: SRX/VSS must be in series with battery current path; keep trace short and wide.
- Design Capacity setting: 1000 mAh (software initialisation).
- Address: 0x55 on I2C1.

---

## 6. LED Driver — TI LM27965

- Address: 0x36 on I2C0.
- HWEN pin: tied to 1.8 V (always enabled; software controls via I2C).
- Bank A: LCD backlight — PWM dimming.
- Bank B: Keypad backlight (white LEDs) — PWM dimming.
- Remaining channels: status LED (red / green).

---

## 7. Key Design Rules

1. Bulk capacitance on VSYS must handle TX burst transient (SX1262 peak ~120 mA).
2. PMID capacitor must be as close to BQ25620 as possible for stable OTG output.
3. BQ27441 sense resistor trace must be short, wide, and low-resistance.
4. Motor MOSFET (SSM3K56ACT) requires a flyback diode across motor terminals to absorb back-EMF.
5. Motor average voltage clamped to ~3.0 V via software PWM duty-cycle compensation based on VBAT ADC reading.

---

## 8. Long-Standby Power Review (2026-04-07)

### 8.1 Standby Current Budget

**Target:** < 50 µA total system current in DORMANT (Nokia BL-4C ~890 mAh → 50 µA = ~2 years theoretical standby).

**Realistic target for Meshtastic:** duty-cycle RX (SX1262 wakes periodically for Rx window) → ~200–500 µA average, giving ~70–180 days standby with 890 mAh.

### 8.2 Rail-by-Rail Issues

#### Issue A — 3.3 V LDO (TPS7A2033) always-on

**Severity: HIGH**

TPS7A2033 EN is tied to VSYS — **cannot be software-disabled**. Even with all 3.3 V loads in low-power mode, the LDO itself draws Iq = 12 µA. More critically:

| 3.3 V Load | Active | Sleep/LP Mode | Residual on 3.3V rail |
|------------|--------|--------------|----------------------|
| Teseo-LIV3FL VCC (RF) | ~26 mA | `$PSTMLOWPOWERONOFF` — TBD | **Unknown** — need to verify if internal LDOs/PLL fully shut down, or if VCC pin still draws leakage |
| NHD LCD VDD (analogue) | ~few mA | SLPIN: < 50 µA | ~50 µA (display RAM refresh) |
| LPS22HH | 12 µA | Power-down: 1 µA | 1 µA |
| LIS2MDL | 0.2 mA | Idle: 2 µA | 2 µA |
| **LDO Iq** | — | — | **12 µA** |
| **Subtotal** | | | **~65 µA + Teseo residual** |

**Rev B recommendation:**
1. Route TPS7A2033 EN to a GPIO — software can power-gate the entire 3.3 V rail in DORMANT.
2. Verify all 3.3 V loads tolerate power loss (no data corruption, no latchup on re-power).
3. If LCD VDD is cut, LCD must re-init on wake (acceptable — already done after SLPIN).
4. Teseo VCC loss → cold start penalty (~30 s TTFF vs. hot-start ~1 s). Acceptable for DORMANT.

#### Issue B — I2C Bus Pull-Up Static Current

**Severity: MEDIUM (conditional)**

Both I2C buses have 2.2–4.7 kΩ pull-ups to 1.8 V. In normal idle (bus HIGH), leakage is negligible. But:
- If any slave holds SDA LOW (e.g., BQ27441 latchup per Issue 10), **1.8 V / 2.2 kΩ = 0.8 mA per line**.
- In DORMANT, MCU GPIO goes Hi-Z — if a slave latches SDA/SCL LOW, pull-up current flows continuously.

**Rev B recommendation:**
1. Bus B (power bus): add series load switch (TPS22919 or similar) between 1.8 V and pull-up resistors — cut pull-up power in DORMANT.
2. Or: use 10 kΩ pull-ups (higher value reduces worst-case static current to 0.18 mA per line). Trade-off: slower I2C edge rate, may limit clock speed to 100 kHz.
3. Investigate BQ27441 latchup root cause (Issue 10) — if resolved, this becomes non-issue.

#### Issue C — LM27965 HWEN Always Enabled

**Severity: LOW-MEDIUM**

LM27965 HWEN is tied to 1.8 V → LED driver IC is always powered. With all banks disabled (GP = 0x20), IC quiescent current is ~5 µA (typical, verify datasheet).

**Rev B recommendation:**
1. Route HWEN to a GPIO — allows full shutdown of LED driver in DORMANT.
2. On wake, re-init LM27965 via I2C (all registers revert to defaults after HWEN toggle).

#### Issue D — BQ25622 Charger Iq

**Severity: LOW**

BQ25622 active Iq is ~1 mA (ADC scanning, watchdog). In HIZ mode: ~10 µA.

Questions to verify:
- Does HIZ mode disconnect VBUS from VSYS? (Yes — HIZ disables the power path, but battery still powers VSYS directly through the body diode / low-side FET.)
- Can watchdog be disabled to prevent periodic wake? (Yes — REG07 WATCHDOG = 00 disables timer.)
- ADC continuous scan should be disabled in standby (set REG0F ADC_EN = 0).

#### Issue E — PSRAM Standby Current

**Severity: LOW**

APS6404L standby (CE# HIGH) is ~40 µA. Half-Sleep mode (vendor-specific command) claims ~20 µA. Deep Power Down is ~3 µA but loses all data.

Production firmware loads MIE dictionary from Flash to PSRAM at boot. If dictionary is re-loadable (~200 ms from Flash at 37.5 MHz), PSRAM data loss in Deep Power Down is acceptable. Re-load on wake.

**Rev B recommendation:**
1. Use PSRAM Deep Power Down in DORMANT (save ~37 µA vs. standby).
2. Budget ~200 ms MIE dictionary reload on wake from DORMANT.

#### Issue F — NAND Flash (Rev B, new)

**Severity: TBD — depends on part selection**

SPI NAND standby current varies by part:
- Typical: 10–50 µA (CE# HIGH, idle)
- Deep Power Down: < 5 µA (if supported)

Must verify selected NAND part has a true low-power standby. Budget 10–50 µA in power estimate.

### 8.3 Dormant Current Estimate Summary

| Component | Rev A (est.) | Rev B (optimized) | Notes |
|-----------|-------------|-------------------|-------|
| RP2350B DORMANT | 20 µA | 20 µA | POWMAN, XOSC off |
| TPS62840 Iq | 0.06 µA | 0.06 µA | 60 nA — negligible |
| TPS7A2033 Iq + loads | 65 µA + Teseo? | **0 µA (gated)** | Rev B: EN → GPIO |
| W25Q128JW DPD | 1 µA | 1 µA | |
| APS6404L | 40 µA (standby) | 3 µA (DPD) | Reload MIE on wake |
| SX1262 Sleep | 0.16 µA | 0.16 µA | DIO1 wake functional |
| BQ25622 | 10 µA (HIZ) | 10 µA (HIZ) | Watchdog + ADC disabled |
| BQ27441 | 1 µA | 1 µA | Auto sleep |
| LM27965 | 5 µA | **0 µA (HWEN off)** | Rev B: HWEN → GPIO |
| NAND Flash | — | 10–50 µA (TBD) | Part-dependent |
| I2C pull-ups | ~0 (bus idle) | ~0 (bus idle) | Worst-case if latchup: add load switch |
| LCD panel (SLPIN) | 50 µA | **0 µA (VDD off)** | Rev B: 3.3V gated |
| Sensors (LP) | 5 µA | 5 µA | |
| **Total** | **~200 µA + Teseo** | **~50–90 µA** | Rev B with EN GPIO |

### 8.4 Rev B Power Tree Recommendations Summary

| # | Change | Impact | Priority |
|---|--------|--------|----------|
| 1 | **TPS7A2033 EN → GPIO** | Gate entire 3.3 V rail in DORMANT; saves 65+ µA + Teseo leakage | **Critical** |
| 2 | **LM27965 HWEN → GPIO** | Full LED driver shutdown; saves ~5 µA | Medium |
| 3 | **PSRAM Deep Power Down** in DORMANT | Saves ~37 µA; 200 ms reload cost on wake | Medium |
| 4 | **I2C Bus B pull-up load switch** or higher-value pull-ups | Protect against BQ27441 latchup drain | Low (conditional on Issue 10) |
| 5 | **BQ25622 dormant config:** WDT off, ADC off, HIZ | Saves ~990 µA vs. default active | High (firmware) |
| 6 | **NAND Flash part selection:** require Deep Power Down | Avoid 50 µA standing drain | High (BOM) |
| 7 | **SX1262 duty-cycle RX budget** | Dominates average current in Meshtastic STANDBY | Design (firmware) |

### 8.5 Power Modes & Expected Battery Life

Battery: Nokia BL-4C, 890 mAh. Charger efficiency losses excluded (battery-direct).

| Mode | System Current (est.) | Battery Life |
|------|----------------------|-------------|
| ACTIVE (screen on, Rx/Tx) | ~50 mA | ~18 hours |
| STANDBY (screen off, duty-cycle Rx) | ~500 µA (Rev A) / ~200 µA (Rev B) | 74 days / 185 days |
| DORMANT (all off, GPIO wake only) | ~200 µA (Rev A) / ~50 µA (Rev B) | 185 days / **2+ years** |

> **Note:** STANDBY with SX1262 duty-cycle Rx (~2% duty, 4.5 mA Rx) contributes ~90 µA average. This is the dominant term and is protocol-determined (Meshtastic slot timing). Hardware optimization has diminishing returns below this floor.
