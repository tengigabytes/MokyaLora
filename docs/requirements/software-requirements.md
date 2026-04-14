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

**GPS bridge:** Core 1 reads Teseo-LIV3FL on the sensor bus (`i2c1`, GPIO 34/35, 0x3A)
and writes NMEA sentences into `IpcGpsBuf` (a double-buffer in shared SRAM). Meshtastic's
`PositionModule` on Core 0 polls the buffer and never calls I2C directly. Core 0 never
accesses any I2C bus.

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
| Application Heap| Core 1 | 4 MB            | Message history, node cache, application data                         |
| Core 0 reserve  | Core 0 | 0 MB (reserved) | Available for StoreAndForward cache or large NodeDB; not required for normal operation |

**Display framebuffer lives in SRAM, not PSRAM.** Single full-screen framebuffer
(240×320×RGB565 = 150 KB) in Core 1 SRAM with LVGL direct mode — only dirty areas are
redrawn and DMA-flushed to the LCD via PIO 8080, achieving 60–80 FPS. PSRAM reads are
too slow for display (12.8 FPS max due to QMI per-word transaction overhead) and have a
known DMA burst read error whose root cause is not yet confirmed (see bringup log
Issue 14 / Step 25). PSRAM is used exclusively for IME dictionary data (CPU random-access
binary search, <0.4 ms/query) and application heap.

**SRAM partition (520 KB total):**

| Region | Address Range | Size | Contents |
|--------|--------------|------|----------|
| Core 0 | `0x20000000–0x2002BFFF` | 176 KB | Meshtastic: .data/.bss (54 KB) + heap (122 KB) |
| Core 1 | `0x2002C000–0x20079FFF` | 312 KB | FreeRTOS + LVGL + framebuffer + MIE + HAL |
| Shared IPC | `0x2007A000–0x2007FFFF` | 24 KB | SPSC rings (data + log + cmd) + GPS buffer |
| SCRATCH_X | `0x20080000–0x20080FFF` | 4 KB | (unused) |
| SCRATCH_Y | `0x20081000–0x20081FFF` | 4 KB | Core 0 MSP / ISR stack |

See `docs/design-notes/ipc-ram-replan.md` for detailed budget and verification data.
PSRAM is not required for Core 0 under normal operation. Both cores can access PSRAM
via the shared QMI/QSPI bus if required in future.

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

**Canonical POD types — defined in `firmware/shared/ipc/ipc_protocol.h`:**

```c
#include <stdint.h>
#define IPC_MSG_PAYLOAD_MAX       256u
#define IPC_RING_SLOT_COUNT        32u   /* DATA + CMD rings */
#define IPC_LOG_RING_SLOT_COUNT    16u   /* LOG ring (best-effort) */

/* Every IPC frame starts with this 4-byte header */
typedef struct {
    uint8_t  msg_id;         /* IpcMsgId — IPC_MSG_* or IPC_CMD_*   */
    uint8_t  seq;            /* rolling sequence number             */
    uint16_t payload_len;    /* byte length of the payload          */
} IpcMsgHeader;              /* 4 bytes                             */

/* Shared-SRAM ring slot — 4 B header + 256 B payload + 4 B metadata */
typedef struct {
    IpcMsgHeader hdr;
    uint8_t      payload[IPC_MSG_PAYLOAD_MAX];
    uint32_t     _slot_meta;    /* owned by the SPSC primitives */
} IpcRingSlot;                  /* sizeof == 264 bytes          */
```

Per-message payload layouts (`IpcPayloadText`, `IpcPayloadNodeUpdate`,
`IpcPayloadDeviceStatus`, `IpcPayloadTxAck`, `IpcPayloadPowerState`,
`IpcPayloadLogLine`, `IpcPayloadGetConfig`, `IpcPayloadConfigValue`,
`IpcPayloadConfigResult`, `IpcPayloadSetTxPower`, `IpcPayloadSetNodeAlias`) are all
defined in `ipc_protocol.h`. All are POD with fixed-size fields; variable-length
strings use a trailing flexible array accompanied by an explicit length field.

**Prohibited patterns:**

```c
/* PROHIBITED — exposes Meshtastic internal type across the boundary */
#include "mesh/MeshPacket.h"
typedef struct { MeshPacket *pkt; } IpcRingSlot;

/* PROHIBITED — C++ linkage; Core 1 build must not link to Core 0 objects */
class IPCFrame { ... };

/* PROHIBITED — direct cross-core symbol reference */
extern NodeDB g_nodeDB;   /* Core 1 must not reference any Core 0 symbol */
```

