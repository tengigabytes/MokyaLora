# Software Requirements Specification

**Project:** MokyaLora (Project MS-RP2350 — Meshtastic Feature Phone)
**Version:** v5.2

---

## 1. Software Architecture & Resource Planning

### 1.1 Dual-Core AMP Architecture

| Core   | Role              | Responsibilities                                                                     |
|--------|-------------------|--------------------------------------------------------------------------------------|
| Core 0 | Modem Worker      | Meshtastic protocol stack, LoRa radio                                                |
| Core 1 | UI Host & IME     | FreeRTOS + LVGL, Input Method Engine (IME), UI rendering, power management, all I2C drivers |

**Frameworks:**
- **Core 0:** Arduino-Pico (Meshtastic base) — cooperative `OSThread` scheduler (`concurrency::Scheduler`)
- **Core 1:** FreeRTOS + LVGL — preemptive task scheduler

**GPS bridge:** Core 1 reads Teseo-LIV3FL (I2C0, 0x3A) and forwards NMEA position data to Core 0
via the IPC ring buffer. Meshtastic's `PositionModule` on Core 0 consumes GPS data from IPC,
not directly from I2C. Core 0 never accesses any I2C bus.

### 1.2 Flash Memory Map (16 MB — W25Q128JW)

| Address Range        | Size | Contents                                                                              |
|----------------------|------|---------------------------------------------------------------------------------------|
| `0x0000_0000`        | 2 MB | Core 0 firmware (Meshtastic base)                                                    |
| `0x0020_0000`        | 2 MB | Core 1 firmware (UI app & IME logic)                                                 |
| `0x0040_0000`        | 4 MB | OTA buffer / factory recovery image                                                   |
| `0x0080_0000`        | 4 MB | Assets: language packs (Trie tree), 16×16 bitmap fonts, offline maps, icons          |
| `0x00C0_0000`        | 4 MB | LittleFS — user settings, message DB, node DB                                        |

### 1.3 PSRAM Allocation (8 MB — APS6404L)

| Region          | Core   | Size            | Contents                                                              |
|-----------------|--------|-----------------|-----------------------------------------------------------------------|
| IME Runtime     | Core 1 | 4 MB            | Language index, DAT trie, dictionary — loaded from Flash at boot      |
| UI & Heap       | Core 1 | 4 MB            | LVGL double framebuffer (~300 KB) + application heap                  |
| Core 0 reserve  | Core 0 | 0 MB (reserved) | Available for StoreAndForward cache or large NodeDB; not required for normal operation |

**Core 0 SRAM:** Meshtastic fits within RP2350B's 520 KB internal SRAM (~160–180 KB used for
code, stack, NodeDB, packet queues). PSRAM is not required for Core 0 under normal operation.
Both cores can access PSRAM via the shared QMI/QSPI bus if required in future.

### 1.4 Stability Mechanisms

- **Hardware Watchdog (WDT):** Core 1 UI task feeds the watchdog. If UI deadlocks for > 8 s, the system resets.
- **Safe Mode:** if WDT reset occurs more than 3 times within 10 s of boot, enter safe mode — USB Serial only, radio and UI disabled.

### 1.5 Inter-Core Communication (IPC)

#### License Boundary and Isolation Model

The dual-core partition maps directly to a dual-license boundary:

| Domain | Firmware | License |
|--------|----------|---------|
| Core 0 | Meshtastic base + `IPCPhoneAPI` adapter | GPL-3.0 |
| Core 1 | FreeRTOS UI, HAL drivers, MIE | Apache-2.0 |
| `firmware/shared/ipc/` | IPC protocol header only | MIT — zero GPL dependency |

The IPC boundary must operate as a **communication protocol** (analogous to a UART or network
socket), not as a C/C++ library link. Core 1 must have no compile-time or link-time dependency
on any Meshtastic header or object. This prevents GPL-3.0 from propagating to Core 1.

**Hard rules for `firmware/shared/ipc/ipc_protocol.h`:**

