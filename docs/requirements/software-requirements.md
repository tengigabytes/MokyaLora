# Software Requirements Specification

**Project:** MokyaLora (Project MS-RP2350 — Meshtastic Feature Phone)
**Version:** v5.2

This document specifies **what** the firmware must do — user-visible behaviour,
performance targets, interoperability guarantees. It deliberately avoids
implementation detail.

For **how** the firmware is built (memory map, IPC byte layout, build system,
boot sequence, cross-image non-interference), see
[`docs/design-notes/firmware-architecture.md`](../design-notes/firmware-architecture.md)
(hereafter **FA**), which is the single source of truth for implementation.

For hardware specs (BOM, GPIO map, power tree), see
[`docs/requirements/system-requirements.md`](system-requirements.md) and
[`docs/requirements/hardware-requirements.md`](hardware-requirements.md).

---

## 1. Software Architecture Overview

MokyaLora runs as **two independent AMP images** on the RP2350B dual-core MCU:

| Core   | Role              | Responsibilities                                                                        |
|--------|-------------------|-----------------------------------------------------------------------------------------|
| Core 0 | Modem Worker      | Meshtastic protocol stack, LoRa radio, GNSS polling (via shared buffer)                 |
| Core 1 | UI Host & IME     | FreeRTOS + LVGL, Input Method Engine, UI rendering, power management, all I2C drivers   |

Licence boundary: Core 0 is **GPL-3.0** (Meshtastic), Core 1 is **Apache-2.0**,
and the only shared compile-time surface is `firmware/shared/ipc/ipc_protocol.h`
(**MIT**, POD types only, no C++, no pointers). Implementation details and the
rationale for why this keeps the two licences separable are in FA §1 and §11.

### 1.1 Stability Requirements

- **Watchdog:** the UI path must feed a hardware watchdog. If the UI deadlocks
  for more than **8 s**, the system must reset.
- **Safe Mode:** if the watchdog fires more than **3 times within 10 s of boot**,
  the firmware must enter a minimal safe mode — USB Serial only, radio and UI
  disabled — until manually reset.

Implementation: FA §9.4 (Watchdog Discipline & Safe Mode).

### 1.2 Inter-Core Communication — Functional Requirements

The firmware must support the following cross-core interactions. The transport
(ring buffer layout, message IDs, payload structs, doorbell mechanism) is
defined normatively by `firmware/shared/ipc/ipc_protocol.h` and documented in
FA §5.

**Core 0 → Core 1 notifications:** received text, node updates, device status
(battery, LoRa RSSI/SNR, GPS fix), TX ACK / failure, channel configuration
changes, CLI byte stream, config value replies, reboot-imminent notice.

**Core 1 → Core 0 commands:** send text, set channel, set TX power, request
status, set node alias, change power state, reboot, factory reset, get/set/commit
config.

**GPS data path:** Core 1 owns the Teseo-LIV3FL on the sensor I2C bus, parses
NMEA, and writes sentences into a shared-SRAM double-buffer. Core 0 reads the
buffer; Core 0 must not touch any I2C bus.

**Boot handshake:** Core 0 must initialise IPC structures before signalling
Core 1 to begin UI init. Core 1 must not access IPC state before receiving this
signal.

**Licence hygiene:** no Meshtastic type, pointer, or C++ object may cross the
IPC boundary. Payloads are POD byte sequences.

---

## 2. HAL Driver Classes

All HAL drivers run on **Core 1** (see FA §7 for bus ownership and GPIO
assignment; see `docs/design-notes/mcu-gpio-allocation.md` for the GPIO map).

### `ChargerManager` — TI BQ25622

- **Init:** honour the hardware ILIM pin state as the default current limit
  (500 mA).
- **Dynamic current:** raise IINDPM to 1–2 A via I2C after USB enumeration or
  when the user enables fast-charge.
- **Monitoring:** periodically read VBUS voltage, VBAT voltage, IBAT current,
  and TS temperature from the ADC.
- **OTG control:** `enableOTG(bool on)` — activate / deactivate the 5 V boost
  for reverse power output.
- **Interrupt:** on charger IRQ assertion (open-drain), update UI for VBUS
  insertion / removal and charge completion.

### `GaugeMonitor` — TI BQ27441

- **Init:** set Design Capacity (1000 mAh) and Design Energy for the Nokia BL-4C
  pack.
- **API:** `getSoC()`, `getAverageCurrent()`, `getSOH()`.
- **Remaining time:** compute Time-to-Empty from average current draw.

### `EnvironmentSensor` — LSM6DSV16X + LIS2MDL + LPS22HH

