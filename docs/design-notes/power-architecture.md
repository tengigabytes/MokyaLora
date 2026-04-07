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
| Loads            | RP2350B IOVDD/DVDD, W25Q128JW Flash, APS6404L PSRAM, SX1262 VDD_IN + VBAT_IO, IM69D130 VDD, NHD LCD VDDI (logic), Teseo-LIV3FL VCC_IO |
| Behaviour        | Always-on; Dormant mode draws < 10 µA system-wide |

### Rail B — 3.3 V (TPS7A2033 LDO)

| Parameter        | Value                         |
|------------------|-------------------------------|
| Regulator        | TI TPS7A2033 (WSON-6)         |
| Output current   | 300 mA max                    |
| PSRR             | Ultra-low noise                |
| Loads            | **SX1262 VBAT (PA + Rx frontend)**, Teseo-LIV3FL VCC (RF), NHD LCD VDD (analogue), LIS2MDL, LPS22HH |
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

**Critical finding:** SX1262 **VBAT (pin 10) is on the 3.3 V rail**, not 1.8 V. VBAT powers the PA (TX) and Rx frontend (LNA, mixer). The 3.3 V rail **cannot be shut down** whenever LoRa needs to operate:

| Power Mode | LoRa State | 3.3 V Rail | Reason |
|------------|-----------|-----------|--------|
| ACTIVE | Rx/Tx | **Must be ON** | VBAT needed for PA and Rx frontend |
| STANDBY (duty-cycle Rx) | Periodic Rx | **Must be ON** | VBAT needed for Rx windows |
| DORMANT + LoRa wake | Sleep (warm start) | **Must be ON** | VBAT needed for SX1262 to retain state and respond to DIO1 wake |
| DORMANT (PWR_BTN only) | Power off | Can be OFF | No LoRa — deepest sleep |

SX1262 power pins (from schematic):
- **VBAT (pin 10) → VCC_3V3** — PA + Rx analog supply (1.8–3.7 V range, 3.3 V on this board)
- **VDD_IN (pin 1) → VCC_1V8** — digital core
- **VBAT_IO (pin 11) → VCC_1V8** — SPI I/O levels

This means the 3.3 V rail is effectively **required for all normal operation** (Meshtastic always has LoRa active). Only in the deepest DORMANT (no LoRa, PWR_BTN wake only) can it be shut down.

**Implication for STANDBY power:** The 3.3 V LDO (12 µA Iq) plus all parasitic 3.3 V loads (Teseo leakage, LCD SLPIN, sensors) are **always present** during STANDBY. This makes optimizing the non-LoRa 3.3 V loads more important:

| 3.3 V Load | STANDBY draw | Can be reduced? |
|------------|-------------|----------------|
| SX1262 VBAT (duty-cycle Rx) | ~90 µA avg (2% duty × 4.5 mA) | No — protocol-defined |
| Teseo-LIV3FL VCC | **TBD (LP mode residual)** | `$PSTMLOWPOWERONOFF` — verify |
| NHD LCD VDD (SLPIN) | ~50 µA | Minimal — already sleeping |
| LPS22HH + LIS2MDL | ~3 µA | Already power-down |
| TPS7A2033 Iq | 12 µA | No — inherent |
| **Subtotal** | **~155 µA + Teseo** | |

**Rev B recommendation:**
1. **Still route TPS7A2033 EN to a GPIO** — enables deepest DORMANT (PWR_BTN-only wake) by shutting down entire 3.3 V rail. Saves ~155 µA + Teseo leakage in that mode.
2. For STANDBY: 3.3 V stays on; focus on minimizing parasitic loads (Teseo LP, sensor power-down).
3. **Teseo residual current on VCC is the biggest unknown** — if `$PSTMLOWPOWERONOFF` doesn't fully gate internal regulators, Teseo may leak mA-level current on the 3.3 V rail even in LP mode. Measure this during Step 26 bringup.
4. **Consider splitting SX1262 VBAT to a separate 3.3 V supply** (e.g., dedicated LDO or buck with lower Iq) to avoid powering Teseo/LCD/sensors unnecessarily in STANDBY. Trade-off: BOM cost + board area vs. significant power saving.
5. **Alternative: load switch on non-LoRa 3.3 V loads** — keep SX1262 VBAT powered from TPS7A2033, but gate Teseo VCC and LCD VDD through a load switch (TPS22919, ~1 µA Iq). This isolates LoRa from the other 3.3 V consumers without a second regulator.

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

#### DORMANT with LoRa wake (SX1262 sleep warm-start, DIO1 wake)

3.3 V rail must stay ON for SX1262 VBAT.