#### Bus and Peripheral Ownership

| Bus / Peripheral          | Owner  | Devices / Notes                                            |
|---------------------------|--------|------------------------------------------------------------|
| Sensor bus (`i2c1`, GPIO 34/35) | Core 1 | LSM6DSV16X, LIS2MDL, LPS22HH, Teseo-LIV3FL         |
| Power bus (`i2c1`, GPIO 6/7)   | Core 1 | BQ25622, BQ27441, LM27965 (same peripheral, different GPIOs) |
| SPI1 (GPIO 24–27)         | Core 0 | SX1262 LoRa transceiver                                    |
| PIO — keypad scan         | Core 1 | 6×6 matrix, GPIO 36–47                                     |
| PWM — motor               | Core 1 | MTR_PWM GPIO 9                                             |

Core 0 exclusively owns SPI1 and LoRa-related GPIOs. All I2C buses are owned by Core 1.
No register or memory of any Core 0-owned peripheral may be accessed by Core 1, and vice versa.

#### IPC Mechanism

- **Transport:** three SPSC (Single-Producer Single-Consumer) lock-free ring buffers in the
  dedicated 24 KB shared SRAM region at `0x2007A000`:
  - `c0_to_c1`     — DATA ring, 32 slots × `IpcRingSlot` (264 B) — protobuf frames (blocking on full)
  - `c0_log_to_c1` — LOG  ring, 16 slots × `IpcRingSlot` (264 B) — log lines (best-effort)
  - `c1_to_c0`     — CMD  ring, 32 slots × `IpcRingSlot` (264 B) — host commands
  Each `IpcRingSlot` holds an `IpcMsgHeader` (4 B) + up to 256 B payload + 4 B slot metadata.
- **Log ring rationale:** log traffic goes to its own ring so `LOG_DEBUG/LOG_INFO` output
  cannot starve protobuf data during `writeStream()` bursts. The log ring is **best-effort —
  if full, the producer drops the log line rather than blocking the data path**.
- **Doorbell:** RP2350 hardware FIFO (8 × 32-bit words, one per core) used as a wake signal only;
  the HW FIFO word carries only a frame count, not data. Ring buffer carries the actual payload.
- **Memory ordering:** `__dmb()` (Data Memory Barrier) on ring buffer head/tail writes before
  writing the doorbell.
- **Serialisation:** all application data (protobuf bytes, NMEA sentences, config fields) is
  serialised into `IpcRingSlot.payload[]` by the sender and deserialised by the receiver.
  No Meshtastic object references or pointers are present anywhere in shared SRAM.
- **GPS double-buffer:** a `uint8_t gps_buf[2][128]` array in shared SRAM holds the latest NMEA
  sentence as plain bytes. Defined in `ipc_protocol.h` as a POD array. Core 1 writes, Core 0
  reads. Core 0 never calls any GPS I2C driver — it only reads this buffer.

```
Core 0 → Core 1 : c0_to_c1      SPSC ring (32 slots, DATA — protobuf, blocking on full)
Core 0 → Core 1 : c0_log_to_c1  SPSC ring (16 slots, LOG  — best-effort, drops on full)
Core 1 → Core 0 : c1_to_c0      SPSC ring (32 slots, CMD  — host commands)
Both directions : HW FIFO doorbell (32-bit frame-count word, non-blocking)
GPS data        : gps_buf uint8_t[2][128] in shared SRAM (Core 1 writes, Core 0 reads)
```

See `docs/design-notes/firmware-architecture.md` §4 for the full 24 KB shared-SRAM byte map
and the non-overlap guarantees between the two ELF images.

**Boot handshake:** Core 0 initialises the ring buffers and `gps_buf`, then signals Core 1 via
HW FIFO (`IPC_BOOT_READY`) before Core 1 begins UI init. Core 1 must not touch IPC structures
before receiving this signal.

#### IPC Message Types

The definitive catalogue lives in `firmware/shared/ipc/ipc_protocol.h` (`IpcMsgId` enum).
All payloads are POD byte sequences — no Meshtastic types, no pointers, no C++ objects.
IDs in the `0x0X` range are Core 0 → Core 1 notifications; `0x8X` are Core 1 → Core 0
commands; `0xFX` are bidirectional / protocol.

