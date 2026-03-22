# Software Requirements Specification

**Project:** MokyaLora (Project MS-RP2350 — Meshtastic Feature Phone)
**Version:** v5.1

---

## 1. Software Architecture & Resource Planning

### 1.1 Dual-Core AMP Architecture

| Core   | Role              | Responsibilities                                        |
|--------|-------------------|---------------------------------------------------------|
| Core 0 | Modem Worker      | Meshtastic protocol stack, LoRa radio                              |
| Core 1 | UI Host & IME     | FreeRTOS + LVGL, Input Method Engine (IME), UI rendering, power management |

**Framework:** Arduino-Pico + FreeRTOS + LVGL

### 1.2 Flash Memory Map (16 MB — W25Q128JW)

| Address Range        | Size | Contents                                              |
|----------------------|------|-------------------------------------------------------|
| `0x0000_0000`        | 2 MB | Core 0 firmware (Meshtastic base)                    |
| `0x0020_0000`        | 2 MB | Core 1 firmware (UI app & IME logic)                 |
| `0x0040_0000`        | 4 MB | OTA buffer / factory recovery image                  |
| `0x0080_0000`        | 4 MB | Assets: language packs (Trie tree), 16×16 bitmap fonts, offline maps, icons |
| `0x00C0_0000`        | 4 MB | LittleFS — user settings, message DB, node DB        |

### 1.3 PSRAM Allocation (8 MB — APS6404L)

| Region       | Size | Contents                                               |
|--------------|------|--------------------------------------------------------|
| IME Runtime  | 4 MB | Language index, dictionary, error-correction weights — loaded from Flash at boot |
| UI & Heap    | 4 MB | LVGL double-buffer, application heap                  |

### 1.4 Stability Mechanisms

- **Hardware Watchdog (WDT):** Core 1 UI task feeds the watchdog. If UI deadlocks for > 8 s, the system resets.
- **Safe Mode:** if WDT reset occurs more than 3 times within 10 s of boot, enter safe mode — USB Serial only, radio and UI disabled.

---

## 2. HAL Driver Classes

### `ChargerManager` — TI BQ25620 (I2C1, 0x6B)

- **Init:** read ILIM pin state; maintain hardware current limit (500 mA) by default.
- **Dynamic current:** raise IINDPM to 1–2 A via I2C after USB enumeration or user enables fast-charge.
- **Monitoring:** periodically read ADC registers — VBUS voltage, VBAT voltage, IBAT current, TS temperature.
- **OTG control:** `enableOTG(bool on)` — activates / deactivates 5 V boost for reverse power output.
- **Interrupt:** handle GPIO 8 (PWR_INT, open-drain); update UI on VBUS insertion/removal and charge completion.

### `GaugeMonitor` — TI BQ27441-G1A (I2C1, 0x55)

- **Init:** set Design Capacity (1000 mAh) and Design Energy.
- **API:** `getSoC()`, `getAverageCurrent()`, `getSOH()`.
- **Remaining time:** compute `Time-to-Empty` from `getAverageCurrent()`.

### `EnvironmentSensor` — LSM6DSV16X, LIS2MDL, LPS22HH (I2C0)

| Driver  | Address | Function                                          |
|---------|---------|---------------------------------------------------|
| IMU     | 0x6A    | 6-axis attitude; gesture detection (raise-to-wake)|
| Mag     | 0x1E    | Electronic compass heading                        |
| Baro    | 0x5C    | Barometric altitude                               |

- **Fusion:** Kalman filter or Madgwick algorithm for 9-DOF data fusion.

### `AudioPipeline` — IM69D130 (PDM) + NAU8315 (I2S)

- **PDM driver:** PIO-based PDM sampling → 16-bit PCM conversion.
- **DSP:** software high-pass filter to remove wind-noise low-frequency components.
- **Codec2:** encode clean speech for LoRa voice transmission.
- **Speaker protection:** limit NAU8315 output gain to −3 dB to stay within CMS-131304 (0.7 W) rating.

### `GPSDriver_I2C` — ST Teseo-LIV3FL (I2C0, 0x3A)

- **Transport:** I2C0 polling mode.
- **Init:** disable unused NMEA sentences (keep RMC, GGA only); set 1 Hz update rate.
- **Buffer:** ring buffer emulating a serial stream for Meshtastic core consumption.