| Driver | Function                                           |
|--------|----------------------------------------------------|
| IMU    | 6-axis attitude; gesture detection (raise-to-wake) |
| Mag    | Electronic compass heading                         |
| Baro   | Barometric altitude                                |

- **Fusion:** Kalman filter or Madgwick algorithm for 9-DOF data fusion.

### `GPSDriver_I2C` — ST Teseo-LIV3FL

- **Transport:** polling-mode I2C on the sensor bus (shared with IMU, Mag, Baro).
  Driver lives in its own FreeRTOS task (`gps_task`, priority tskIDLE+2, 2 KB
  stack) because the 100 ms drain cadence and line-accumulator parser state do
  not fit the unified sensor tick.
- **NMEA sentence set:** RMC + GGA + GSV + GSA kept **always on**. Runtime
  mask switching is not implemented — bringup's `$PSTMSAVEPAR`-committed NVM
  config already enables this set plus GLONASS and Galileo. Rationale: avoids a
  state machine for per-screen mask toggling; total bandwidth at 10 Hz is
  ~3 KB/s, trivial on a 400 kHz bus.
- **Adjustable fix rate:** driver exposes
  `teseo_set_fix_rate(GNSS_RATE_{OFF,1,2,5,10}HZ)`. The native ODR is the ceiling
  for any consumer — device drives NMEA at `1 / period`, consumers read the
  latest `teseo_get_state()` / `teseo_get_sat_view()` snapshot at whatever
  cadence they want. Typical usage:
  - Meshtastic position publish: 10 s – 60 s (background default; 1 Hz ceiling)
  - UI satellite/speed screen: 10 Hz while mounted, back to 1 Hz on unmount
  - Deep-sleep background: `GNSS_RATE_OFF` (`$PSTMGPSSUSPEND` — engine parked)
  Policy (who requests what rate) is caller-owned; the driver does not
  implement reference-counting in M3.4.5d.
- **NVM policy:** driver never writes `$PSTMSAVEPAR`. Rate changes are RAM-only
  (`$PSTMSETPAR,1303,<period>,0`) so the 10k NVM write cycle budget is preserved
  for bringup / commissioning paths.
- **State snapshots:**
  - `teseo_state_t` — fix, lat/lon (×1e7), alt, speed, course, HDOP, UTC
    time/date, sentence count, fail count. `lat_e7`/`lon_e7` chosen to feed
    `IpcPayloadDeviceStatus` directly without conversion.
  - `teseo_sat_view_t` — up to 32 satellites pooled from all talkers
    (GP/GL/GA/BD), each with PRN + elevation + azimuth + SNR, plus an
    `update_count` tick for UI staleness detection.
- **Output:** M3.4.5d **does not yet populate `IpcGpsBuf`**. Core 0 still has no
  GPS feed; Core 1 owns the snapshot. Wiring `IpcGpsBuf` (double-buffer, atomic
  flip, no doorbell per FA §5.4) is deferred to a later milestone alongside
  Core 0's position adapter.

### `StatusController` — LM27965 LED Driver

- **Priority state machine:** Error > Charging > Message > Idle.
- **LED patterns:**
  - `CHARGING` → red solid
  - `CHARGED` → green solid
  - `MESSAGE` → orange breathing (PWM mix of red + green)
  - `ERROR` → red fast blink

### `HapticFeedback` — Motor via PWM

- **Voltage compensation:** read VBAT from the charger ADC; adjust PWM duty cycle
  to keep average motor voltage near **3.0 V** (avoid overvoltage from VSYS at
  4.2 V).
- **Patterns:** `Click` (short), `Buzz` (long), `Alarm` (SOS rhythm).

---

## 3. Power Management State Machine

Power management runs on **Core 1**. The FSM has four states; every transition
must be announced to Core 0 via IPC so Core 0 can adjust LoRa duty cycle and
radio behaviour. Exact command ID and payload encoding are in
`ipc_protocol.h`; FSM implementation lives in FA §4.5.

### States

| State      | Screen | CPU Clock         | LoRa                  | Trigger                                                     |
|------------|--------|-------------------|-----------------------|-------------------------------------------------------------|
| `ACTIVE`   | ON     | 133 MHz           | Rx / Tx               | Any key press; boot                                         |
| `IDLE`     | OFF    | 48 MHz            | Rx (short duty cycle) | 60 s idle in ACTIVE; any key or IMU WoM → ACTIVE            |
| `SLEEP`    | OFF    | WFI + clock-gated | Rx (long duty cycle)  | 5 min in IDLE; LoRa IRQ / power key / USB wakes             |
| `SHIPPING` | OFF    | OSC OFF           | OFF                   | Hold PWR 3 s — all peripherals off, charger ship-mode latch |