| ID | Direction | Message Type | Payload struct / contents |
|----|-----------|--------------|---------------------------|
| `0x01` | C0 → C1 | `IPC_MSG_RX_TEXT`        | `IpcPayloadText` — from/to node ID, channel, text[] |
| `0x02` | C0 → C1 | `IPC_MSG_NODE_UPDATE`    | `IpcPayloadNodeUpdate` — node ID, RSSI, SNR×4, hops, lat/lon e7, batt mV, alias[] |
| `0x03` | C0 → C1 | `IPC_MSG_DEVICE_STATUS`  | `IpcPayloadDeviceStatus` — batt mV/%, charging, LoRa RSSI/SNR, GPS sats + fix, uptime |
| `0x04` | C0 → C1 | `IPC_MSG_TX_ACK`         | `IpcPayloadTxAck` — original seq, result (sending/delivered/failed) |
| `0x05` | C0 → C1 | `IPC_MSG_CHANNEL_UPDATE` | Channel configuration changed |
| `0x06` | C0 → C1 | `IPC_MSG_SERIAL_BYTES`   | Raw CLI byte stream (M1 byte bridge, ≤ 256 B per slot) |
| `0x07` | C0 → C1 | `IPC_MSG_CONFIG_VALUE`   | `IpcPayloadConfigValue` — key + typed value (reply or push) |
| `0x08` | C0 → C1 | `IPC_MSG_CONFIG_RESULT`  | `IpcPayloadConfigResult` — key + OK / UNKNOWN_KEY / INVALID / BUSY |
| `0x09` | C0 → C1 | `IPC_MSG_REBOOT_NOTIFY`  | Core 0 about to reboot — Core 1 should detach USB CDC |
| `0x81` | C1 → C0 | `IPC_CMD_SEND_TEXT`      | `IpcPayloadText` — dest node ID, channel, want_ack, text[] |
| `0x82` | C1 → C0 | `IPC_CMD_SET_CHANNEL`    | Channel index + optional new config bytes |
| `0x83` | C1 → C0 | `IPC_CMD_SET_TX_POWER`   | `IpcPayloadSetTxPower` — `int8_t power_dbm` |
| `0x84` | C1 → C0 | `IPC_CMD_REQUEST_STATUS` | Empty — request an immediate `IPC_MSG_DEVICE_STATUS` |
| `0x85` | C1 → C0 | `IPC_CMD_SET_NODE_ALIAS` | `IpcPayloadSetNodeAlias` — node ID + alias UTF-8 |
| `0x86` | C1 → C0 | `IPC_CMD_POWER_STATE`    | `IpcPayloadPowerState` — 0 ACTIVE / 1 IDLE / 2 SLEEP / 3 SHIPPING |
| `0x87` | C1 → C0 | `IPC_CMD_REBOOT`         | Empty payload |
| `0x88` | C1 → C0 | `IPC_CMD_FACTORY_RESET`  | Empty — wipes persistent config |
| `0x89` | C1 → C0 | `IPC_CMD_GET_CONFIG`     | `IpcPayloadGetConfig` — `uint16_t key` |
| `0x8A` | C1 → C0 | `IPC_CMD_SET_CONFIG`     | `IpcPayloadConfigValue` — key + typed value |
| `0x8B` | C1 → C0 | `IPC_CMD_COMMIT_CONFIG`  | Empty — save pending changes (may reboot) |
| `0xF0` | both    | `IPC_MSG_LOG_LINE`       | `IpcPayloadLogLine` — level, originating core, UTF-8 text[] |
| `0xFE` | both    | `IPC_MSG_PANIC`          | Cross-core panic notification (reserved for M6) |
| `0xFF` | both    | `IPC_BOOT_READY`         | Boot handshake marker (also written to shared handshake block) |

**GPS data does not use an IPC message.** The `IpcGpsBuf` double-buffer in shared SRAM
is the GPS transport; Core 1 flips `write_idx` atomically after committing a sentence,
and Core 0 reads `buf[write_idx ^ 1]`. No doorbell is required — the `write_idx` flip
*is* the signal.

`IpcConfigKey` in `ipc_protocol.h` enumerates 18 typed config keys across 7
categories (Device, LoRa, Position, Power, Display, Channel, Owner) — consumed by the
`GET_CONFIG` / `SET_CONFIG` / `COMMIT_CONFIG` triple.

#### Dual Build System Isolation

The dual-licence boundary is reinforced by using **two independent build tools** —
not one unified CMake tree. Core 0 uses PlatformIO (to track the upstream Meshtastic
toolchain); Core 1 uses CMake + Ninja (to use the Pico SDK directly without Arduino-Pico).
`scripts/build_and_flash.sh` drives both builds, then flashes the two flash slots via J-Link.