### `StatusController` — LM27965 LED Driver (I2C0, 0x36)

- **Priority state machine:** Error > Charging > Message > Idle.
- **LED patterns:**
  - `CHARGING` → red solid
  - `CHARGED` → green solid
  - `MESSAGE` → orange breathing (PWM mix of red + green)
  - `ERROR` → red fast blink

### `HapticFeedback` — Motor via PWM (GPIO 9)

- **Voltage compensation:** read VBAT from BQ25620 ADC; dynamically adjust PWM duty cycle to keep average motor voltage at ~3.0 V (avoid overvoltage from VSYS at 4.2 V).
- **Patterns:** `Click` (short), `Buzz` (long), `Alarm` (SOS rhythm).

---

## 3. Power Management State Machine

### States

| State       | Screen | CPU Clock | LoRa       | Trigger                                |
|-------------|--------|-----------|------------|----------------------------------------|
| ACTIVE      | ON     | 133 MHz   | Rx/Tx      | Any key press                          |
| STANDBY     | OFF    | 48 MHz    | Duty-cycle | 30 s idle; any key wakes               |
| DORMANT     | OFF    | OSC OFF   | Standby / OFF | Manual power-off or battery < 15 % |

### STANDBY Behaviour
- LVGL rendering halted; only keypad scan and LoRa packet monitoring remain active.
- LCD `Sleep In` command sent; backlight off.

### DORMANT Wakeup Sources

| Source              | GPIO    |
|---------------------|---------|
| Power button        | GPIO 33 |
| LoRa interrupt      | GPIO 29 (SX1262 DIO1) |
| USB insertion       | GPIO 1  |

### Wakeup Sequence

1. IRQ detected → CPU clock restored → peripherals reinitialised → wakeup reason checked.
2. **LoRa IRQ:** read packet → store in DB → display notification (or vibrate only) → return to STANDBY.
3. **Power key:** turn on screen → return to ACTIVE.

---

## 4. UI / UX Design

### 4.1 Interaction Model

Classic feature-phone navigation (Nokia / BlackBerry style):

| Key      | Action                                        |
|----------|-----------------------------------------------|
| FUNC     | Open context menu for current page            |
| BACK     | Go back; long-press → home                   |
| OK       | Confirm / enter / view detail                 |
| PWR      | Short: toggle standby; long: power menu       |
| 0–9      | Quick-jump to app in grid view                |

### 4.2 Visual Theme Engine

| Theme           | Description                                    |
|-----------------|------------------------------------------------|
| High Contrast   | White background, black bold text — sunlight readable |
| Tactical Night  | Pure black, red/green text, minimal brightness |

**Status bar (always visible):** left — LoRa RSSI, GPS satellite count; right — unread messages, battery, time.
**Toasts:** bottom pop-up notifications (e.g. "Sent", "GPS locked") — auto-dismiss after 3 s.

### 4.3 App Sitemap

#### Home (Dashboard)
Grid menu or list layout (user preference). Core apps:
1. Messages
2. Nodes (Contacts)
3. Map
4. Compass / GPS
5. Settings
6. Status

#### Messages
- **Thread list:** sorted by update time; unread shown bold with red dot.
- **Chat view:** bubble layout; own messages right-aligned with delivery ticks (sending / delivered to mesh / read); others left-aligned with sender name.
- **Composer:** T9 bilingual (Chinese + English) input; FUNC key inserts GPS coords, canned messages, or telemetry.

#### Map & Navigation
- **Radar view:** no map file needed; concentric rings show relative bearing and distance of nearby nodes. Useful for SAR and low-visibility situations.
- **Offline map:** vector data from Flash; supports zoom; shows own track.

#### Node Manager
- List all mesh nodes; set custom aliases; add to ignore list; view telemetry history (battery, altitude, SNR).
- Start a direct message from node detail view.

#### Settings
Full configuration without a phone app:
- **Radio:** frequency preset, custom frequency, TX power.
- **Channels:** manage channels, generate/display PSK QR codes.
- **Display:** brightness, sleep timeout, theme, 180° rotation.
- **Notifications:** silent / vibrate / ring, LED colour.
- **System:** reset NodeDB, format filesystem, firmware version.
- **Power Expert:** OTG toggle, charge speed (500 mA / 1 A), live ADC readings (VBUS, IBAT, PCB temp, SOH %).

