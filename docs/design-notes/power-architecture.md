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
| Enable           | EN tied to VSYS/1.8 V — always-on, no software control needed |

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
