# Hardware Requirements Specification

**Project:** MokyaLora (Project MS-RP2350 — Meshtastic Feature Phone)
**Version:** v5.2

---

## 1. Core Compute

| Role | Part | Package | Notes |
|------|------|---------|-------|
| MCU | RP2350B | QFN-80 | Dual-core Cortex-M33, 1.8 V logic, no built-in flash |
| Flash | Winbond W25Q128JW | WSON-8 | 16 MB, 1.8 V, QSPI |
| PSRAM | AP Memory APS6404L-3SQN | QFN | 8 MB, 1.8 V, QSPI — holds IME Trie index and UI double-buffer at runtime |
| MCU Crystal | Abracon ABM8-272-T3 | SMD | 12.000 MHz |

**Design constraints:**
- QSPI Flash and PSRAM must be placed adjacent to MCU; 33 Ω series resistors on CLK lines.
- Expose QSPI Flash CS to a test point or micro switch — anti-brick: forces ROM bootloader (UF2 mode).
- Expose SWCLK, SWDIO, GND test points — required for dual-core AMP debugging with a hardware debugger.

---

## 2. Power System

### 2.1 Components

| Role | Part | Package | Notes |
|------|------|---------|-------|
| Charger / Path | TI BQ25622RYKR | DSBGA-18 | I2C, 16-bit ADC, VINDPM (solar MPPT), OTG 5 V boost; **no D+/D−** to preserve MCU USB signal integrity |
| Buck Inductor | Murata DFE322520F-1R5M | 3225 | 1.5 µH, metal-alloy shielded — lower ripple than 1.0 µH, better for RF-sensitive design |
| Fuel Gauge | TI BQ27441DRZR | SON-12 | Impedance Track™; must be in series with battery current path |
| 1.8 V Buck | TI TPS62840DLCR | VSON-8 | 60 nA Iq — minimises standby current |
| 3.3 V LDO | TI TPS7A2033 | WSON-6 | 300 mA, ultra-low noise (high PSRR) — clean supply for GPS/RF analogue |
| LED / BL Driver | TI LM27965 | WQFN-24 | Dual-bank I2C dimming (I2C1); Bank A = LCD BL, Bank B = keypad BL; HWEN tied to 1.8 V |
| Battery | Nokia BL-4C | — | Li-ion 3.7 V, ~890 mAh |
| Battery Connector | AVX 009155003301006 | 3-pin pogo | 2.5 mm pitch; polarity marked on silkscreen |

### 2.2 Power Rail Summary

| Rail | Voltage | Regulator | Key Loads |
|------|---------|-----------|-----------|
| 1.8 V | 1.8 V | TPS62840 | MCU, Flash, PSRAM, SX1262, Mic, LCD logic, GPS I/O |
| 3.3 V | 3.3 V | TPS7A2033 | GPS RF, LCD analogue, magnetometer, barometer |
| VSYS | ~3.5–4.2 V | BQ25622 | Buck input, LDO input, audio amp, backlight, motor |
| OTG | 5 V | BQ25622 boost | USB VBUS reverse power output |

See `docs/design-notes/power-architecture.md` for the full power tree.

**Design constraints:**
- Charger ILIM: hardware resistor sets 500 mA default; MCU raises limit to 1–2 A via I2C after USB enumeration.
- BQ25622 PMID bulk capacitor must be placed close to IC; VBUS trace rated for ≥ 2 A.
- BQ27441 SRX–VSS sense path must be short and wide (in series with battery current loop).
- TPS7A2033 EN pin tied to VSYS / 1.8 V (always-on).

---

## 3. RF — LoRa

### 3.1 Components