- Include only `<stdint.h>` and `<stddef.h>` — no Meshtastic headers, no project headers.
- No C++ classes, templates, or namespaces.
- **No pointers in any struct field** — Core 1 must never dereference Core 0 heap memory, and vice versa.
- All structs must be Plain Old Data (POD) with fixed-size fields only.
- Payload is always a fixed-size `uint8_t payload[IPC_MAX_PAYLOAD]` byte array; the sender
  serialises application data into it, the receiver deserialises. No Meshtastic objects cross
  the boundary.
- Message/command IDs are plain `#define` constants or `typedef enum` with an explicit `uint8_t` base.

**Canonical POD frame — the only IPC data structure:**

```c
/* firmware/shared/ipc/ipc_protocol.h */
#include <stdint.h>
#define IPC_MAX_PAYLOAD 256

typedef struct {
    uint8_t  msg_id;              /* IPC_MSG_* or IPC_CMD_* constant */
    uint8_t  reserved;
    uint16_t length;              /* valid bytes in payload[]; <= IPC_MAX_PAYLOAD */
    uint8_t  payload[IPC_MAX_PAYLOAD];
} ipc_frame_t;                    /* sizeof == 260 bytes, no padding surprises */
```

**Prohibited patterns:**

```c
/* PROHIBITED — exposes Meshtastic internal type across the boundary */
#include "mesh/MeshPacket.h"
typedef struct { MeshPacket *pkt; } ipc_frame_t;

/* PROHIBITED — C++ linkage; Core 1 build must not link to Core 0 objects */
class IPCFrame { ... };

/* PROHIBITED — direct cross-core symbol reference */
extern NodeDB g_nodeDB;   /* Core 1 must not reference any Core 0 symbol */
```

#### Bus and Peripheral Ownership

| Bus / Peripheral          | Owner  | Devices / Notes                                            |
|---------------------------|--------|------------------------------------------------------------|
| I2C0 (GPIO 34 / 35)       | Core 1 | LSM6DSV16X, LIS2MDL, LPS22HH, Teseo-LIV3FL                |
| I2C1 (GPIO 6 / 7)         | Core 1 | BQ25622, BQ27441, LM27965                                  |
| SPI1 (GPIO 24–27)         | Core 0 | SX1262 LoRa transceiver                                    |
| PIO — keypad scan         | Core 1 | 6×6 matrix, GPIO 36–47                                     |
| PIO — audio               | Core 1 | IM69D130 PDM (GPIO 4/5), NAU8315 I2S (GPIO 30–32)         |
| PWM — motor               | Core 1 | MTR_PWM GPIO 9                                             |

Core 0 exclusively owns SPI1 and LoRa-related GPIOs. All I2C buses are owned by Core 1.
No register or memory of any Core 0-owned peripheral may be accessed by Core 1, and vice versa.

#### IPC Mechanism

- **Transport:** two SPSC (Single-Producer Single-Consumer) lock-free ring buffers in a dedicated
  ~32 KB shared SRAM region; one per direction. Each slot holds one `ipc_frame_t` (260 bytes).
- **Doorbell:** RP2350 hardware FIFO (8 × 32-bit words, one per core) used as a wake signal only;
  the HW FIFO word carries only a frame count, not data. Ring buffer carries the actual payload.
- **Memory ordering:** `__dmb()` (Data Memory Barrier) on ring buffer head/tail writes before
  writing the doorbell.
- **Serialisation:** all application data (protobuf bytes, NMEA sentences, config fields) is
  serialised into `ipc_frame_t.payload[]` by the sender and deserialised by the receiver.
  No Meshtastic object references or pointers are present anywhere in shared SRAM.
- **GPS double-buffer:** a `uint8_t gps_buf[2][128]` array in shared SRAM holds the latest NMEA
  sentence as plain bytes. Defined in `ipc_protocol.h` as a POD array. Core 1 writes, Core 0
  reads. Core 0 never calls any GPS I2C driver — it only reads this buffer.

```
Core 0 → Core 1 : c0_to_c1  SPSC ring buffer  (ipc_frame_t slots, events / notifications)
Core 1 → Core 0 : c1_to_c0  SPSC ring buffer  (ipc_frame_t slots, commands)
Both directions : HW FIFO doorbell (32-bit frame-count word, non-blocking)
GPS data        : gps_buf uint8_t[2][128] in shared SRAM (Core 1 writes, Core 0 reads)
```