```
firmware/
├── core0/meshtastic/                    — Core 0, PlatformIO, GPL-3.0
│   .pio/build/rp2350b-mokya/firmware.elf  → flash @ 0x10000000
│   variants/.../patch_arduinopico.py       — idempotent framework patches at build time
│
├── core1/m1_bridge/                     — Core 1, CMake + Ninja, Apache-2.0
│   build/core1_bridge/core1_bridge.bin    → flash @ 0x10200000 (raw bytes, no IMAGE_DEF)
│   memmap_core1_bridge.ld                 — Core 1 linker script
│
├── mie/                                 — MIT static library, no Pico SDK dependency
│                                           host-only unit-test build runs on PC
│
└── shared/ipc/                          — header-only, MIT
    ipc_protocol.h, ipc_shared_layout.h    shared by both builds, #include <stdint.h> only
```

**Rules:**
- PlatformIO `build_src_filter` excludes `firmware/core1/` — Core 0 cannot link Core 1 objects.
- Core 1 CMake `add_executable()` lists sources explicitly; no Meshtastic include path is
  available — Core 1 cannot `#include` any Meshtastic header.
- `firmware/shared/ipc/` is the only compile-time surface shared by both builds; its headers
  may `#include` only `<stdint.h>` / `<stddef.h>`.
- Post-link CI check: `nm` both ELFs and assert `g_ipc_shared` resolves to the identical
  address. Layout drift between `ipc_shared_layout.h` and the two linker regions becomes a
  build-break, not a silent memory stomp.
- There is **no** top-level CMake tree that links both cores together. Cross-core coupling
  exists only at flash-programming time — the J-Link script programs two independent
  artefacts (`firmware.elf` + `core1_bridge.bin`) into two non-overlapping flash slots.

See `docs/design-notes/firmware-architecture.md` §8 for the full build-system description.

#### Meshtastic PhoneAPI Porting

`PhoneAPI` (`meshtastic/firmware/src/mesh/PhoneAPI.h`) is Meshtastic's abstract transport interface.
A new subclass `IPCPhoneAPI` replaces the USB/BLE transport with IPC ring buffer writes.
This is the **only structural addition to Meshtastic** on Core 0:

| Meshtastic Component             | Adaptation                                                         |
|----------------------------------|--------------------------------------------------------------------|
| `PhoneAPI`                       | Subclassed as `IPCPhoneAPI` — serialises into `IpcRingSlot`, writes to `c0_to_c1` DATA ring |
| `GPSDriver`                      | Replaced by GPS adapter that reads `gps_buf[]` (plain bytes, no I2C) |
| `SerialAPI` / `BLEApi`           | Excluded from Core 0 build                                         |
| `Router`, `MeshService`, all Modules | **No changes**                                               |
| `NodeDB` / `config`              | Core 0 pushes node entries via `IPC_MSG_NODE_UPDATE` (unsolicited, on change) and answers config round-trips via `IPC_CMD_GET_CONFIG` / `IPC_CMD_SET_CONFIG` / `IPC_CMD_COMMIT_CONFIG`. No direct cross-core memory access. |
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

### `EnvironmentSensor` — LSM6DSV16X, LIS2MDL, LPS22HH [Core 1 · sensor bus, `i2c1`, GPIO 34/35]

| Driver | Address | Function                                           |
|--------|---------|----------------------------------------------------|
| IMU    | 0x6A    | 6-axis attitude; gesture detection (raise-to-wake) |
| Mag    | 0x1E    | Electronic compass heading                         |
| Baro   | 0x5D    | Barometric altitude                                |

- **Fusion:** Kalman filter or Madgwick algorithm for 9-DOF data fusion.

### `GPSDriver_I2C` — ST Teseo-LIV3FL [Core 1 · sensor bus `i2c1` GPIO 34/35 · 0x3A]

- **Transport:** `i2c1` polling mode on the sensor bus (shared with IMU 0x6A, Mag 0x1E, Baro 0x5D).
- **Init:** disable unused NMEA sentences (keep RMC, GGA only); set 1 Hz update rate.
- **IPC bridge:** parsed NMEA sentence written into `IpcGpsBuf.buf[write_idx ^ 1]` in shared
  SRAM. After `memcpy` + `len[]` update, a `__dmb()` barrier commits the new slot, then
  `write_idx` is flipped atomically. **No doorbell is sent — the `write_idx` flip *is* the
  signal.** Meshtastic's `PositionModule` on Core 0 polls `buf[write_idx ^ 1]` once per
  second and never calls any I2C driver.

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