The `IDLE` / `SLEEP` split gives Core 0 two distinct LoRa duty-cycle profiles —
one for "briefly stepping away", one for "in pocket for hours". `SHIPPING` is
the cold-ship factory mode; exit requires USB insertion or a long PWR press that
triggers a full reboot from the charger's ship-mode latch.

### IDLE / SLEEP Behaviour

- On entry, Core 1 announces the new state to Core 0; Core 0 adjusts LoRa duty
  cycle.
- LVGL rendering is halted; only keypad scan, IMU WoM ISR, and IPC drain remain
  live.
- LCD `Sleep In` issued; backlight off.
- In `SLEEP`, non-essential FreeRTOS tasks are suspended; only IPC-RX, power
  management, and keypad / WoM ISRs are active.

### Wakeup Sources (SLEEP / SHIPPING)

| Source         | Trigger                    |
|----------------|----------------------------|
| Power button   | GPIO edge                  |
| LoRa interrupt | SX1262 DIO1                |
| USB insertion  | VBUS detect                |
| IMU WoM        | LSM6DSV16X INT1            |

GPIO assignments are defined in `docs/design-notes/mcu-gpio-allocation.md`.

### Wakeup Sequence

1. IRQ detected → CPU clock restored → peripherals reinitialised → wakeup
   reason checked.
2. **LoRa IRQ:** Core 0 decodes the packet and notifies Core 1; Core 1 displays
   the notification or vibrates; system returns to `IDLE` / `SLEEP`.
3. **Power key:** Core 1 turns on the screen and transitions to `ACTIVE`; Core 0
   is notified.

---

## 4. UI / UX Design

### 4.1 Interaction Model

Classic feature-phone navigation (Nokia / BlackBerry style):

| Key  | Action                                        |
|------|-----------------------------------------------|
| FUNC | Open context menu for current page            |
| BACK | Go back; long-press → home                    |
| OK   | Confirm / enter / view detail                 |
| PWR  | Short: toggle standby; long: power menu       |
| 0–9  | Quick-jump to app in grid view                |

### 4.2 Visual Theme Engine

| Theme          | Description                                           |
|----------------|-------------------------------------------------------|
| High Contrast  | White background, black bold text — sunlight readable |
| Tactical Night | Pure black, red/green text, minimal brightness        |

**Status bar (always visible):** left — LoRa RSSI, GPS satellite count; right —
unread messages, battery, time.
**Toasts:** bottom pop-up notifications (e.g. "Sent", "GPS locked") —
auto-dismiss after 3 s.

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
- **Chat view:** bubble layout; own messages right-aligned with delivery ticks
  (sending / delivered to mesh / read); others left-aligned with sender name.
- **Composer:** T9 bilingual (Chinese + English) input; FUNC key inserts GPS
  coords, canned messages, or telemetry.

#### Map & Navigation
- **Radar view:** no map file needed; concentric rings show relative bearing
  and distance of nearby nodes. Useful for SAR and low-visibility situations.
- **Offline map:** vector data from Flash; supports zoom; shows own track.

#### Node Manager
- List all mesh nodes; set custom aliases; add to ignore list; view telemetry
  history (battery, altitude, SNR).
- Start a direct message from node detail view.

#### Settings
Full configuration without a phone app:
- **Radio:** frequency preset, custom frequency, TX power.
- **Channels:** manage channels, generate/display PSK QR codes.
- **Display:** brightness, sleep timeout, theme, 180° rotation.
- **Notifications:** silent / vibrate / ring, LED colour.
- **System:** reset NodeDB, format filesystem, firmware version.
- **Power Expert:** OTG toggle, charge speed (500 mA / 1 A), live ADC readings
  (VBUS, IBAT, PCB temp, SOH %).

---

## 5. Input Method Engine — MokyaInput Engine (MIE)

MIE is developed as a self-contained sub-library. See
`docs/design-notes/mie-architecture.md` for the full architectural design note.

### 5.1 Input Modes (cycled via MODE key)

Five modes cycle in order: SmartZh → SmartEn → DirectUpper → DirectLower →
DirectBopomofo → (back to SmartZh).

| # | `InputMode` enum | Trigger | Description |
|---|-----------------|---------|-------------|
| 0 | `SmartZh` | default | Bopomofo prefix prediction; SPACE appends first-tone marker `ˉ` |
| 1 | `SmartEn` | MODE×1 | Half-keyboard letter-pair English prediction (en_dat.bin) |
| 2 | `DirectUpper` | MODE×2 | Multi-tap uppercase letters / digits |
| 3 | `DirectLower` | MODE×3 | Multi-tap lowercase letters |
| 4 | `DirectBopomofo` | MODE×4 | Single Bopomofo phoneme cycling; single-char candidates only |

