# System Requirements Specification

**Project:** MokyaLora (Project MS-RP2350 — Meshtastic Feature Phone)
**Version:** v5.1

---

## 1. Project Overview

This project develops a standalone Meshtastic handheld terminal based on the Raspberry Pi RP2350B.
The goal is fully phone-free long-range mesh communication.
The device integrates a dedicated 36-key physical matrix keyboard, phone-grade Chinese predictive input, a calculator function, and a USB data-terminal mode.

## 2. Core Design Philosophy

- **Dual-core AMP split:** leverages the RP2350B dual-core architecture.
  - **Core 0:** Meshtastic protocol, LoRa radio, power management.
  - **Core 1:** UI rendering, Input Method Engine (IME), application logic.
- **1.8 V system logic:** unified 1.8 V I/O with Dormant sleep for maximum battery life.
- **Virtualised architecture:** software isolation keeps the standard Meshtastic firmware on Core 0, preserving upstream compatibility.

## 3. Operating Modes

The device supports two mutually exclusive modes, selected by the user on USB insertion:

**Mode A — Standalone / Charge-Only**
- The device operates as a handheld radio.
- USB state: power only (VBUS charging, no USB enumeration).
- Features: full operation — LoRa Tx/Rx, GPS, display, 36-key input.

**Mode B — USB Data Terminal**
- The device acts as a LoRa modem for a PC or phone.
- USB state: CDC Serial enumerated; PC sees a virtual COM port.
- Features: Core 1 bridges USB data to Core 0 transparently.

## 4. System-Level Specification Summary

| Parameter        | Value / Part                                                |
|------------------|-------------------------------------------------------------|
| MCU              | Raspberry Pi RP2350B (QFN80)                               |
| Flash            | Winbond W25Q128JW — 16 MB, 1.8 V, QSPI                    |
| PSRAM            | AP Memory APS6404L-3SQN — 8 MB, 1.8 V, QSPI               |
| Display          | Newhaven NHD-2.4-240320AF — 2.4″ IPS, 240×320, 8-bit 8080 |
| Input            | 36-key 6×6 matrix (SDM03U40 anti-ghost diodes, PIO+DMA)    |
| Audio            | IM69D130 PDM mic + NAU8315 3 W Class-D amp + CMS-131304    |
| LoRa             | Semtech SX1262 (SPI1) + ECS-TXO-20CSMV4 TCXO              |
| GNSS             | ST Teseo-LIV3FL (I2C0, 0x3A)                               |
| Sensors          | LSM6DSV16X IMU, LIS2MDL mag, LPS22HH baro (I2C0)           |
| Battery          | Nokia BL-4C Li-ion ~890 mAh                                |
| Charging         | USB-C 1 A fast charge; OTG 5 V reverse power supported     |

## 5. Mechanical Requirements

- Target width: < 65 mm (one-hand grip).
- PCB stack-up: 4-layer (Signal / GND / Power / Signal).
- Antenna placement: LoRa and GPS chip antennas at opposite PCB corners.

## 6. Mandatory Hardware Requirements

The following must not be omitted from the PCB:

1. **Anti-brick boot point** — expose QSPI Flash CS to a test point or tactile switch.
   Shorting this at power-on forces the RP2350 ROM bootloader (UF2 mode).

2. **SWD debug interface** — expose SWCLK, SWDIO, GND (2.54 mm header or test points).
   Required for dual-core AMP debugging with a hardware debugger.

3. **I2C address strapping**
   - LSM6DSV16X SA0 → GND (address 0x6A; avoids conflict with BQ25620 at 0x6B).
   - LPS22HH SA0 → GND (address 0x5C).

4. **LDO enable strategy** — TPS7A2033 EN pin tied to VSYS/1.8 V (always-on).

## 7. Key BOM Highlights

| Function            | Part Number          | Package       |
|---------------------|----------------------|---------------|
| MCU                 | RP2350B              | QFN-80        |
| Flash               | W25Q128JW            | WSON-8        |
| PSRAM               | APS6404L-3SQN        | BGA / QFN     |
| Charger             | TI BQ25622RWMR       | WQFN-18       |
| Fuel Gauge          | TI BQ27441-G1A       | SON-12        |
| 1.8 V Buck          | TI TPS62840DLCR      | VSON-8        |
| 3.3 V LDO           | TI TPS7A2033         | WSON-6        |
| LED Driver          | TI LM27965           | WQFN-24       |
| LoRa Transceiver    | Semtech SX1262       | QFN-24        |
| TCXO (LoRa)         | ECS-TXO-20CSMV4-320  | SMD           |
| GPS                 | ST Teseo-LIV3FL      | LGA-14        |
| IMU                 | ST LSM6DSV16X        | LGA-14        |
| Magnetometer        | ST LIS2MDL           | LGA-12        |
| Barometer           | ST LPS22HH           | LGA-10        |
| Microphone          | Infineon IM69D130    | LGA-5         |
| Audio Amp           | Nuvoton NAU8315YG    | WSON-6        |
| Speaker             | CUI CMS-131304-SMT-TR| SMT           |
| LoRa Ant            | Kyocera AVX M620720  | Chip          |
| GPS Ant             | Kyocera AVX M830120  | Chip          |
| RF Switch           | Murata PE4259        | SPDT          |
| USB-C               | GCT USB4105-GF-A     | 16-pin        |
| Battery Conn        | AVX 009155003301006  | 3-pin pogo    |
| Keypad Diodes       | SDM03U40             | SOD-523       |
| Keypad Switches     | SWT0105              | Metal dome    |
| Motor               | HD-EMB1104-SM-2      | SMD ERM       |
| Motor MOSFET        | Toshiba SSM3K56ACT   | CST3          |