**Boot handshake:** Core 0 initialises the ring buffers and `gps_buf`, then signals Core 1 via
HW FIFO (`IPC_BOOT_READY`) before Core 1 begins UI init. Core 1 must not touch IPC structures
before receiving this signal.

#### IPC Message Types

All payloads are serialised byte sequences. No Meshtastic types or pointers appear in any payload.

| Direction | Message Type              | Payload contents (serialised into `payload[]`)              |
|-----------|---------------------------|-------------------------------------------------------------|
| C0 → C1   | `IPC_MSG_PACKET_RECEIVED` | Meshtastic `Data` protobuf bytes (text, position, telemetry) |
| C0 → C1   | `IPC_MSG_PACKET_SENT`     | `uint32_t` packet ID (4 bytes)                              |
| C0 → C1   | `IPC_MSG_NODE_UPDATED`    | Serialised node record: node ID, SNR, battery %, lat/lon    |
| C0 → C1   | `IPC_MSG_NODE_LIST`       | One or more serialised node records (response to `IPC_CMD_GET_NODE_LIST`) |
| C0 → C1   | `IPC_MSG_GPS_UPDATE`      | Doorbell only — NMEA sentence already written to `gps_buf`  |
| C0 → C1   | `IPC_MSG_RADIO_STATUS`    | `int8_t` RSSI, `int8_t` SNR, `uint8_t` channel utilisation % |
| C0 → C1   | `IPC_MSG_LOG_LINE`        | UTF-8 log string (null-terminated, ≤ 255 chars)             |
| C1 → C0   | `IPC_CMD_SEND_TEXT`       | ToRadio protobuf bytes                                      |
| C1 → C0   | `IPC_CMD_SEND_POSITION`   | `double` lat, `double` lon, `float` alt (serialised)        |
| C1 → C0   | `IPC_CMD_SET_CONFIG`      | Meshtastic `Config` protobuf bytes                          |
| C1 → C0   | `IPC_CMD_GET_NODE_LIST`   | Empty payload — requests NodeDB dump as `IPC_MSG_NODE_LIST` frames |
| C1 → C0   | `IPC_CMD_POWER_STATE`     | `uint8_t` state: 0 = ACTIVE, 1 = STANDBY, 2 = DORMANT      |
| C1 → C0   | `IPC_CMD_REBOOT`          | Empty payload                                               |
| C1 → C0   | `IPC_CMD_FACTORY_RESET`   | Empty payload                                               |

#### CMake Build Isolation

The CMake build system enforces the license boundary at the linker level:

```
firmware/
├── core0/           CMakeLists.txt  — GPL-3.0 target
│   links: meshtastic_base, pico-sdk, pico_multicore, ipc_protocol (INTERFACE)
├── core1/           CMakeLists.txt  — Apache-2.0 target
│   links: freertos, lvgl, mie, pico-sdk, pico_multicore, ipc_protocol (INTERFACE)
├── mie/             CMakeLists.txt  — MIT static library (no Pico SDK dependency)
└── shared/ipc/      CMakeLists.txt  — INTERFACE header-only target, zero library deps
    provides: ipc_protocol header to both cores
```

**Rules:**
- `core1` target must **not** `target_link_libraries` to any `core0` static library or object file.
- `core0` target must **not** `target_link_libraries` to any `core1` static library or object file.
- `ipc_protocol` is a CMake `INTERFACE` (header-only) target; its only dependency is `<stdint.h>`.
- `ipc_protocol` is the **only** shared compile-time dependency between `core0` and `core1`.
- The top-level `add_executable` that packages both images for the Pico SDK linker is the sole
  permitted point of cross-core linkage in the build graph.

#### Meshtastic PhoneAPI Porting

`PhoneAPI` (`meshtastic/firmware/src/mesh/PhoneAPI.h`) is Meshtastic's abstract transport interface.
A new subclass `IPCPhoneAPI` replaces the USB/BLE transport with IPC ring buffer writes.
This is the **only structural addition to Meshtastic** on Core 0:

