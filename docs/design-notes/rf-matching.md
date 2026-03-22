# RF Matching and Frontend Design

**Project:** MokyaLora
**Revision:** Rev 5.2

---

## 1. LoRa RF Path — SX1262

### 1.1 Block Diagram

```
SX1262 ──► RF Switch (PE4259) ──► LoRa Chip Antenna (M620720)
              │
              └──► TX path (PA output)
              └──► RX path (LNA input)
```

### 1.2 TCXO Interface — ECS-TXO-20CSMV4

The SX1262 requires a 32 MHz clock reference. A TCXO (Temperature-Compensated Crystal Oscillator) is used instead of a passive crystal for frequency stability.

**Mandatory coupling network on XTA pin:**

```
TCXO Output ──[ 220 Ω ]──┬──[ 10 pF ]── GND
                          │
                        SX1262 XTA
```

- **220 Ω series resistor:** limits drive current and optimises phase noise per Semtech application note.
- **10 pF shunt capacitor:** filters high-frequency noise on the clock input.

> **Note:** ECS-TXO-20CSMV4 is a clipped-sine output type. The coupling network is mandatory; omitting it degrades receiver sensitivity.

### 1.3 RF Switch — Murata PE4259 (SPDT)

- Controls PA/LNA path switching for the SX1262.
- Driven by SX1262 DIO2 (configured as RF switch control in firmware).
- Insertion loss: typ. 0.6 dB @ 900 MHz.

### 1.4 LoRa Chip Antenna — Kyocera AVX M620720

- Type: ceramic chip antenna.
- Placement: at PCB corner, away from ground plane, per manufacturer keep-out.
- PCB tuning stub may be required depending on board dielectric; verify with network analyser.

### 1.5 RF Shield

- Cover: Wurth 3600213120S (SMT frame + lid).
- Covers SX1262, TCXO, RF switch, and matching network.
- Solder frame to PCB before placing lid.

---

## 2. GNSS RF Path — Teseo-LIV3FL

### 2.1 Block Diagram

```
GNSS Chip Antenna (M830120) ──► SAW Filter (B39162B4327P810) ──► LNA (BGA725L6) ──► Teseo-LIV3FL RF_IN
```

### 2.2 SAW Filter — Qualcomm B39162B4327P810

- Bandpass filter centred on L1 GPS/GNSS band (1575.42 MHz).
- Rejects out-of-band interference (cellular, LoRa).
- Must be placed immediately after the antenna feed point.

### 2.3 LNA — Infineon BGA725L6

- Low-noise amplifier for GPS front-end.
- Noise figure: typ. 0.9 dB.
- Gain: typ. 14 dB.
- Supply: 1.8 V (Rail A) or 3.3 V depending on variant — verify from datasheet.
- Place close to SAW filter output; minimise trace length.

### 2.4 GNSS Chip Antenna — Kyocera AVX M830120

- Placement: PCB corner diagonally opposite the LoRa antenna.
- Requires ground-plane clearance as specified in the antenna datasheet.

---

## 3. Antenna Placement Rules

| Antenna    | Location       | Min. Keep-out from Ground Plane |
|------------|----------------|---------------------------------|
| LoRa M620720 | PCB corner A  | Per antenna datasheet           |
| GPS M830120  | PCB corner B  | Per antenna datasheet           |

- LoRa and GPS antennas must be at opposite corners to minimise mutual coupling.
- No copper pour under antenna keep-out area on any layer.

---

## 4. RF Layout Guidelines

1. **50 Ω traces** on all RF signal paths (calculate width based on PCB stack-up and dielectric).
2. **Solid ground plane** under RF components on Layer 2 (GND).
3. **Via fence** along RF traces to prevent ground noise coupling.
4. **Minimise trace length** from SX1262 RF pins to the RF switch.
5. **QSPI CLK** (GPIO 14/15 area, QSPI dedicated pins): place 33 Ω series resistors on CLK; Flash and PSRAM must be adjacent to MCU to minimise trace length.

---

## 5. Open Items

- [ ] Confirm M620720 matching network values on target 4-layer stack-up.
- [ ] Verify BGA725L6 supply voltage selection.
- [ ] Perform antenna isolation measurement (LoRa vs. GPS) on Rev A board.