Power management runs on **Core 1**. The FSM has four states that map 1:1 to
`IpcPowerState` in `ipc_protocol.h`. Every transition is announced to Core 0 via
`IPC_CMD_POWER_STATE` so that Core 0 can adjust LoRa duty cycle and radio behaviour.

### States

| # | State (`IpcPowerState`) | Screen | CPU Clock | LoRa                         | Trigger                                               |
|---|-------------------------|--------|-----------|------------------------------|-------------------------------------------------------|
| 0 | `IPC_POWER_ACTIVE`      | ON     | 133 MHz   | Rx / Tx                      | Any key press; boot                                   |
| 1 | `IPC_POWER_IDLE`        | OFF    | 48 MHz    | Rx (short duty cycle)        | 60 s idle in ACTIVE; any key or IMU WoM → ACTIVE      |
| 2 | `IPC_POWER_SLEEP`       | OFF    | WFI + clock-gated | Rx (long duty cycle) | 5 min in IDLE; LoRa IRQ / power key / USB wakes       |
| 3 | `IPC_POWER_SHIPPING`    | OFF    | OSC OFF   | OFF                          | Hold PWR 3 s — all peripherals off, BQ25622 ship-mode latch |

The `IDLE` + `SLEEP` split replaces the old single STANDBY bucket so Core 0 can pick
different LoRa duty cycles for "briefly stepping away" vs. "pocket for hours".
`SHIPPING` is the cold-ship factory mode — exit requires USB insertion or a long
PWR press that triggers a full reboot from BQ25622 ship-mode latch.

### IDLE / SLEEP Behaviour

- Core 1 sends `IPC_CMD_POWER_STATE = IDLE` or `SLEEP`; Core 0 adjusts LoRa duty cycle accordingly.
- LVGL rendering halted; only the keypad scan loop, IMU WoM ISR, and IPC drain remain live.
- LCD `Sleep In` command sent; backlight off.
- In `SLEEP`, FreeRTOS suspends non-essential tasks; only `IPCRxTask`, `PowerTask`, and the
  keypad / WoM ISRs are active.

### Wakeup Sources (SLEEP / SHIPPING)

| Source         | GPIO                       |
|----------------|----------------------------|
| Power button   | GPIO 33                    |
| LoRa interrupt | GPIO 29 (SX1262 DIO1)      |
| USB insertion  | GPIO 1                     |
| IMU WoM        | LSM6DSV16X INT1 pin        |

### Wakeup Sequence

1. IRQ detected → CPU clock restored → peripherals reinitialised → wakeup reason checked.
2. **LoRa IRQ:** Core 0 reads packet → sends `IPC_MSG_RX_TEXT` (or other notification) to
   Core 1 → Core 1 displays notification or vibrates → returns to `IDLE` / `SLEEP`.
3. **Power key:** Core 1 turns on screen → sends `IPC_CMD_POWER_STATE = ACTIVE` to Core 0 →
   enters `ACTIVE`.

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

Five modes cycle in order: SmartZh → SmartEn → DirectUpper → DirectLower → DirectBopomofo → (back to SmartZh).

| # | `InputMode` enum | Trigger | Description |
|---|-----------------|---------|-------------|
| 0 | `SmartZh` | default | Bopomofo prefix prediction; SPACE appends first-tone marker `ˉ` |
| 1 | `SmartEn` | MODE×1 | Half-keyboard letter-pair English prediction (en_dat.bin) |
| 2 | `DirectUpper` | MODE×2 | Multi-tap uppercase letters / digits |
| 3 | `DirectLower` | MODE×3 | Multi-tap lowercase letters |
| 4 | `DirectBopomofo` | MODE×4 | Single Bopomofo phoneme cycling; single-char candidates only |

**Bopomofo disambiguation (SmartZh):** syllable position state machine (initial → medial → final → tone)
constrains which of the two phonemes on an ambiguous key is valid. Phase 1 uses the primary
phoneme only; full disambiguation is Phase 3.

**Tone-aware ranking (SmartZh):** trailing tone-key bytes or SPACE after a matched prefix set a
tone intent (1–5). Candidates are sorted into 4 tiers (single/multi × tone-match/no-match);
tier-2/3 candidates are hidden when intent is non-zero (strict filter). Falls back to full
frequency-sorted list when no tier-0/1 candidates exist (v1 dict compatibility).

**English prediction (SmartEn):** each key press produces two candidate letters; all valid prefix
combinations are searched against an English MIED dictionary; results are merged by frequency.

**Direct modes:** no dictionary lookup; `DirectUpper` and `DirectLower` use multi-tap cycling;
`DirectBopomofo` cycles the two phonemes on each key and produces single-character candidates.

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