| Meshtastic Component             | Adaptation                                                         |
|----------------------------------|--------------------------------------------------------------------|
| `PhoneAPI`                       | Subclassed as `IPCPhoneAPI` — serialises into `ipc_frame_t`, writes to `c0_to_c1` ring buffer |
| `GPSDriver`                      | Replaced by GPS adapter that reads `gps_buf[]` (plain bytes, no I2C) |
| `SerialAPI` / `BLEApi`           | Excluded from Core 0 build                                         |
| `Router`, `MeshService`, all Modules | **No changes**                                               |
| `NodeDB` / `config`              | Core 1 requests data via `IPC_CMD_GET_NODE_LIST` / `IPC_CMD_SET_CONFIG`; Core 0 serialises and replies. No direct cross-core memory access. |
| `OSThread` / `Scheduler`         | Runs entirely on Core 0, no changes                                |

#### Intent Declaration

`firmware/shared/ipc/ipc_protocol.h` must carry the following header comment:

```c
/*
 * ipc_protocol.h -- MokyaLora Inter-Core Communication Protocol
 *
 * This header defines a strict, decoupled communication boundary between
 * Core 0 (GPL-3.0 Meshtastic firmware) and Core 1 (Apache-2.0 UI firmware).
 * It operates purely via value-passing of primitive C types (POD structs).
 *
 * No internal data structures, pointers, C++ objects, or GPL-licensed symbols
 * are shared or linked across this boundary. This file is intentionally kept
 * free of any Meshtastic or project-specific includes so that it may be used
 * by Core 1 without creating a GPL-3.0 derived work.
 *
 * SPDX-License-Identifier: MIT
 */
```

---

## 2. HAL Driver Classes

All HAL drivers listed below run on **Core 1** and exclusively own their respective peripherals.

### `ChargerManager` — TI BQ25622 [Core 1 · I2C1 · 0x6B]

- **Init:** read ILIM pin state; maintain hardware current limit (500 mA) by default.
- **Dynamic current:** raise IINDPM to 1–2 A via I2C after USB enumeration or user enables fast-charge.
- **Monitoring:** periodically read ADC registers — VBUS voltage, VBAT voltage, IBAT current, TS temperature.
- **OTG control:** `enableOTG(bool on)` — activates / deactivates 5 V boost for reverse power output.
- **Interrupt:** handle GPIO 8 (PWR_INT, open-drain); update UI on VBUS insertion/removal and charge completion.

### `GaugeMonitor` — TI BQ27441DRZR [Core 1 · I2C1 · 0x55]

- **Init:** set Design Capacity (1000 mAh) and Design Energy.
- **API:** `getSoC()`, `getAverageCurrent()`, `getSOH()`.
- **Remaining time:** compute `Time-to-Empty` from `getAverageCurrent()`.

### `EnvironmentSensor` — LSM6DSV16X, LIS2MDL, LPS22HH [Core 1 · I2C0]

| Driver | Address | Function                                           |
|--------|---------|----------------------------------------------------|
| IMU    | 0x6A    | 6-axis attitude; gesture detection (raise-to-wake) |
| Mag    | 0x1E    | Electronic compass heading                         |
| Baro   | 0x5C    | Barometric altitude                                |

- **Fusion:** Kalman filter or Madgwick algorithm for 9-DOF data fusion.

### `AudioPipeline` — IM69D130 (PDM) + NAU8315 (I2S) [Core 1]

- **PDM driver:** PIO-based PDM sampling → 16-bit PCM conversion.
- **DSP:** software high-pass filter to remove wind-noise low-frequency components.
- **Codec2:** encode clean speech for LoRa voice transmission.
- **Speaker protection:** limit NAU8315 output gain to −3 dB to stay within CMS-131304 (0.7 W) rating.

### `GPSDriver_I2C` — ST Teseo-LIV3FL [Core 1 · I2C0 · 0x3A]

- **Transport:** I2C0 polling mode.
- **Init:** disable unused NMEA sentences (keep RMC, GGA only); set 1 Hz update rate.
- **IPC bridge:** parsed position fix written to `gps_double_buffer` (shared SRAM); doorbell sent
  to Core 0 via `IPC_MSG_GPS_UPDATE`. Meshtastic's `PositionModule` reads from this buffer,
  never directly from I2C.

### `StatusController` — LM27965 LED Driver [Core 1 · I2C1 · 0x36]