| Role | Part | Package | Notes |
|------|------|---------|-------|
| LoRa Transceiver | Semtech SX1262 | QFN-24 | SPI1; supports 150–960 MHz sub-GHz bands |
| TCXO | ECS-TXO-20CSMV4-320-AY-TR | SMD | 32 MHz, 1.8 V, ±0.5 ppm, clipped sine output |
| RF Switch | Murata PE4259 | SPDT | Tx / Rx antenna path switching |
| LoRa Antenna | Kyocera AVX M620720 | Chip | Placed at top PCB edge |
| RF Shield | Wurth 3600213120S | SMT frame + lid | Covers digital core to reduce radiated emissions |

### 3.2 SPI1 Interface

| Signal | GPIO | Notes |
|--------|------|-------|
| LORA_MISO | GPIO 24 | SPI1 |
| LORA_nCS | GPIO 25 | SPI1 chip select |
| LORA_SCK | GPIO 26 | SPI1 |
| LORA_MOSI | GPIO 27 | SPI1 |
| LORA_BUSY | GPIO 28 | Active-high; poll before each SPI transaction |
| LORA_DIO1 | GPIO 29 | IRQ / DORMANT wakeup source |
| LORA_nRST | GPIO 23 | Active-low reset |

**Design constraints:**
- RF traces: 50 Ω controlled impedance on dedicated RF layer.
- TCXO XTA pin: 220 Ω series resistor + 10 pF shunt capacitor — mandatory per Semtech application note.
- U.FL test connector on RF signal path for bench RF matching and power verification.

---

## 4. RF — GNSS

### 4.1 Components

| Role | Part | Package | Notes |
|------|------|---------|-------|
| GNSS Receiver | ST Teseo-LIV3FL | LGA-14 | Multi-constellation; I2C0, address 0x3A |
| LNA | Infineon BGA725L6 | — | 14 dB gain, NF 0.9 dB |
| SAW Filter | Qualcomm B39162B4327P810 | — | L1 bandpass, after LNA |
| GNSS Antenna | Kyocera AVX M830120 | Chip | Placed at left PCB edge |

**Design constraints:**
- Protocol Select pin must be configured for I2C mode.
- RF trace: 50 Ω controlled impedance.
- 3.3 V supply (TPS7A2033) for RF and analogue sections.
- SDA/SCL share I2C0 pull-ups with other sensors.

---

## 5. Display

### 5.1 Components

| Role | Part | Package | Notes |
|------|------|---------|-------|
| LCD | Newhaven NHD-2.4-240320AF | — | 2.4″ IPS, 240×320, 8-bit parallel 8080, no touch |
| FPC Connector | Molex 54132-4062 | FPC-40 | 0.5 mm pitch ZIF, 40-pin |

### 5.2 Interface

| Signal | GPIO | Notes |
|--------|------|-------|
| TFT_nCS | GPIO 10 | Chip select |
| TFT_DCX | GPIO 11 | Data / command select |
| TFT_nWR | GPIO 12 | Write strobe |
| TFT_D8–D15 | GPIO 13–20 | 8-bit parallel data bus (PIO) |
| TFT_nRST | GPIO 21 | Reset |
| TFT_TE | GPIO 22 | Tearing effect |
| Backlight | LM27965 Bank A | I2C1 dimming |

**Design constraints:**
- FPC connector: insert FPC with contacts facing **down**; verify orientation before locking ZIF.

---

## 6. Audio

### 6.1 Components

| Role | Part | Package | Notes |
|------|------|---------|-------|
| Microphone | Infineon IM69D130 | LGA-5 | PDM, SNR 69 dB, AOP 128 dBSPL; bottom-port |
| Audio Amplifier | Nuvoton NAU8315YG | WSON-6 | 3.2 W Class-D I2S |
| Speaker | CUI CMS-131304-SMT-TR | SMT | 0.7 W, 8 Ω |

### 6.2 Interface

| Signal | GPIO | Notes |
|--------|------|-------|
| MIC_CLK | GPIO 4 | PDM clock output to mic |
| MIC_DATA | GPIO 5 | PDM data input from mic |
| AMP_BCLK | GPIO 30 | I2S bit clock to amp |
| AMP_FSR | GPIO 31 | I2S frame sync to amp |
| AMP_DAC | GPIO 32 | I2S data output to amp |

