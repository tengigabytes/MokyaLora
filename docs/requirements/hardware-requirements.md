# Hardware Requirements Specification

**Project:** MokyaLora (Project MS-RP2350 — Meshtastic Feature Phone)
**Version:** v5.1

---

## 1. Component Selection (BOM)

### 1.1 Core Compute & Storage

| Role   | Part                  | Package  | Notes                                          |
|--------|-----------------------|----------|------------------------------------------------|
| MCU    | RP2350B               | QFN-80   | Dual-core Cortex-M33, 1.8 V logic, no built-in flash |
| Flash  | Winbond W25Q128JW     | WSON-8   | 16 MB, 1.8 V, QSPI                            |
| PSRAM  | AP Memory APS6404L-3SQN | QFN   | 8 MB, 1.8 V, QSPI — holds IME Trie index and UI double-buffer at runtime |

### 1.2 Power System (TI Total Solution)

| Role              | Part                   | Package   | Notes                                                        |
|-------------------|------------------------|-----------|--------------------------------------------------------------|
| Charger / Path    | TI BQ25622RWMR         | WQFN-18   | I2C, 16-bit ADC, VINDPM (solar MPPT), OTG 5 V boost; **no D+/D−** to preserve MCU USB signal integrity |
| Buck inductor     | Murata DFE322520F-1R5M | 3225      | 1.5 µH, metal-alloy shielded — lower ripple than 1.0 µH, better for RF-sensitive design |
| Fuel Gauge        | TI BQ27441DRZR-G1A     | SON-12    | Impedance Track™; must be in series with battery current path; G1A supports 4.2 V Li-ion |
| 1.8 V Buck        | TI TPS62840DLCR        | VSON-8    | 60 nA Iq — minimises standby current                        |
| 3.3 V LDO         | TI TPS7A2033           | WSON-6    | 300 mA, ultra-low noise (high PSRR) — clean supply for GPS/RF analogue |
| LED / BL Driver   | TI LM27965             | WQFN-24   | Dual-bank I2C dimming; HWEN tied to 1.8 V (always enabled); Bank A = LCD BL, Bank B = keypad BL |

**Charger ILIM:** hardware resistor sets 500 mA default; MCU raises limit to 1–2 A via I2C after USB enumeration.

### 1.3 Audio & Sensors

| Role           | Part                      | Package  | Notes                                                   |
|----------------|---------------------------|----------|---------------------------------------------------------|
| Microphone     | Infineon IM69D130         | LGA-5    | PDM, SNR 69 dB, AOP 128 dBSPL; bottom-port — PCB opening + gasket required |
| Audio Amp      | Nuvoton NAU8315YG         | WSON-6   | 3.2 W Class-D I2S; software must limit output to −3 dB to protect speaker |
| Speaker        | CUI CMS-131304-SMT-TR     | SMT      | 0.7 W, 8 Ω, micro SMT                                  |
| LoRa           | Semtech SX1262            | QFN-24   | SPI1 interface                                          |
| GNSS           | ST Teseo-LIV3FL           | LGA-14   | I2C0, address 0x3A; configure Protocol Select pin for I2C mode |
| IMU            | ST LSM6DSV16X             | LGA-14   | I2C0, address **0x6A** — SA0 must be tied to GND (default 0x6B conflicts with BQ25620) |
| Magnetometer   | ST LIS2MDL                | LGA-12   | I2C0, address 0x1E (fixed)                             |
| Barometer      | ST LPS22HH                | LGA-10   | I2C0, address 0x5C — SA0 must be tied to GND           |
| LCD            | Newhaven NHD-2.4-240320AF | FPC-40   | 2.4″ IPS, 240×320, 8-bit parallel 8080, no touch       |
| LCD Connector  | Molex 54132-4062          | FPC-40   | 0.5 mm pitch ZIF, 40-pin                               |

### 1.4 I2C Address Allocation

| Device       | Part           | 7-bit Addr | Bus  | Address Pin Setting                              |
|--------------|----------------|------------|------|--------------------------------------------------|
| Charger      | BQ25620        | 0x6B       | I2C1 | Fixed                                            |
| Fuel Gauge   | BQ27441-G1A    | 0x55       | I2C1 | Fixed                                            |
| IMU          | LSM6DSV16X     | **0x6A**   | I2C0 | **SA0/SDO → GND** (default 0x6B conflicts with charger) |
| Magnetometer | LIS2MDL        | 0x1E       | I2C0 | Fixed                                            |
| Barometer    | LPS22HH        | 0x5C       | I2C0 | SA0 → GND                                       |
| GPS          | Teseo-LIV3FL   | 0x3A       | I2C0 | Fixed                                            |
| LED Driver   | LM27965        | 0x36       | I2C0 | I2C backlight controller                        |

### 1.5 Indicators & Haptics