- **Priority state machine:** Error > Charging > Message > Idle.
- **LED patterns:**
  - `CHARGING` → red solid
  - `CHARGED` → green solid
  - `MESSAGE` → orange breathing (PWM mix of red + green)
  - `ERROR` → red fast blink

### `HapticFeedback` — Motor via PWM [Core 1 · GPIO 9]

- **Voltage compensation:** read VBAT from BQ25622 ADC; dynamically adjust PWM duty cycle to keep average motor voltage at ~3.0 V (avoid overvoltage from VSYS at 4.2 V).
- **Patterns:** `Click` (short), `Buzz` (long), `Alarm` (SOS rhythm).

---

## 3. Power Management State Machine

Power management runs on **Core 1**. Power state changes are communicated to Core 0 via
`IPC_CMD_POWER_STATE` so that Core 0 can adjust LoRa duty cycle and radio behaviour accordingly.

### States

| State   | Screen | CPU Clock | LoRa          | Trigger                                   |
|---------|--------|-----------|---------------|-------------------------------------------|
| ACTIVE  | ON     | 133 MHz   | Rx/Tx         | Any key press                             |
| STANDBY | OFF    | 48 MHz    | Duty-cycle Rx | 30 s idle; any key wakes                  |
| DORMANT | OFF    | OSC OFF   | Standby / OFF | Manual power-off or battery < 15 %        |

### STANDBY Behaviour

- Core 1 sends `IPC_CMD_POWER_STATE = STANDBY` to Core 0; Core 0 switches LoRa to duty-cycle Rx.
- LVGL rendering halted; only keypad scan and LoRa packet monitoring remain active.
- LCD `Sleep In` command sent; backlight off.

### DORMANT Wakeup Sources

| Source        | GPIO                       |
|---------------|----------------------------|
| Power button  | GPIO 33                    |
| LoRa interrupt | GPIO 29 (SX1262 DIO1)     |
| USB insertion | GPIO 1                     |

### Wakeup Sequence

1. IRQ detected → CPU clock restored → peripherals reinitialised → wakeup reason checked.
2. **LoRa IRQ:** Core 0 reads packet → sends `IPC_MSG_PACKET_RECEIVED` to Core 1 → Core 1 displays notification or vibrates → returns to STANDBY.
3. **Power key:** Core 1 turns on screen → sends `IPC_CMD_POWER_STATE = ACTIVE` to Core 0 → returns to ACTIVE.

---

## 4. UI / UX Design

### 4.1 Interaction Model

Classic feature-phone navigation (Nokia / BlackBerry style):

| Key  | Action                                        |
|------|-----------------------------------------------|
| FUNC | Open context menu for current page            |
| BACK | Go back; long-press → home                   |
| OK   | Confirm / enter / view detail                 |
| PWR  | Short: toggle standby; long: power menu       |
| 0–9  | Quick-jump to app in grid view                |

### 4.2 Visual Theme Engine

| Theme         | Description                                    |
|---------------|------------------------------------------------|
| High Contrast | White background, black bold text — sunlight readable |
| Tactical Night | Pure black, red/green text, minimal brightness |

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

| Layer      | Module        | Location                    | Reusability       |
|------------|---------------|-----------------------------|-------------------|
| Data       | Data Pipeline | `firmware/mie/tools/`       | Standalone tool   |
| Core       | Trie-Searcher | `firmware/mie/src/`         | Full              |
| Logic      | IME-Logic     | `firmware/mie/src/`         | Partial           |
| Adaptation | HAL-Port      | `firmware/mie/hal/`         | Platform-specific |

### 5.2 Data Assets (generated, not committed)

| File               | Contents                                         | Budget  |
|--------------------|--------------------------------------------------|---------|
| `font_glyphs.bin`  | 8,104 × 16×16 monochrome glyphs (32 B/glyph)    | ~260 KB |
| `font_index.bin`   | `(codepoint, offset)` lookup table               | ~65 KB  |
| `dict_dat.bin`     | Double-Array Trie base[] + check[] arrays        | ≤ 2 MB  |
| `dict_values.bin`  | Per-key word list with frequency weights         | ≤ 2 MB  |

Total PSRAM budget for MIE runtime: **4 MB** (DAT + values loaded from Flash at boot).
Font glyphs remain in Flash and are accessed on demand during rendering.