**Design constraints:**
- IM69D130 is a bottom-port microphone: PCB must have an acoustic opening aligned to the mic inlet; enclosure must seal the acoustic cavity with a rubber gasket.
- NAU8315 software must limit output to −3 dB to protect the speaker from over-drive.

---

## 7. Sensors

### 7.1 Components

| Role | Part | Package | I2C0 Address | Address Setting |
|------|------|---------|-------------|-----------------|
| IMU (ACC + GYRO) | ST LSM6DSV16X | LGA-14 | 0x6A | SA0 → GND (default 0x6B conflicts with charger) |
| Magnetometer | ST LIS2MDL | LGA-12 | 0x1E | Fixed |
| Barometer | ST LPS22HH | LGA-10 | 0x5D | SA0 → 3.3 V (Rev A confirmed; schematic ties SA0 to 3.3 V) |

All sensors share **I2C0 (GPIO 34 / 35)** with the GNSS receiver.

**Design constraints:**
- LSM6DSV16X SA0 must be tied to GND — default address 0x6B conflicts with BQ25622 on I2C1.
- LPS22HH SA0 is tied to 3.3 V → address 0x5D (confirmed Rev A bring-up).
- I2C0 pull-ups: 2.2 kΩ–4.7 kΩ to 1.8 V on SDA and SCL.

---

## 8. Human Interface

### 8.1 Keypad

| Role | Part | Notes |
|------|------|-------|
| Keypad Switches | SWT0105 | Metal dome sheet |
| Anti-ghost Diodes | Diodes Inc. SDM03U40 | SOD-523 Schottky, Vf ~0.37 V @ 30 mA; enables NKRO at 1.8 V logic |
| Keypad BL LEDs | LiteOn LTST-C191TBWET (×6–8) | White; driven by LM27965 Bank B (I2C1) |

**Scan mechanism:** RP2350 PIO drives 6 column lines (GPIO 36–41), samples 6 row lines (GPIO 42–47). DMA writes key state directly to RAM — zero CPU overhead. Logic level 1.8 V, active-low.

#### Physical Layout (6×6 Matrix)

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

#### Electrical Matrix Map

| Row \ Col | Col 0 | Col 1 | Col 2 | Col 3 | Col 4 | Col 5 (ext) |
|-----------|-------|-------|-------|-------|-------|-------------|
| Row 0 | 1 2 / ㄅㄉ / ANS | 3 4 / ˇˋ / 7 | 5 6 / ㄓˊ / 8 | 7 8 / ˙ㄚ / 9 | 9 0 / ㄞㄢ / ÷ | FUNC |
| Row 1 | Q W / ㄆㄊ / ( | E R / ㄍㄐ / 4 | T Y / ㄔㄗ / 5 | U I / ㄧㄛ / 6 | O P / ㄟㄣ / × | SET |
| Row 2 | A S / ㄇㄋ / ) | D F / ㄎㄑ / 1 | G H / ㄕㄘ / 2 | J K / ㄨㄜ / 3 | L / ㄠㄤ / - | BACK |
| Row 3 | Z X / ㄈㄌ / AC | C V / ㄏㄒ / 0 | B N / ㄖㄙ / . | M / ㄩㄝ / x10ˣ | — / ㄡㄥ / + | DEL |
| Row 4 | MODE | TAB | SPACE | ，SYM | 。.？ | VOL+ |
| Row 5 (nav) | UP | DOWN | LEFT | RIGHT | OK | VOL− |

### 8.2 Indicators & Haptics

| Role | Part | Notes |
|------|------|-------|
| Status LED | LiteOn LTST-C235KGKRKT | Side-view, red/green; driven via LM27965 remaining channels; 256-step I2C dimming |
| Vibration Motor | HD-EMB1104-SM-2 | SMD ERM, 3 V, 80 mA |
| Motor MOSFET | Toshiba SSM3K56ACT | N-ch, low Vth, 1.8 V logic compatible, CST3; low-side driver |
| Motor Protection | Flyback diode | Reverse-parallel across motor terminals to absorb back-EMF |