| Role              | Part                     | Notes                                                          |
|-------------------|--------------------------|----------------------------------------------------------------|
| Status LED        | LiteOn LTST-C235KGKRKT   | Side-view, red/green; driven via LM27965 remaining channels; 256-step I2C dimming |
| Vibration Motor   | HD-EMB1104-SM-2          | SMD ERM, 3 V, 80 mA                                          |
| Motor MOSFET      | Toshiba SSM3K56ACT       | N-ch, low Vth, 1.8 V logic compatible, CST3; low-side driver  |
| Motor Protection  | Flyback diode (e.g. 1N4148) | Reverse-parallel across motor terminals to absorb back-EMF  |

### 1.6 RF Front-End & Mechanical

| Role              | Part                         | Notes                                             |
|-------------------|------------------------------|---------------------------------------------------|
| LoRa Antenna      | Kyocera AVX M620720          | Chip antenna                                      |
| LoRa RF Switch    | Murata PE4259                | SPDT                                              |
| LoRa TCXO         | ECS-TXO-20CSMV4-320-AY-TR   | 32 MHz, 1.8 V, ±0.5 ppm, clipped sine            |
| GNSS Antenna      | Kyocera AVX M830120          | Chip antenna                                      |
| GNSS SAW Filter   | Qualcomm B39162B4327P810     | L1 bandpass                                       |
| GNSS LNA          | Infineon BGA725L6            | 14 dB gain, NF 0.9 dB                            |
| Keypad Switches   | SWT0105                      | Metal dome sheet                                  |
| Keypad BL LEDs    | LiteOn LTST-C191TBWET (×6–8)| White, driven by LM27965 Bank B                  |
| Keypad Diodes     | Diodes Inc. SDM03U40         | SOD-523 Schottky, Vf ~0.37 V @ 30 mA; anti-ghosting at 1.8 V |
| USB-C Connector   | GCT USB4105-GF-A             | 16-pin mid-mount; CC1 & CC2 each need independent 5.1 kΩ pull-down to GND |
| Battery           | Nokia BL-4C                  | Li-ion 3.7 V, ~890 mAh                           |
| Battery Connector | AVX 009155003301006          | 3-pin pogo, 2.5 mm pitch                         |
| MCU Crystal       | Abracon ABM8-272-T3          | 12.000 MHz                                        |
| RF Shield Cover   | Wurth 3600213120S            | SMT frame + lid                                   |

---

## 2. Power Architecture

See `docs/design-notes/power-architecture.md` for the full power tree.

### Summary

| Rail   | Voltage | Regulator   | Key Loads                                         |
|--------|---------|-------------|---------------------------------------------------|
| 1.8 V  | 1.8 V   | TPS62840    | MCU, Flash, PSRAM, SX1262, Mic, LCD logic, GPS I/O |
| 3.3 V  | 3.3 V   | TPS7A2033   | GPS RF, LCD analogue, magnetometer, barometer     |
| VSYS   | ~3.5–4.2 V | BQ25620  | Buck input, LDO input, audio amp, backlight, motor |
| OTG    | 5 V     | BQ25620 boost | USB VBUS reverse power output                  |

---

## 3. GPIO Pin Map

See `docs/design-notes/mcu-gpio-allocation.md` for the full table.

### Critical Design Notes

1. **TCXO coupling (XTA pin):** 220 Ω series resistor + 10 pF shunt capacitor — mandatory per Semtech app note.
2. **QSPI routing:** Flash and PSRAM must be adjacent to MCU; place 33 Ω series resistors on CLK lines.
3. **I2C pull-ups:** I2C0 (GPIO 40/41) needs 2.2 kΩ–4.7 kΩ to 1.8 V.

---

## 4. Keypad Input System

### 4.1 Scan Mechanism

- **Controller:** RP2350 PIO drives 6 column lines, samples 6 row lines.
- **DMA integration:** key state written directly to RAM — zero CPU overhead.
- **Logic level:** 1.8 V, active-low.
- **Anti-ghosting:** SDM03U40 Schottky diodes on all 36 keys (NKRO at 1.8 V).

### 4.2 Physical Layout (6×6 Matrix)