**Bopomofo disambiguation (SmartZh):** syllable position state machine
(initial → medial → final → tone) constrains which of the two phonemes on an
ambiguous key is valid. Phase 1 uses the primary phoneme only; full
disambiguation is Phase 3.

**Tone-aware ranking (SmartZh):** trailing tone-key bytes or SPACE after a
matched prefix set a tone intent (1–5). Candidates are sorted into 4 tiers
(single/multi × tone-match/no-match); tier-2/3 candidates are hidden when
intent is non-zero (strict filter). Falls back to full frequency-sorted list
when no tier-0/1 candidates exist (v1 dict compatibility).

**English prediction (SmartEn):** each key press produces two candidate
letters; all valid prefix combinations are searched against an English MIED
dictionary; results are merged by frequency.

**Direct modes:** no dictionary lookup; `DirectUpper` and `DirectLower` use
multi-tap cycling; `DirectBopomofo` cycles the two phonemes on each key and
produces single-character candidates.

### 5.2 Smart Correction

- **Spatial correction:** candidate scoring penalised by physical key adjacency
  distance.
- **Phonetic fuzzy match:** near-homophone Bopomofo syllables accepted as valid
  input.
- **Dynamic weighting:** candidate rank updated from per-session input history
  (stored in LittleFS).

### 5.3 Internationalisation

- IME-Logic and language data packs are fully decoupled.
- Supports Latin-script languages (EN / FR / DE / ES) and syllabic scripts
  (Japanese kana / Korean / Bopomofo).
- Language pack loaded from Flash into PSRAM on language switch.

### 5.4 Shortcuts

| Shortcut                        | Action                    |
|---------------------------------|---------------------------|
| Long-press `0`                  | Flashlight toggle         |
| Long-press `1`                  | Speed dial                |
| FUNC + OK (hold 5 s)            | Send SOS distress signal  |
| Long-press MODE (in text field) | Quick symbol picker       |

### 5.5 MIE Development Roadmap

**Phase 1 — PC environment & validation ✓ complete**
- [x] `gen_font.py`: extract 8,104 glyphs from GNU Unifont; output MIEF v1 binary.
- [x] `gen_dict.py`: compile MoE + English word lists to MIED v2 (with tone byte); validate on PC.
- [x] `Trie-Searcher`: binary search on sorted key index; `dict_version()` v1/v2 compat; 14 GoogleTest cases passing.
- [x] `IME-Logic`: 5 input modes (SmartZh, SmartEn, DirectUpper, DirectLower, DirectBopomofo); tone-aware ranking; **83 GoogleTest cases passing**.
- [x] GUI tool (`mie_gui`): Dear ImGui + SDL2; virtual keyboard matching PCB layout; live candidate display; click-to-commit.

**Phase 1.5 — Standalone Repo & C API ✓ complete**
- [x] Split `src/ime_logic.cpp` (991 lines) → 7 focused modules + `ime_internal.h`.
- [x] Split `tests/test_ime_logic.cpp` (1,554 lines) → 3 test files + `test_helpers.h`.
- [x] `include/mie/mie.h` C API + `src/mie_c_api.cpp` — **120 GoogleTest cases passing**.
- [x] `firmware/mie/README.md` added for standalone project landing page.
- [x] `git subtree split --prefix=firmware/mie -b libmie-standalone` — branch ready.
- [ ] Push `libmie-standalone` to `tengigabytes/libmie`; replace `firmware/mie/` with submodule *(deferred)*.

**Phase 2 — Hardware integration (Rev A)**
- [ ] `hal/rp2350/`: bridge PIO+DMA key buffer to `mie::KeyEvent`.
- [ ] Boot loader: copy DAT + values from Flash to PSRAM; measure search latency.
- [ ] Display: render `font_glyphs.bin` via LVGL custom font driver on NHD 2.4″.
- [ ] UI: integrate candidate bar widget with Trie-Searcher output.

**Phase 3 — Optimisation & extension**
- [ ] Spatial + phonetic fuzzy correction.
- [ ] User-defined word list in LittleFS, merged into DAT at runtime.
- [ ] Additional language pack slots.

---

## 6. Debug / Test Control Interface

A secondary USB CDC interface (`CDC#1`) provides a host-driven control channel
for automated testing, remote debugging, and development tooling, in parallel
with the Meshtastic CLI bridge on `CDC#0`.