| Component | Rev A (est.) | Rev B (optimized) | Notes |
|-----------|-------------|-------------------|-------|
| RP2350B DORMANT | 20 µA | 20 µA | POWMAN, XOSC off |
| TPS62840 Iq | 0.06 µA | 0.06 µA | 60 nA — negligible |
| TPS7A2033 Iq | 12 µA | 12 µA | Must stay on for SX1262 VBAT |
| SX1262 Sleep (warm) | 0.6 µA | 0.6 µA | VBAT on 3.3V, DIO1 wake functional |
| Teseo VCC (LP) | **TBD** | **TBD (load switch)** | Rev B: gate via TPS22919 if LP residual is high |
| LCD VDD (SLPIN) | 50 µA | **~1 µA (load switch)** | Rev B: gate LCD VDD via load switch |
| Sensors (3.3V, LP) | 3 µA | 3 µA | LPS22HH + LIS2MDL power-down |
| W25Q128JW DPD | 1 µA | 1 µA | |
| APS6404L | 40 µA (standby) | 3 µA (DPD) | Reload MIE on wake |
| BQ25622 | 10 µA (HIZ) | 10 µA (HIZ) | Watchdog + ADC disabled |
| BQ27441 | 1 µA | 1 µA | Auto sleep |
| LM27965 | 5 µA | **0 µA (HWEN off)** | Rev B: HWEN → GPIO |
| NAND Flash | — | 10–50 µA (TBD) | Part-dependent |
| **Total** | **~145 µA + Teseo** | **~65–105 µA + Teseo** | Teseo residual is the key unknown |

#### DORMANT without LoRa (PWR_BTN wake only, deepest sleep)

3.3 V rail can be fully shut down. SX1262 loses state (cold start on wake).

| Component | Rev A (est.) | Rev B (optimized) | Notes |
|-----------|-------------|-------------------|-------|
| RP2350B DORMANT | 20 µA | 20 µA | |
| TPS62840 Iq | 0.06 µA | 0.06 µA | |
| TPS7A2033 | 12 µA (always-on) | **0 µA (EN off)** | Rev B: EN → GPIO |
| W25Q128JW DPD | 1 µA | 1 µA | |
| APS6404L DPD | 40 µA / 3 µA | 3 µA | |
| BQ25622 HIZ | 10 µA | 10 µA | |
| BQ27441 | 1 µA | 1 µA | |
| LM27965 | 5 µA | **0 µA (HWEN off)** | |
| NAND Flash | — | 10–50 µA (TBD) | |
| All 3.3V loads | 55 µA + Teseo | **0 µA (rail off)** | |
| **Total** | **~145 µA + Teseo** | **~45–85 µA** | True deep sleep |

### 8.4 Rev B Power Tree Recommendations Summary

| # | Change | Impact | Priority |
|---|--------|--------|----------|
| 1 | **TPS7A2033 EN → GPIO** | Gate entire 3.3 V rail in deepest DORMANT (PWR_BTN only); saves all 3.3 V loads | **Critical** |
| 2 | **Load switch on non-LoRa 3.3 V loads** (Teseo VCC, LCD VDD) | In STANDBY/LoRa-wake DORMANT: isolate Teseo + LCD from SX1262 VBAT; saves Teseo leakage + LCD 50 µA | **High** |
| 3 | **LM27965 HWEN → GPIO** | Full LED driver shutdown; saves ~5 µA | Medium |
| 4 | **PSRAM Deep Power Down** in DORMANT | Saves ~37 µA; 200 ms reload cost on wake | Medium |
| 5 | **I2C Bus B pull-up load switch** or higher-value pull-ups | Protect against BQ27441 latchup drain | Low (conditional on Issue 10) |
| 6 | **BQ25622 dormant config:** WDT off, ADC off, HIZ | Saves ~990 µA vs. default active | High (firmware) |
| 7 | **NAND Flash part selection:** require Deep Power Down | Avoid 50 µA standing drain | High (BOM) |
| 8 | **Measure Teseo VCC residual in LP mode** | Determine if load switch is necessary | **High** (Step 26 bringup) |

### 8.5 Power Modes & Expected Battery Life

Battery: Nokia BL-4C, 890 mAh. Charger efficiency losses excluded (battery-direct).

| Mode | 3.3 V Rail | System Current (est.) | Battery Life |
|------|-----------|----------------------|-------------|
| ACTIVE (screen on, Rx/Tx) | ON | ~50 mA | ~18 hours |
| STANDBY (screen off, duty-cycle Rx) | **ON** (SX1262 VBAT) | ~300 µA (Rev A) / ~200 µA (Rev B + load switch) | 123 days / 185 days |
| DORMANT + LoRa wake | **ON** (SX1262 VBAT) | ~150 µA (Rev A) / ~100 µA (Rev B + load switch) | 247 days / 370 days |
| DORMANT (PWR_BTN only) | **OFF** (Rev B) | ~150 µA (Rev A) / ~50 µA (Rev B, EN gated) | 247 days / **2+ years** |

> **Key constraint:** SX1262 VBAT on 3.3 V rail means the LDO must stay on whenever LoRa is needed (STANDBY + LoRa-wake DORMANT). The dominant STANDBY terms are SX1262 duty-cycle Rx (~90 µA avg) and Teseo VCC residual (unknown — **must measure in Step 26**).

> **Rev B optimization path:** Add load switch (TPS22919) between TPS7A2033 output and non-LoRa 3.3 V loads (Teseo, LCD, sensors). SX1262 VBAT stays powered directly from LDO. This allows STANDBY current to approach **~115 µA** (LDO Iq + SX1262 duty-cycle + MCU dormant) without the Teseo/LCD parasitic draw.