```
[ Navigation & Control Area ]

      (Left)        (D-Pad)        (Right)      (Vol)
    +--------+    +    +    +    +--------+  +--------+
    |  FUNC  |    |    ▲    |    |   SET  |  |  VOL+  |
    +--------+    +    |    +    +--------+  +--------+
                  +--+ | +--+
             ◄ -- |  | OK |  | -- ►
                  +--+ | +--+
    +--------+    +    |    +    +--------+  +--------+
    |  BACK  |    |    ▼    |    |   DEL  |  |  VOL-  |
    +--------+    +    +    +    +--------+  +--------+

[ Core Input Area (5×5) ]

       Col 0       Col 1       Col 2       Col 3       Col 4
    +-------+   +-------+   +-------+   +-------+   +-------+
 R0 | 1 2   |   | 3 4   |   | 5 6   |   | 7 8   |   | 9 0   |
    | ㄅㄉ  |   | ˇˋ    |   | ㄓˊ   |   | ˙ㄚ   |   | ㄞㄢㄦ |
    | [ANS] |   | [7]   |   | [8]   |   | [9]   |   | [÷]   |
    +-------+   +-------+   +-------+   +-------+   +-------+
 R1 | Q W   |   | E R   |   | T Y   |   | U I   |   | O P   |
    | ㄆㄊ  |   | ㄍㄐ  |   | ㄔㄗ  |   | ㄧㄛ  |   | ㄟㄣ  |
    | [(]   |   | [4]   |   | [5]   |   | [6]   |   | [×]   |
    +-------+   +-------+   +-------+   +-------+   +-------+
 R2 | A S   |   | D F   |   | G H   |   | J K   |   |   L   |
    | ㄇㄋ  |   | ㄎㄑ  |   | ㄕㄘ  |   | ㄨㄜ  |   | ㄠㄤ  |
    | [)]   |   | [1]   |   | [2]   |   | [3]   |   | [-]   |
    +-------+   +-------+   +-------+   +-------+   +-------+
 R3 | Z X   |   | C V   |   | B N   |   |   M   |   |  ---  |
    | ㄈㄌ  |   | ㄏㄒ  |   | ㄖㄙ  |   | ㄩㄝ  |   | ㄡㄥ  |
    | [AC]  |   | [0]   |   | [.]   |   | [x10ˣ]|   | [+]   |
    +-------+   +-------+   +-------+   +-------+   +-------+
 R4 | MODE  |   |  TAB  |   | SPACE |   | ，SYM |   | 。.？ |
    +-------+   +-------+   +-------+   +-------+   +-------+
```

### 4.3 Electrical Matrix Map

| Row \ Col   | Col 0          | Col 1        | Col 2        | Col 3         | Col 4         | Col 5 (ext) |
|-------------|----------------|--------------|--------------|---------------|---------------|-------------|
| Row 0 (R0)  | 1 2 / ㄅㄉ / ANS | 3 4 / ˇˋ / 7 | 5 6 / ㄓˊ / 8 | 7 8 / ˙ㄚ / 9 | 9 0 / ㄞㄢ / ÷ | FUNC       |
| Row 1 (R1)  | Q W / ㄆㄊ / (  | E R / ㄍㄐ / 4 | T Y / ㄔㄗ / 5 | U I / ㄧㄛ / 6 | O P / ㄟㄣ / × | SET        |
| Row 2 (R2)  | A S / ㄇㄋ / )  | D F / ㄎㄑ / 1 | G H / ㄕㄘ / 2 | J K / ㄨㄜ / 3 | L / ㄠㄤ / -   | BACK       |
| Row 3 (R3)  | Z X / ㄈㄌ / AC | C V / ㄏㄒ / 0 | B N / ㄖㄙ / . | M / ㄩㄝ / x10ˣ | — / ㄡㄥ / + | DEL        |
| Row 4 (R4)  | MODE           | TAB          | SPACE        | ，SYM         | 。.？          | VOL+       |
| Row 5 (nav) | UP             | DOWN         | LEFT         | RIGHT         | OK            | VOL−       |

---

## 5. Mandatory Design Rules

1. **Anti-brick boot point:** expose QSPI Flash CS to a test point or micro switch — allows ROM bootloader entry when software is bricked.
2. **SWD debug interface:** expose SWCLK, SWDIO, GND — required for dual-core AMP debugging.
3. **1.8 V GPIO rule:** never drive LEDs or high-Vth MOSFETs directly. Use SSM3K56ACT (low-Vth) as switch.
   - LED: common-anode to VSYS/3.3 V; cathode through resistor to Drain; 100 kΩ pull-down on Gate.
   - Motor: low-side drive; motor+ to VSYS (5–10 Ω series); motor− to Drain.
4. **OTG wiring:** BQ25620 PMID bulk capacitor must be close to IC; VBUS trace rated for ≥ 2 A.
5. **Fuel gauge path:** BQ27441 SRX–VSS sense path in battery current loop; trace must be short and wide.
6. **Acoustic seal:** IM69D130 is bottom-port; PCB must have an opening and the enclosure must use a rubber gasket to seal the acoustic cavity.
7. **Signal integrity:** QSPI CLK lines need 33 Ω series resistors; Flash and PSRAM placed adjacent to MCU.
8. **Motor protection:** flyback diode reverse-parallel across motor terminals is mandatory.
9. **GPS I2C:** Teseo-LIV3FL Protocol Select pin must be configured for I2C mode; SDA/SCL share I2C0 pull-ups.
10. **USB-C CC pins:** CC1 and CC2 must each have an **independent** 5.1 kΩ pull-down to GND to enable PD charger 5 V output.