This is a **non-functional requirement** — it is invisible to the end user
during normal operation, but firmware MUST support it so the product can be
tested, field-debugged, and iterated on at engineering velocity.

Wire format, command catalogue, ACK semantics, and authentication are
normatively defined in
[`docs/design-notes/usb-control-protocol.md`](../design-notes/usb-control-protocol.md).
USB mode architecture (composite device, OFF/COMM selection) is in FA §4.6.

### 6.1 Use Cases

1. **Host-driven input injection** — the host synthesises key events, text
   input, and semantic UI navigation commands. The device executes them as if
   they came from the physical keypad, with an origin flag so the UI can
   distinguish hardware from injected input.
2. **UI state capture** — the host can query current screen, focus path, and
   a framebuffer (full RGB565 or CRC-only) for visual regression testing.
3. **Remote diagnostics** — the host can subscribe to push events (UI
   transitions, IME commits, power-state changes, watchdog-near warnings) and
   stream the breadcrumb log ring.
4. **Hardware-in-the-loop CI** — a Python test harness drives real hardware
   over CDC#1, making deterministic assertions via command ACKs.

### 6.2 Functional Requirements

- **FR-CTRL-1:** Every Control command MUST return a deterministic ACK with
  a host-supplied sequence number. No command is fire-and-forget. Strict
  serial order: the device MUST process one request at a time.
- **FR-CTRL-2:** ACKs MUST be sent only after the command's effect is
  observable (e.g. `KEY` ACK after the next LVGL tick, `TYPE` ACK after the
  last codepoint is dispatched). See protocol §6.2 for the per-opcode table.
- **FR-CTRL-3:** Injected key events MUST carry a `source = INJECT` flag
  distinguishable from hardware events (`source = HW`). The UI layer MUST
  honour the flag for arbitration (FR-CTRL-4) and logging (FR-CTRL-5).
- **FR-CTRL-4:** Hardware key events MUST take priority over injected
  events for the same keycode within the 20 ms debounce window. Injected
  events losing arbitration return `ERR_BUSY`.
- **FR-CTRL-5:** The breadcrumb log (FA §9.3) MUST tag injected events
  distinguishably from hardware events, so post-mortem analysis can tell
  automated activity from human activity.
- **FR-CTRL-6:** `UI_STATE` responses MUST include enough information
  (screen id, focus path, state hash) for a host to make deterministic
  assertions without screenshot comparisons.

### 6.3 Non-Functional Requirements

- **NFR-CTRL-1 (Build-time kill switch):** the entire Control surface MUST
  be removable via a single build flag (`MOKYA_ENABLE_USB_CONTROL=OFF`),
  resulting in an image that does not declare CDC#1, does not link
  `UsbCtrlTask`, and exposes zero Control attack surface. Reserved for
  future certified shipments.
- **NFR-CTRL-2 (Runtime gate):** even with the build flag ON, the feature
  MUST start **disabled** at every boot. Activation requires an explicit
  user action — either via Settings UI, or via a pre-authorised remote-unlock
  flow signed by the pairing key.
- **NFR-CTRL-3 (Authenticated sessions):** state-mutating commands (`KEY`,
  `TYPE`, `UI_CMD`, `EVENT_SUB`) MUST require prior HMAC-SHA256
  challenge-response authentication against a 32-byte control key stored in
  LittleFS. Three authentication failures within 60 s MUST lock CDC#1 for
  5 minutes.
- **NFR-CTRL-4 (Safe-mode restriction):** during safe mode (§1.1), Control
  MUST reject all state-mutating commands with `ERR_SAFE_MODE`. Read-only
  commands (`UI_STATE`, `LOG_TAIL`, `MODE_GET`, `HELLO`) MAY remain available
  for remote diagnosis.
- **NFR-CTRL-5 (No UI degradation):** Control traffic MUST NOT stall the UI.
  The `SCREEN` command MUST copy the framebuffer under the LVGL mutex within
  one 5 ms tick, then stream asynchronously.

### 6.4 Host Tooling Requirements

- **HT-CTRL-1:** A Python CLI (`mokya-ctl`) MUST ship with the firmware
  repository and provide command-line access to every Control opcode.
- **HT-CTRL-2:** A reusable Python module (`mokya_control`) MUST expose the
  protocol as a library so external `pytest` suites can drive the device
  without subprocessing the CLI.
- **HT-CTRL-3:** Keycode and UI-action enumerations used by the host MUST be
  generated from the same C headers the firmware uses
  (`firmware/mie/include/mie/keycode.h`, `firmware/core1/include/ui_actions.h`),
  so firmware and host cannot drift.