### 5.3 Input Modes (cycled via MODE key)

Three modes cycle in order: Bopomofo → English → Alphanumeric → (back to Bopomofo).

| # | Mode | Description |
|---|------|-------------|
| 0 | Bopomofo | Bopomofo syllable accumulation; IME predicts Traditional Chinese — type `ㄐ ㄊ` → 「今天」 |
| 1 | English Auto | Half-keyboard letter-pair expansion → frequency-ranked English word prediction |
| 2 | Alphanumeric | Multi-tap single character — consecutive presses of same key cycle primary/secondary character |

**Bopomofo disambiguation:** syllable position state machine (initial → medial → final → tone)
constrains which of the two phonemes on an ambiguous key is valid. Phase 1 uses the primary
phoneme only; full disambiguation is Phase 3.

**English prediction:** each key press produces two candidate letters; all valid prefix
combinations are searched against an English MIED dictionary; results are merged by frequency.

**Alphanumeric:** no dictionary lookup; `MultiTapState` tracks last key + consecutive tap count;
a different key (or timeout in Phase 2+) confirms the pending character.

### 5.4 Smart Correction

- **Spatial correction:** candidate scoring penalised by physical key adjacency distance.
- **Phonetic fuzzy match:** near-homophone Bopomofo syllables accepted as valid input.
- **Dynamic weighting:** candidate rank updated from per-session input history (stored in LittleFS).

### 5.5 Internationalisation

- IME-Logic and language data packs are fully decoupled.
- Supports Latin-script languages (EN / FR / DE / ES) and syllabic scripts (Japanese kana / Korean / Bopomofo).
- Language pack loaded from Flash into PSRAM on language switch.

### 5.6 Shortcuts

| Shortcut                     | Action                              |
|------------------------------|-------------------------------------|
| Long-press `0`               | Flashlight toggle                   |
| Long-press `1`               | Speed dial                          |
| FUNC + OK (hold 5 s)         | Send SOS distress signal            |
| Long-press MODE (in text field) | Quick symbol picker               |

### 5.7 MIE Development Roadmap

**Phase 1 — PC environment & validation**
- [x] `gen_font.py`: extract 8,104 glyphs from GNU Unifont; verify output.
      Script complete; requires Unifont `.hex` + `charlist_8104.txt` to produce `.bin` assets.
- [x] `gen_dict.py`: compile MoE word list to DAT binary; validate on PC.
      MIED sorted-index format implemented; requires MoE CSV to produce `.bin` assets.
- [x] `Trie-Searcher`: implement and unit-test DAT search in C++ on PC.
      Binary search over sorted key index; 13 GoogleTest cases passing.
- [x] `IME-Logic`: implement Bopomofo de-ambiguation; test with simulated key sequences.
      Phase 1 skeleton: primary-phoneme key map, mode FSM, REPL integration.
      Full disambiguation and smart correction are Phase 3 items.
- [x] Phase 1 wrap-up: three-mode design (Bopomofo / English / Alphanumeric); `Calculator`
      mode removed; MODE cycles `% 3`; mode-dispatch skeleton with stubs for English and
      Alphanumeric; `MultiTapState` struct added; all 14 unit tests passing.
- [ ] Phase 1 extension — Alphanumeric multi-tap: implement `process_alpha()`; consecutive
      same-key presses cycle primary/secondary character; confirm on different key.
- [ ] Phase 1 extension — English word prediction: `gen_en_dict.py` + English MIED dictionary;
      `ImeLogic` accepts second `TrieSearcher`; `process_english()` expands key pairs and
      merges prefix-search results by frequency.

**Phase 2 — Hardware integration (Rev A)**
- [ ] `hal/rp2350/`: bridge PIO+DMA key buffer to `mie::KeyEvent`.
- [ ] Boot loader: copy DAT + values from Flash to PSRAM; measure search latency.
- [ ] Display: render `font_glyphs.bin` via LVGL custom font driver on NHD 2.4″.
- [ ] UI: integrate candidate bar widget with Trie-Searcher output.

**Phase 3 — Optimisation & extension**
- [ ] Spatial + phonetic fuzzy correction.
- [ ] User-defined word list in LittleFS, merged into DAT at runtime.
- [ ] Additional language pack slots.
