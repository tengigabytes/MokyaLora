# System Requirements Specification

**Project:** MokyaLora (Project MS-RP2350 — Meshtastic Feature Phone)
**Version:** v5.2

---

## 1. Project Overview

This project develops a standalone Meshtastic handheld terminal based on the Raspberry Pi RP2350B.
The goal is fully phone-free long-range mesh communication.
The device integrates a dedicated 36-key physical matrix keyboard, phone-grade Chinese predictive input, a calculator function, and a USB data-terminal mode.

## 2. System Architecture

![System Block Diagram](../../docs/assets/system-block-diagram.png)

### Central Processing Unit

| Block | Key Component | Top-Level Function |
|-------|---------------|--------------------|
| MCU | RP2350B (dual-core Cortex-M33) | Core 0 — Meshtastic protocol stack and LoRa radio. Core 1 — UI, IME, power management, application logic. |

### Left-Side Peripherals — Power and Human Interface

| Block | Key Component | Top-Level Function |
|-------|---------------|--------------------|
| USB-C | GCT USB4105-GF-A | Power input (5 V charging); USB CDC serial in data-terminal mode; OTG 5 V reverse output. |
| Power & Charger | BQ25620, BQ27441, TPS62840, TPS7A2033, LM27965 | Battery charging and path management, fuel gauging, 1.8 V / 3.3 V rail generation, backlight dimming. |
| Battery | Nokia BL-4C (~890 mAh) | Primary Li-ion energy source. |
| Microphone | Infineon IM69D130 | **Rev A only — removed from next HW revision.** PDM digital microphone; was reserved for voice-message capture. |
| Speaker | NAU8315 + CMS-131304 | **Rev A only — removed from next HW revision.** Class-D amplifier and SMT speaker; was reserved for audio playback and alerts. |
| Keyboard | 36-key 6×6 matrix (SWT0105 + SDM03U40) | Primary text entry and navigation input; NKRO via PIO + DMA scanner. |
| Vibrate Motor | HD-EMB1104-SM-2 (ERM) | Haptic feedback for incoming messages and alerts. |

### Right-Side Peripherals — Radio and Sensing

| Block | Key Component | Top-Level Function |
|-------|---------------|--------------------|
| LoRa | SX1262 + ECS-TXO-20CSMV4 TCXO | Sub-GHz LoRa transceiver; Meshtastic mesh communication over long range. |
| Display | NHD-2.4-240320AF (2.4″ IPS, 240×320) | Primary user interface display; driven over 8-bit parallel 8080 bus. |
| GNSS | ST Teseo-LIV3FL | Multi-constellation GNSS receiver; provides node position for the Meshtastic mesh. |
| IMU — ACC / GYRO | ST LSM6DSV16X | 6-axis inertial measurement; motion detection, orientation, activity recognition. |
| IMU — Magnetometer | ST LIS2MDL | 3-axis magnetometer; compass heading. |
| IMU — Barometer | ST LPS22HH | Barometric pressure sensor; altitude estimation. |

---

## 3. Core Design Philosophy

- **Dual-core AMP split:** leverages the RP2350B dual-core architecture.
  - **Core 0:** Meshtastic protocol, LoRa radio.
  - **Core 1:** UI rendering, Input Method Engine (IME), application logic, power management.
- **1.8 V system logic:** unified 1.8 V I/O with Dormant sleep for maximum battery life.
- **Virtualised architecture:** software isolation keeps the standard Meshtastic firmware on Core 0, preserving upstream compatibility.

## 4. Operating Modes

The device supports two mutually exclusive modes, selected by the user on USB insertion:

**Mode A — Standalone / Charge-Only**
- The device operates as a handheld radio.
- USB state: power only (VBUS charging, no USB enumeration).
- Features: full operation — LoRa Tx/Rx, GPS, display, 36-key input.

**Mode B — USB Data Terminal**
- The device acts as a LoRa modem for a PC or phone.
- USB state: CDC Serial enumerated; PC sees a virtual COM port.
- Features: Core 1 bridges USB data to Core 0 transparently.

## 5. System-Level Specification Summary

| Parameter        | Value / Part                                                |
|------------------|-------------------------------------------------------------|
| MCU              | Raspberry Pi RP2350B (dual-core Cortex-M33, 1.8 V logic)   |
| Flash            | 16 MB QSPI (1.8 V)                                         |
| PSRAM            | 8 MB QSPI (1.8 V)                                          |
| Display          | 2.4″ IPS, 240×320, 8-bit parallel 8080                     |
| Input            | 36-key 6×6 matrix, Schottky anti-ghost, PIO+DMA scan       |
| Audio (Rev A)    | PDM mic + Class-D amp + SMT speaker — **Rev A only; removed in next revision.** Firmware has no audio task. |
| LoRa             | Semtech SX1262 + TCXO (Sub-GHz, mesh communication)        |
| GNSS             | ST Teseo-LIV3FL (multi-constellation)                      |
| Sensors          | 6-axis IMU + magnetometer + barometer                      |
| Battery          | Nokia BL-4C Li-ion ~890 mAh                                |
| Charging         | USB-C up to 2 A fast charge (configurable); OTG 5 V reverse power supported |

Full part numbers, packages, and design rules live in `hardware-requirements.md`.

## 6. Mechanical Requirements

- PCB dimensions: 66 × 105 mm.
- PCB stack-up: 6-layer (Signal / GND / Power / Power / GND / Signal).
- Antenna placement: LoRa chip antenna at top edge; GNSS chip antenna at left edge.

## 7. Mandatory Hardware Requirements

The following constraints must be satisfied at the system level.
Implementation details are specified in `docs/requirements/hardware-requirements.md`.

1. **Debug and recovery access** — The PCB must always provide physically accessible
   SWD debug and ROM bootloader recovery interfaces, independent of firmware state.

2. **I2C address conflict prevention** — Device addresses on each I2C bus must be unique.
   Address-configurable parts must be strapped to avoid conflicts at the PCB level.

3. **Battery safety** — The system must provide hardware-level battery protection
   (over-voltage, over-current, over-discharge, short-circuit, thermal cutoff)
   independent of firmware. Protection must not rely solely on software.

4. **EMC** — The design must address conducted and radiated emissions (RE), immunity (RI),
   and ESD on all external interfaces. Design measures include:
   - RF shielding enclosure over the digital core
   - ESD protection components on USB-C, SWD, and RF interfaces
   - High-speed digital signals routed on inner copper layers to reduce radiated emissions

5. **Signal integrity** — RF and high-speed digital signals must maintain controlled
   impedance throughout the signal path. Design measures include:
   - 50 Ω impedance-controlled traces for LoRa and GNSS RF paths
   - Layout practices to avoid crosstalk between digital and RF signal pairs
   - U.FL test connector on the RF path to allow bench verification of RF matching

## 8. Related Documents

- `hardware-requirements.md` — full BOM (part numbers, packages), per-subsystem
  component tables, and mandatory design rules.
- `docs/design-notes/power-architecture.md` — power tree and rail definitions.
- `docs/design-notes/rf-matching.md` — LoRa / GNSS RF frontend details.
- `docs/design-notes/mcu-gpio-allocation.md` — GPIO pin map and I2C bus allocation.