**Design constraints:**
- 1.8 V GPIO rule: never drive LEDs or high-Vth MOSFETs directly from GPIO. Use SSM3K56ACT (low-Vth).
  - LED: common-anode to VSYS / 3.3 V; cathode through resistor to Drain; 100 kΩ pull-down on Gate.
  - Motor: low-side drive; motor+ to VSYS (5–10 Ω series); motor− to Drain.
- Flyback diode reverse-parallel across motor terminals is mandatory.

---

## 9. Connectivity — USB-C

| Role | Part | Package | Notes |
|------|------|---------|-------|
| USB-C Connector | GCT USB4105-GF-A | 16-pin mid-mount | Through-hole shell legs must be soldered for mechanical retention |

**Design constraints:**
- CC1 and CC2 must each have an **independent** 5.1 kΩ pull-down to GND — enables PD charger 5 V output.
- D+/D− connected to MCU only — not to charger IC (preserves USB signal integrity).
- ESD protection devices on D+/D− and VBUS.
- VBUS trace rated for ≥ 2 A.

---

## 10. I2C Bus Allocation

| Device | Part | 7-bit Addr | Bus | GPIO | Address Setting |
|--------|------|-----------|-----|------|-----------------|
| Charger | BQ25622 | 0x6B | I2C1 | GPIO 6 / 7 | Fixed |
| Fuel Gauge | BQ27441 | 0x55 | I2C1 | GPIO 6 / 7 | Fixed |
| LED Driver | LM27965 | 0x36 | I2C1 | GPIO 6 / 7 | Fixed; nets BKLT_SCL / BKLT_SDA |
| IMU | LSM6DSV16X | 0x6A | I2C0 | GPIO 34 / 35 | SA0 → GND |
| Magnetometer | LIS2MDL | 0x1E | I2C0 | GPIO 34 / 35 | Fixed |
| Barometer | LPS22HH | 0x5D | I2C0 | GPIO 34 / 35 | SA0 → 3.3 V |
| GNSS | Teseo-LIV3FL | 0x3A | I2C0 | GPIO 34 / 35 | Fixed |

Pull-up: 2.2 kΩ–4.7 kΩ to 1.8 V on SDA and SCL for both buses.

---

## 11. Mandatory Design Rules

1. **Anti-brick boot point:** expose QSPI Flash CS to a test point or micro switch — allows ROM bootloader entry when software is bricked.
2. **SWD debug interface:** expose SWCLK, SWDIO, GND — required for dual-core AMP debugging.
3. **1.8 V GPIO rule:** never drive LEDs or high-Vth MOSFETs directly. Use SSM3K56ACT (low-Vth) as switch.
   - LED: common-anode to VSYS / 3.3 V; cathode through resistor to Drain; 100 kΩ pull-down on Gate.
   - Motor: low-side drive; motor+ to VSYS (5–10 Ω series); motor− to Drain.
4. **OTG wiring:** BQ25622 PMID bulk capacitor must be close to IC; VBUS trace rated for ≥ 2 A.
5. **Fuel gauge path:** BQ27441 SRX–VSS sense path in battery current loop; trace must be short and wide.
6. **Acoustic seal:** IM69D130 is bottom-port; PCB must have an opening aligned to mic inlet; enclosure must seal with a rubber gasket.
7. **Signal integrity:** QSPI CLK lines need 33 Ω series resistors; Flash and PSRAM placed adjacent to MCU.
8. **Motor protection:** flyback diode reverse-parallel across motor terminals is mandatory.
9. **GPS I2C mode:** Teseo-LIV3FL Protocol Select pin must be configured for I2C mode.
10. **USB-C CC pins:** CC1 and CC2 must each have an independent 5.1 kΩ pull-down to GND to enable PD charger 5 V output.