---

## 5. Input Method Engine — MokyaInput Engine (MIE)

MIE is developed as a self-contained sub-library under `firmware/mie/`.
See `docs/design-notes/mie-architecture.md` for the full architectural design note.

### 5.1 Sub-project Structure

| Layer       | Module        | Location                    | Reusability    |
|-------------|---------------|-----------------------------|----------------|
| Data        | Data Pipeline | `firmware/mie/tools/`       | Standalone tool|
| Core        | Trie-Searcher | `firmware/mie/src/`         | Full           |
| Logic       | IME-Logic     | `firmware/mie/src/`         | Partial        |
| Adaptation  | HAL-Port      | `firmware/mie/hal/`         | Platform-specific |

### 5.2 Data Assets (generated, not committed)

| File               | Contents                                         | Budget    |
|--------------------|--------------------------------------------------|-----------|
| `font_glyphs.bin`  | 8,104 × 16×16 monochrome glyphs (32 B/glyph)    | ~260 KB   |
| `font_index.bin`   | `(codepoint, offset)` lookup table               | ~65 KB    |
| `dict_dat.bin`     | Double-Array Trie base[] + check[] arrays        | ≤ 2 MB    |
| `dict_values.bin`  | Per-key word list with frequency weights         | ≤ 2 MB    |

Total PSRAM budget for MIE runtime: **4 MB** (DAT + values loaded from Flash at boot).
Font glyphs remain in Flash and are accessed on demand during rendering.

### 5.3 Input Modes (cycled via MODE key)

| Mode                  | Description                                                           |
|-----------------------|-----------------------------------------------------------------------|
| Bopomofo Auto         | Consonant-first prediction — type `ㄐ ㄊ`, IME predicts 「今天」     |
| English Auto          | Word-prediction based on English vocabulary                          |
| Alphanumeric Manual   | Traditional multi-tap — for passwords and exact commands             |
| Calculator            | Full calculator UI; OK key acts as `=`; supports floats, brackets    |

### 5.4 Smart Correction

- **Spatial correction:** candidate scoring penalised by physical key adjacency distance.
- **Phonetic fuzzy match:** near-homophone Bopomofo syllables accepted as valid input.
- **Dynamic weighting:** candidate rank updated from per-session input history (stored in LittleFS).

### 5.5 Internationalisation

- IME-Logic and language data packs are fully decoupled.
- Supports Latin-script languages (EN / FR / DE / ES) and syllabic scripts (Japanese kana / Korean / Bopomofo).
- Language pack loaded from Flash into PSRAM on language switch.

### 5.6 Shortcuts

| Shortcut                        | Action                              |
|---------------------------------|-------------------------------------|
| Long-press `0`                  | Flashlight toggle                   |
| Long-press `1`                  | Speed dial                          |
| FUNC + OK (hold 5 s)            | Send SOS distress signal            |
| Long-press MODE (in text field) | Pop-up calculator widget            |

### 5.7 MIE Development Roadmap

**Phase 1 — PC environment & validation**
- [ ] `gen_font.py`: extract 8,104 glyphs from GNU Unifont; verify output.
- [ ] `gen_dict.py`: compile MoE word list to DAT binary; validate on PC.
- [ ] `Trie-Searcher`: implement and unit-test DAT search in C++ on PC.
- [ ] `IME-Logic`: implement Bopomofo de-ambiguation; test with simulated key sequences.

**Phase 2 — Hardware integration (Rev A)**
- [ ] `hal/rp2350/`: bridge PIO+DMA key buffer to `mie::KeyEvent`.
- [ ] Boot loader: copy DAT + values from Flash to PSRAM; measure search latency.
- [ ] Display: render `font_glyphs.bin` via LVGL custom font driver on NHD 2.4″.
- [ ] UI: integrate candidate bar widget with Trie-Searcher output.

**Phase 3 — Optimisation & extension**
- [ ] Spatial + phonetic fuzzy correction.
- [ ] User-defined word list in LittleFS, merged into DAT at runtime.
- [ ] Additional language pack slots.
