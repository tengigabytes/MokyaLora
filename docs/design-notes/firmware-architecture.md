# Firmware Architecture

**Project:** MokyaLora (Project MS-RP2350)
**Status:** Phase 2 — Core 0/Core 1 dual-image production firmware (Rev A)
**Last updated:** 2026-04-15

This document is the **single source of truth** for how MokyaLora's dual-core
firmware is built. If it conflicts with the SRS
(`docs/requirements/software-requirements.md`) or with CLAUDE.md on any
implementation detail — memory map, IPC byte layout, build flow, boot sequence,
licence boundary — this document and the normative headers it references
(`ipc_protocol.h`, `ipc_shared_layout.h`, `memmap_core1_bridge.ld`) win.

The SRS specifies **what** the firmware must do (user behaviour, performance
targets, compatibility). This document specifies **how** that is realised.

It covers:

1. Dual-core AMP topology and licence boundary (§1)
2. Full memory map — Flash layout, SRAM partition, PSRAM allocation (§2)
3. Per-core software stacks — from HW peripheral up through driver → HAL → service → app (§3, §4)
4. USB Mode A / Mode B — charge-only vs CDC data-terminal (§4.6)
5. Shared-SRAM IPC — 24 KB byte map, message catalogue, end-to-end flows, doorbell (§5)
6. MIE user-dictionary persistence via IPC (§5.7, planned for M5+)
7. Cross-image non-interference — link, boot, SRAM, SPSC, memory ordering, flash safety (§6)
8. Bus and peripheral ownership (§7)
9. Build system isolation — PlatformIO vs. CMake, ELF vs. BIN, dual-image flashing (§8)
10. Boot sequence, handshake, watchdog discipline and safe mode (§9)
11. UF2 release distribution strategy (§10)
12. Licence compliance for binary distribution (§11)
13. Hardware revision boundary — what survives vs. what is Rev A-only (§12)

Primary sources of truth:

- `firmware/core1/m1_bridge/memmap_core1_bridge.ld` — Core 1 linker script
- `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/patch_arduinopico.py` — Core 0 memmap patch
- `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/platformio.ini` — Core 0 build config
- `firmware/core1/m1_bridge/CMakeLists.txt` — Core 1 build config
- `firmware/shared/ipc/ipc_protocol.h`, `ipc_shared_layout.h`, `ipc_ringbuf.[ch]` — IPC transport
- `scripts/build_and_flash.sh` — dual-image build + flash driver
- `docs/design-notes/ipc-ram-replan.md` — budget derivation & SWD verification record
- `docs/requirements/software-requirements.md` — normative SRS

---

## 1. Dual-Core AMP Topology

MokyaLora runs the RP2350B's two Cortex-M33 cores as independent AMP images with separate
flash slots, separate SRAM regions, separate schedulers, and separate licences. Core 0 is
launched by the bootrom from `0x10000000`; Core 0 then launches Core 1 via
`multicore_launch_core1_raw()` from `0x10200000`.

| Attribute             | Core 0                                           | Core 1                                            |
|-----------------------|--------------------------------------------------|---------------------------------------------------|
| Role                  | Meshtastic LoRa modem                            | UI host, IME, power manager, all I2C drivers      |
| Licence               | GPL-3.0                                          | Apache-2.0                                        |
| Flash slot            | `0x10000000` (2 MB)                              | `0x10200000` (2 MB)                               |
| SRAM region           | `0x20000000 – 0x2002BFFF` (176 KB)               | `0x2002C000 – 0x20079FFF` (312 KB)                |
| Framework             | Arduino-Pico + Meshtastic base                   | Pico SDK + FreeRTOS + LVGL                        |
| Scheduler             | `OSThread` cooperative (`concurrency::Scheduler`)| FreeRTOS V11.3.0 preemptive (single-core port)    |
| Boot model            | Bootrom → crt0 → `main()`                        | `multicore_launch_core1_raw()` (no IMAGE_DEF)     |
| Build tool            | PlatformIO (`pio run`)                           | CMake + Ninja                                     |
| Owns                  | SPI1 + SX1262                                    | I2C ×2, PIO (keypad, LCD), PWM, GPIO, USB CDC     |

The sole cross-core compile-time dependency is `firmware/shared/ipc/ipc_protocol.h` (MIT) —
POD types, `<stdint.h>` only. The IPC boundary behaves as a **wire protocol**, not a library
link. There is no C++, no pointers, no project-internal header dependency.

---

## 2. Memory Map — Complete View

RP2350B has three distinct storage resources. Each is partitioned between the two cores at
link time; no run-time allocator ever crosses the boundary.

```
┌─────────────────────────────────────────────────────────────────────────┐
│  W25Q128JW QSPI Flash (16 MB)  — XIP via QMI                            │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  0x10000000 │ Core 0 firmware     │ 2 MB │ GPL-3.0 ELF          │    │
│  │  0x10200000 │ Core 1 firmware     │ 2 MB │ Apache-2.0 BIN       │    │
│  │  0x10400000 │ OTA / recovery      │ 4 MB │ reserved (M6)        │    │
│  │  0x10800000 │ Assets (dict,font)  │ 4 MB │ gen_*.py output      │    │
│  │  0x10C00000 │ LittleFS            │ 4 MB │ settings + msg DB    │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                         │
│  RP2350B SRAM (520 KB)  — on-chip, 0 wait-state                         │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  0x20000000 │ Core 0 region       │ 176 KB │ Meshtastic image   │    │
│  │  0x2002C000 │ Core 1 region       │ 312 KB │ FreeRTOS+LVGL+MIE  │    │
│  │  0x2007A000 │ Shared IPC          │  24 KB │ NOLOAD rings + GPS │    │
│  │  0x20080000 │ SCRATCH_X (Core 0)  │   4 KB │ task stack         │    │
│  │  0x20081000 │ SCRATCH_Y (Core 0)  │   4 KB │ MSP/ISR stack      │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                         │
│  APS6404L QSPI PSRAM (8 MB)  — XIP via QMI, Core 1 only                 │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  0x11000000 │ MIE dictionary      │ 4 MB │ DAT + values + font  │    │
│  │  0x11400000 │ Application heap    │ 4 MB │ msg history, node $  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Flash Layout (16 MB — W25Q128JW)

| Address        | Size  | Content                                    | Owner         | Write policy              |
|----------------|-------|--------------------------------------------|---------------|---------------------------|
| `0x10000000`   | 2 MB  | Core 0 firmware (Meshtastic ELF)           | Core 0 ELF    | J-Link flash or OTA       |
| `0x10200000`   | 2 MB  | Core 1 firmware (raw `.bin`, no IMAGE_DEF) | Core 1 BIN    | J-Link flash or OTA       |
| `0x10400000`   | 4 MB  | OTA buffer / factory recovery image        | reserved (M6) | OTA flow writes           |
| `0x10800000`   | 4 MB  | Assets: dict, font, language pack, icons   | gen_*.py      | gen script + flasher      |
| `0x10C00000`   | 4 MB  | LittleFS — user config, message DB, NodeDB | Core 0        | at runtime (flash-safe)   |

Notes:
- The two firmware slots are hard-coded in the linker scripts, not a bootloader. Switching
  to A/B OTA later only requires rewriting the launch sequence in Core 0's early init.
- LittleFS is mounted by Core 0 (Meshtastic's existing persistence layer writes here).
  Core 1 never touches this region directly — any config change goes via `IPC_CMD_SET_CONFIG`
  so Core 0 is the sole flash writer. This keeps the P2-11 flash-write safety discipline
  single-core: only Core 0 needs to coordinate the "park Core 1 + disable IRQ + XIP off"
  dance (see `flash_safety_wrap.c`).
- Assets region (dict, font) is read-only XIP from Core 1's perspective — `gen_dict.py` and
  `gen_font.py` produce the `.bin` images, which are flashed once alongside firmware. Core 1
  copies DAT + values into PSRAM at boot; font glyphs stay XIP.

### 2.2 SRAM Partition (520 KB on-chip)

Verified by reading both linker scripts and by SWD inspection of both cores' SPs +
`g_ipc_shared.boot_magic` after boot.

```
0x20000000 ┌────────────────────────────────────────────────────┐
           │  Core 0 — Meshtastic (GPL-3.0)               176KB │
           │   .vector + .data + .bss             54 KB         │
           │   .heap (protobuf, NodeDB, router)  122 KB         │
0x2002C000 ├────────────────────────────────────────────────────┤
           │  Core 1 — FreeRTOS + LVGL + MIE (Apache-2.0) 312KB │
           │   LCD framebuffer 240×320×16bpp     150 KB         │
           │   LVGL heap + widgets                48 KB         │
           │   FreeRTOS Heap4 (task stacks+TCB)   32 KB         │
           │   FreeRTOS kernel .bss                8 KB         │
           │   HAL + MIE + App state              28 KB         │
           │   Main stack (__StackBottom..Top)     8 KB         │
           │   Reserve / margin                   38 KB         │
0x2007A000 ├────────────────────────────────────────────────────┤
           │  Shared IPC (NOLOAD, MIT)                     24KB │
           │   Handshake (16 B)                                 │
           │   3 × SPSC ring control + slots (21 KB)            │
           │   GPS double-buffer (260 B)                        │
           │   Reserved tail (3 KB, breadcrumbs at 0x5FC0)      │
0x20080000 ├────────────────────────────────────────────────────┤
           │  SCRATCH_X — Core 0 Arduino-Pico task stack   4KB  │
0x20081000 ├────────────────────────────────────────────────────┤
           │  SCRATCH_Y — Core 0 MSP / ISR stack           4KB  │
0x20082000 └────────────────────────────────────────────────────┘
           __StackTop (Core 0)
```

#### Core 0 SRAM breakdown (176 KB)

| Segment    | Size    | Contents                                                           |
|------------|---------|--------------------------------------------------------------------|
| `.vectors` | ~1 KB   | Cortex-M33 vector table (ROM copy lives in flash, RAM is writable) |
| `.data`    | ~6 KB   | Initialised globals (router state, config defaults)                |
| `.bss`     | ~47 KB  | Zero-init globals: NodeDB static, packet queue arrays, crypto ctx  |
| `.heap`    | 122 KB  | Arduino-Pico + Meshtastic dynamic allocations                      |

Meshtastic's peak heap usage (200 nodes, full NodeDB + pending-packet queue + AES contexts)
sits around 100 KB; 122 KB gives ~20% headroom. The Arduino-Pico **task stack** (8 KB
cooperative `OSThread` stack) lives in `SCRATCH_X`, not in this 176 KB region.

#### Core 1 SRAM breakdown (312 KB)

| Segment              | Size    | Contents                                                     |
|----------------------|---------|--------------------------------------------------------------|
| Framebuffer          | 150 KB  | `uint16_t fb[240*320]` — LVGL direct-mode primary buffer     |
| LVGL internal heap   | 48 KB   | `lv_mem` pool — widgets, styles, image cache                 |
| FreeRTOS Heap4       | 32 KB   | `ucHeap[]` — all task stacks + TCBs + queues (see §4.2)      |
| FreeRTOS kernel .bss | 8 KB    | task/timer list heads, scheduler state, critical nesting     |
| HAL / driver state   | 12 KB   | I2C buffers, keypad scan buf, GPS NMEA parser                |
| MIE runtime state    | 10 KB   | IME FSM, candidate ring, current syllable parser             |
| App state            | 6 KB    | Message list cursors, LVGL obj pointers, power FSM           |
| Main stack           | 8 KB    | `__StackBottom..__StackTop` — `main()` + ISR entry           |
| Reserve / margin     | 38 KB   | unallocated; absorbs LVGL peak + message cache growth        |

Task stacks are **not** a separate segment — under `heap_4` every `xTaskCreate()` call
allocates its stack out of `ucHeap`. Summing the per-task stacks in §4.2 plus ~2 KB of
idle/timer task overhead plus TCBs + queues yields ~28 KB live in Heap4, leaving 4 KB
headroom inside the 32 KB pool for burst queue growth.

Target margin ≥ 10 % of 312 KB (31 KB); current 38 KB margin is **12.2 %**.

#### Shared IPC region (24 KB) — see §5

#### SCRATCH_X / SCRATCH_Y (8 KB)

Owned by Core 0. Arduino-Pico's FreeRTOS port places its task stack in SCRATCH_X and the
MSP (handler-mode) stack in SCRATCH_Y. Core 1's linker script `PROVIDE`s zero-length
`__scratch_[xy]_*` symbols so the SDK's crt0 data-copy table sees an empty copy range and
never touches those addresses.

### 2.3 PSRAM Allocation (8 MB — APS6404L via QMI)

PSRAM is XIP-addressable at `0x11000000` and is accessible by either core via the shared
QMI/QSPI bus. In practice **only Core 1** uses PSRAM; Core 0 does not need it for the
planned workload.

| Address       | Size  | Content                     | Access pattern                        |
|---------------|-------|-----------------------------|---------------------------------------|
| `0x11000000`  | 4 MB  | MIE dictionary — DAT + values | CPU random-access, <0.4 ms/query    |
| `0x11400000`  | 4 MB  | Application heap            | message history, node cache, LV image |

Rules:
- **Framebuffer is NOT in PSRAM.** APS6404L tops out at 12.8 FPS for full-frame reads due to
  QMI per-word overhead, and the DMA burst-read anomaly (Step 25 / Issue 14) is unresolved.
  Framebuffer therefore lives in the 150 KB SRAM block above.
- MIE dict + values are copied from Flash `0x10800000` (assets region) into PSRAM at boot;
  this takes ~40 ms and gives the IME sub-millisecond lookup latency.
- Application heap in PSRAM uses a simple bump allocator (single-writer, LIFO release at
  power-mode transition). No LVGL `lv_mem_alloc` backing in PSRAM because LVGL widgets need
  low-latency SRAM access during redraw.
- Both cores can access PSRAM via QMI but there is no software arbitration — by convention
  Core 0 never touches it under the Phase 2 build.

---

## 3. Core 0 Architecture — Meshtastic Modem

Core 0 is stock Meshtastic with exactly one structural change: the USB/BLE host transport is
replaced with `IPCPhoneAPI`, a subclass of Meshtastic's `PhoneAPI` that writes protobuf
bytes into the shared-SRAM ring instead of a serial or BLE endpoint. Everything above
`IPCPhoneAPI` is unmodified Meshtastic code.

### 3.1 Layered stack (bottom → top)

```
┌──────────────────────────────────────────────────────────────────────┐
│ APPLICATION    Meshtastic modules (modules/*.cpp)                    │
│   TextMessageModule · PositionModule · NodeInfoModule · AdminModule  │
│   Telemetry (device only) · Traceroute · NeighborInfo · Waypoint     │
├──────────────────────────────────────────────────────────────────────┤
│ SERVICE        MeshService · Router · Channels · PKI (CryptoEngine)  │
│                NodeDB · MessageStore (LittleFS persist)              │
├──────────────────────────────────────────────────────────────────────┤
│ TRANSPORT      PhoneAPI  ──(subclass)──►  IPCPhoneAPI                │
│                  │                         │ writes protobuf to      │
│                  │                         │ c0_to_c1 DATA ring      │
│                  └─ IpcSerialStream (replaces Arduino Serial) ◄─ CMD │
├──────────────────────────────────────────────────────────────────────┤
│ RADIO          RadioInterface (abstract) → SX126xInterface           │
│                MeshPacket queue · rx/tx FSM · CryptoEngine           │
├──────────────────────────────────────────────────────────────────────┤
│ SCHEDULER      concurrency::OSThread cooperative loop                │
│                FreeRTOS (single-core, configNUMBER_OF_CORES=1)       │
├──────────────────────────────────────────────────────────────────────┤
│ HAL            Arduino-Pico: SPI, GPIO, PIO-multicore doorbell       │
│                flash_safety_wrap (park Core 1 before XIP off)        │
├──────────────────────────────────────────────────────────────────────┤
│ HW             SX1262 (SPI1 GPIO 24–27) · W25Q128 XIP flash          │
│                SIO FIFO (inter-core doorbell)                        │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.2 Scheduler model

Arduino-Pico cooperative `OSThread` scheduler (`concurrency::Scheduler`) drives all
Meshtastic runLoop work. FreeRTOS inside the Arduino-Pico core is forced to **single-core
mode** (`configNUMBER_OF_CORES=1`) because the upstream SMP port assumes it owns Core 1,
which MokyaLora gives to a separate image. The five framework patches that achieve this are
applied idempotently by `patch_arduinopico.py` at build time (see phase2-log Issue P2-2):

1. `SerialUSB.h` — extern guard under `-DNO_USB`
2. `freertos-main.cpp` — guard SMP call sites
3. `freertos-lwip.cpp` — guard SMP call sites
4. `portmacro.h` — add missing extern decl
5. `port.c` — drop `static` on a function the guard references

### 3.3 Meshtastic module exclusions

Core 0 is trimmed hard to fit 176 KB SRAM + 2 MB flash. From `platformio.ini`:

```
-D MESHTASTIC_EXCLUDE_GPS, _SCREEN, _WIFI, _BLUETOOTH, _I2C, _MQTT,
   _POWERMON, _POWER_FSM, _TZ, _AUDIO, _DETECTIONSENSOR,
   _ENVIRONMENTAL_SENSOR, _AIR_QUALITY_SENSOR, _HEALTH_TELEMETRY,
   _EXTERNALNOTIFICATION, _PAXCOUNTER, _POWER_TELEMETRY, _RANGETEST,
   _REMOTEHARDWARE, _STOREFORWARD, _ATAK, _CANNEDMESSAGES,
   _INPUTBROKER, _SERIAL, _POWERSTRESS
```

Kept: `TextMessage`, `Position`, `NodeInfo`, `Admin`, `Traceroute`, `NeighborInfo`,
`Waypoint`, `PKI`, `DeviceTelemetry`. GPS data arrives via the shared-SRAM GPS buffer —
Core 0 never talks I2C.

### 3.4 Flash-write safety

Core 0 is the sole flash writer (LittleFS + Meshtastic config). Flash erase/program on
RP2350 turns XIP off for the duration — any code running from XIP during that window faults
with IACCVIOL (P2-11).

The `flash_safety_wrap.c` TU `-Wl,--wrap`s `flash_range_erase` and `flash_range_program` to:

1. Raise a doorbell to Core 1: "park for flash write"
2. Wait for Core 1 acknowledge via FIFO
3. Disable all IRQs locally
4. Call the real function
5. Re-enable IRQs, release Core 1

Core 1's reset path for this doorbell is a tight busy-loop in RAM (no XIP fetch), so it is
safe while Core 0's XIP is off.

---

## 4. Core 1 Architecture — UI / App Host

Core 1 is a fresh Apache-2.0 image built with the Pico SDK directly (not Arduino-Pico). It
owns every peripheral that isn't LoRa: the LCD, keypad, all I2C sensors and PMICs, PWM
haptic motor, status LED, USB CDC. Audio (PDM mic + I2S amp) is **not** on the target
architecture — the next hardware revision removes both devices.

### 4.1 Layered stack (bottom → top)

```
┌──────────────────────────────────────────────────────────────────────┐
│ APPLICATION    LVGL screens (lv_scr_act view tree)                   │
│   MessagesApp · NodesApp · MapApp · CompassApp · SettingsApp         │
│   StatusBar · NotifyToast · IMEBar                                   │
├──────────────────────────────────────────────────────────────────────┤
│ SERVICE        MIE (IME)  · Power Manager FSM  · IPC Client          │
│                MessageModel (receives IPC_MSG_RX_TEXT)               │
│                NodeModel   (receives IPC_MSG_NODE_UPDATE)            │
│                StatusModel (receives IPC_MSG_DEVICE_STATUS)          │
│                ConfigModel (get/set via IPC_CMD_GET/SET_CONFIG)      │
├──────────────────────────────────────────────────────────────────────┤
│ HAL            Charger (BQ25622)   · Gauge (BQ27441)                 │
│                IMU (LSM6DSV16X)    · Mag (LIS2MDL) · Baro (LPS22HH)  │
│                GPS (Teseo-LIV3FL)  · StatusLED (LM27965)             │
│                HapticMotor (PWM)   · KeypadScan (PIO+DMA) · LCD (PIO)│
│                USB CDC (TinyUSB)                                     │
├──────────────────────────────────────────────────────────────────────┤
│ DRIVER         I2C abstraction · PIO program loader · DMA dispatch   │
│                TinyUSB device core                                   │
├──────────────────────────────────────────────────────────────────────┤
│ SCHEDULER      FreeRTOS V11.3.0 preemptive (single-core port)        │
│                tasks: UI, IPCRx, KeypadScan, Sensors, Power, USB     │
├──────────────────────────────────────────────────────────────────────┤
│ HW             i2c1 (34/35 sensor) · i2c1 (6/7 power) · SPI0 (free)  │
│                PIO0 (LCD) · PIO1 (keypad) · PWM (motor) · USB        │
│                SIO FIFO (inter-core doorbell)                        │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.2 FreeRTOS task layout

FreeRTOS V11.3.0 RP2350 Cortex-M33 port (community-supported, NTZ variant), single-core.
Heap scheme: `heap_4` (`ucHeap[configTOTAL_HEAP_SIZE]` in `.bss`).

| Task           | Priority | Stack   | Period / Event        | Responsibility                             |
|----------------|----------|---------|-----------------------|--------------------------------------------|
| `UITask`       | 4 (high) | 8 KB    | 5 ms tick (LVGL)      | LVGL `lv_timer_handler()` + input dispatch |
| `IPCRxTask`    | 5 (top)  | 2 KB    | FIFO IRQ notify       | Drain DATA + LOG rings, route to Models    |
| `KeypadScan`   | 3        | 1 KB    | 8 ms tick             | Read PIO scan result, debounce, enqueue    |
| `SensorsTask`  | 2        | 2 KB    | 50 ms tick            | Poll IMU/mag/baro, update model            |
| `GPSTask`      | 2        | 2 KB    | I2C IRQ / 100 ms      | Read Teseo, write `IpcGpsBuf`              |
| `PowerTask`    | 2        | 1.5 KB  | 1 s tick              | Poll BQ25622 + BQ27441, run power FSM      |
| `USBTask`      | 3        | 2 KB    | TinyUSB callback      | Serve CDC — console + bridged CLI traffic  |
| `IMETask`      | 3        | 4 KB    | Keypad event          | Run MIE FSM on input chord                 |
| `IdleHook`     | 0        | —       | idle                  | Feed watchdog, enter WFI                   |

`UITask` holds the LVGL mutex. All HAL→UI data pushes go through FreeRTOS queues drained by
`UITask`; no other task directly touches LVGL objects.

### 4.3 Display path

Single full-screen framebuffer (240×320×RGB565 = 150 KB) in SRAM, LVGL in
`LV_DISPLAY_RENDER_MODE_DIRECT`. LVGL redraws dirty rectangles; DMA pushes dirty rects to
the NHD 2.4″ LCD via PIO 8080. Target 60–80 FPS. PSRAM is explicitly avoided for display
(see §2.3).

### 4.4 Input path

PIO1 program scans the 6×6 keypad matrix with SDM03U40 Schottky diodes (NKRO), DMA copies
the 6-byte column state into a ring buffer. `KeypadScan` task debounces (20 ms) and emits
`KeyEvent { code, type }` into a queue consumed by `IMETask` (in IME mode) or `UITask` (in
UI-navigation mode).

### 4.5 Power FSM

```
  ACTIVE ── 60 s no key ──► IDLE (LCD off, IMU WoM armed) ── WoM ──► ACTIVE
    │                         │
    │                         └── 5 min no wake ──► SLEEP (radio low duty,
    │                                                      sensors suspended)
    └── user hold PWR 3 s ──► SHIPPING (all off, BQ25622 ship mode)
```

`IPC_CMD_POWER_STATE` notifies Core 0 whenever the FSM transitions; Core 0 adjusts
transmit cadence + sleep behaviour accordingly.

### 4.6 USB Modes — charge-only vs data-terminal

Per `system-requirements.md` §4, the device supports two mutually exclusive USB modes,
selected by the user on VBUS insertion. Both modes live on Core 1 (Core 0 never touches
USB — `-D NO_USB` strips it from the Arduino-Pico build).

| Mode | TinyUSB | Host view                    | Data path                                    |
|------|---------|------------------------------|----------------------------------------------|
| A    | dormant | dumb charging sink           | Charger manages VBUS only — no enumeration   |
| B    | running | USB CDC virtual COM port     | `USBTask` bridges CDC ↔ `IPC_MSG_SERIAL_BYTES` (M1 bridge, §5.5) |

**Mode selection on VBUS insert (GPIO 1 IRQ):**

```
  VBUS IRQ ──► PowerTask reads settings key IPC_CFG_USB_MODE (M5+) or fallback default
    ├─ "Always charge-only"  → Mode A: leave TinyUSB uninitialised; BQ25622 handles VBUS
    ├─ "Always data"         → Mode B: call tud_init(); USBTask resumed
    └─ "Ask every time"      → Show 3 s LVGL pop-up; OK defaults per pairing history
```

**Runtime switch A ↔ B (Settings → USB Mode):**

- A → B: call `tud_init(BOARD_TUD_RHPORT)`; resume `USBTask`. Host sees new device
  enumerate within ~2 s (no reboot).
- B → A: flush CDC TX queue; call `tud_disconnect()`; suspend `USBTask`. Host sees the
  COM port disappear within ~300 ms.

**Byte-bridge data flow (Mode B steady state):**

```
  PC ──► (CDC OUT) ──► TinyUSB ── CDC RX queue ──► USBTask
          → ipc_ring_push(CMD, IPC_MSG_SERIAL_BYTES, payload≤256)
          → SIO doorbell ──► Core 0 SerialConsole hook consumes bytes
                             drives Meshtastic admin CLI
  Core 0 ──► IPC_MSG_SERIAL_BYTES (DATA) ──► USBTask → CDC IN ──► PC
```

The 256 B slot size matches `IPC_MSG_PAYLOAD_MAX` and allows TinyUSB's 64 B packets to
batch into a single IPC slot, which is the throughput optimization from M1 Part B.

**Relationship to safe mode (§9.4):** safe mode forces **Mode B** regardless of setting.
When the device cannot boot normally, the PC is the only recovery surface; charge-only
would strand the user.

---

## 5. Shared IPC — 24 KB Byte Map

All offsets are relative to `0x2007A000`. The layout is declared once in
`firmware/shared/ipc/ipc_shared_layout.h` and both ELF images place a `.shared_ipc` section
at this exact address (NOLOAD).

| Offset     | Size     | Field                      | Notes                                             |
|------------|----------|----------------------------|---------------------------------------------------|
| `0x0000`   | 16 B     | Handshake                  | `boot_magic = "MOKY"`, `c0_ready`, `c1_ready`, `flash_lock` |
| `0x0010`   | 16 B     | `_pad`                     | alignment to 0x20                                 |
| `0x0020`   | 32 B     | `c0_to_c1_ctrl`            | DATA ring control (head/tail/stats)               |
| `0x0040`   | 32 B     | `c0_log_to_c1_ctrl`        | LOG  ring control                                 |
| `0x0060`   | 32 B     | `c1_to_c0_ctrl`            | CMD  ring control                                 |
| `0x0080`   | 8,448 B  | `c0_to_c1_slots[32]`       | 32 × 264 B — protobuf frames                      |
| `0x2180`   | 4,224 B  | `c0_log_to_c1_slots[16]`   | 16 × 264 B — log lines (best-effort)              |
| `0x3200`   | 8,448 B  | `c1_to_c0_slots[32]`       | 32 × 264 B — host commands                        |
| `0x5300`   | 260 B    | `gps_buf`                  | NMEA double-buffer (see §5.3)                     |
| `0x5404`   | 3,068 B  | `_tail_pad`                | reserved — breadcrumbs at `0x5FC0` (64 B)         |
| **Total**  | **24 KB**|                            |                                                   |

### 5.1 Three rings — why three, and slot counts

Core 0's `writeStream()` loop on a `want_config_id` interleaves ~40 protobuf frames with
`LOG_DEBUG/LOG_INFO` lines. On a single ring the log bytes consume slots that the protobuf
stream needs, creating 12–49 ms per-frame gaps. Splitting into three rings:

- **`c0_to_c1` (DATA, 32 slots, blocking on full)** — protobuf only. Producer blocks if
  full; consumer (Core 1 `IPCRxTask`) drains with priority.
- **`c0_log_to_c1` (LOG, 16 slots, best-effort)** — `IPC_MSG_LOG_LINE` only.
  **If the ring is full, the producer drops the line rather than blocking.** Prevents log
  starvation of the data path.
- **`c1_to_c0` (CMD, 32 slots)** — commands from Core 1 (send text, set config, etc.).

Each slot is an `IpcRingSlot` of 264 bytes: `IpcMsgHeader` (4 B) + `uint8_t payload[256]` +
4 B ring metadata.

### 5.2 Message catalogue — direction and owner

Enumerated in `firmware/shared/ipc/ipc_protocol.h`. **Direction** indicates the producer.

| ID     | Name                   | Dir   | Ring  | Purpose                                    |
|--------|------------------------|-------|-------|--------------------------------------------|
| `0x01` | `IPC_MSG_RX_TEXT`      | C0→C1 | DATA  | Incoming text message received             |
| `0x02` | `IPC_MSG_NODE_UPDATE`  | C0→C1 | DATA  | Node list entry added/updated              |
| `0x03` | `IPC_MSG_DEVICE_STATUS`| C0→C1 | DATA  | Periodic status (batt, GPS, RSSI, uptime)  |
| `0x04` | `IPC_MSG_TX_ACK`       | C0→C1 | DATA  | Tx ACK (sending/delivered/failed)          |
| `0x05` | `IPC_MSG_CHANNEL_UPDATE`| C0→C1| DATA  | Channel config changed                     |
| `0x06` | `IPC_MSG_SERIAL_BYTES` | C0→C1 | DATA  | Raw CLI bytes (M1 byte bridge)             |
| `0x07` | `IPC_MSG_CONFIG_VALUE` | C0→C1 | DATA  | Config get reply / unsolicited push        |
| `0x08` | `IPC_MSG_CONFIG_RESULT`| C0→C1 | DATA  | Config set/commit OK/err                   |
| `0x09` | `IPC_MSG_REBOOT_NOTIFY`| C0→C1 | DATA  | Core 0 about to reboot — detach USB        |
| `0x81` | `IPC_CMD_SEND_TEXT`    | C1→C0 | CMD   | Send text message                          |
| `0x82` | `IPC_CMD_SET_CHANNEL`  | C1→C0 | CMD   | Set active channel                         |
| `0x83` | `IPC_CMD_SET_TX_POWER` | C1→C0 | CMD   | LoRa TX power (dBm)                        |
| `0x84` | `IPC_CMD_REQUEST_STATUS`| C1→C0| CMD   | Immediate DEVICE_STATUS push               |
| `0x85` | `IPC_CMD_SET_NODE_ALIAS`| C1→C0| CMD   | Assign user alias to node ID               |
| `0x86` | `IPC_CMD_POWER_STATE`  | C1→C0 | CMD   | Notify power FSM transition                |
| `0x87` | `IPC_CMD_REBOOT`       | C1→C0 | CMD   | Request reboot                             |
| `0x88` | `IPC_CMD_FACTORY_RESET`| C1→C0 | CMD   | Wipe persistent config                     |
| `0x89` | `IPC_CMD_GET_CONFIG`   | C1→C0 | CMD   | Request config value by key                |
| `0x8A` | `IPC_CMD_SET_CONFIG`   | C1→C0 | CMD   | Set config value by key                    |
| `0x8B` | `IPC_CMD_COMMIT_CONFIG`| C1→C0 | CMD   | Commit pending (save + reboot if needed)   |
| `0xF0` | `IPC_MSG_LOG_LINE`     | both  | LOG   | Debug log, best-effort                     |
| `0xFE` | `IPC_MSG_PANIC`        | both  | LOG   | Cross-core panic notification (M6)         |
| `0xFF` | `IPC_BOOT_READY`       | both  | —     | Handshake (also written to shared region)  |

### 5.3 Ring API

```c
bool ipc_ring_push(IpcRingCtrl *ctrl, IpcRingSlot *slots,
                   uint32_t slot_count,      // 32 for data/cmd, 16 for log
                   uint8_t msg_id, uint8_t seq,
                   const void *payload, uint16_t payload_len);

bool ipc_ring_pop (IpcRingCtrl *ctrl, IpcRingSlot *slots,
                   uint32_t slot_count,
                   IpcRingSlot *out);
```

The `slot_count` parameter lets one implementation serve all three rings.

### 5.4 GPS double-buffer (out-of-band, not a ring)

`IpcGpsBuf` at `0x5300`: `uint8_t buf[2][128]` + a single-byte `write_idx` (0 or 1) that
the writer (Core 1 `GPSTask`) flips atomically after a full NMEA sentence is committed. The
reader (Core 0 `PositionModule`) always reads `buf[write_idx ^ 1]`, so producer and consumer
never touch the same slot. No locking, no doorbell needed — the `write_idx` flip *is* the
signal.

### 5.5 End-to-end flows

**A. Sending a text from UI → LoRa air:**

```
  User types in IMEBar ──► MIE emits UTF-8
    ──► MessagesApp.send()
        ──► IpcClient.sendText(to, chan, text)
             ipc_ring_push(CMD, IPC_CMD_SEND_TEXT, IpcPayloadText)
             SIO FIFO push (doorbell) ────────────────────────┐
                                                              ▼
  Core 0 SIO_IRQ_BELL ──► IPCPhoneAPI reads CMD ring
    ──► PhoneAPI.handleToRadio(ToRadio{...SendText})
        ──► MeshService.sendToMesh()
            ──► Router.send()
                ──► SX126xInterface.send() ──► SPI1 ──► SX1262 air
  Core 0: on tx complete, IPC_MSG_TX_ACK ──► DATA ring ──► UI toast
```

**B. Incoming text from LoRa → UI:**

```
  SX1262 RX IRQ ──► SX126xInterface.handleDio1()
    ──► Router.receive() ──► TextMessageModule.handleReceived()
        ──► IPCPhoneAPI.sendOnToRadio(ToRadio{FromRadio{RxText}})
             ipc_ring_push(DATA, IPC_MSG_RX_TEXT)
             SIO FIFO push ──────────────────────────────────┐
                                                             ▼
  Core 1 SIO_IRQ_BELL ──► IPCRxTask notified (task-level)
    ──► ipc_ring_pop(DATA) ──► MessageModel.append()
        ──► xQueueSend(UIQueue)
            ──► UITask drains ──► LVGL redraw ──► LCD DMA
```

**C. GPS NMEA pipe (no ring involved):**

```
  Core 1 GPSTask (100 ms):
    i2c1 read from Teseo 0x3A ──► NMEA parser
    on full sentence:
      idx = g_gps.write_idx ^ 1
      memcpy(g_gps.buf[idx], sentence, len)
      g_gps.len[idx] = len
      __dmb()
      g_gps.write_idx = idx      // commit

  Core 0 PositionModule runOnce (1 s):
    idx = g_gps.write_idx ^ 1
    parse g_gps.buf[idx] ──► Position protobuf
    emit via mesh + IpcPhoneAPI
```

**D. Config read/write round trip (M4+):**

```
  SettingsApp.read(KEY)
    ──► IpcClient.getConfig(KEY)
        ipc_ring_push(CMD, IPC_CMD_GET_CONFIG, {key})
                                                ▼
  Core 0 ConfigAdapter (IPCPhoneAPI extension):
    translate IPC key ──► AdminModule get-config
    reply IPC_MSG_CONFIG_VALUE ──► DATA ring
                                                ▼
  Core 1 IPCRxTask ──► ConfigModel.update(key, bytes)
    ──► SettingsApp refreshes widget
```

### 5.6 Doorbell mechanism

RP2350 SIO inter-core FIFO is used as a notification doorbell, **not** as data transport.
After pushing a ring slot, the sender writes a single 32-bit word to the FIFO; the receiver
takes an `SIO_IRQ_BELL` interrupt that wakes its ring-drain task (Core 1) or schedules a
runLoop check (Core 0). Payload data always travels through shared SRAM.

Notes:
- `SIO_IRQ_BELL` is shared across all doorbells — the ISR must clear only the bit it owns
  and leave others set (see `project_sio_irq_bell_shared` memory).
- During flash write (P2-11 fix), Core 1 parks on a RAM-resident busy loop and the
  doorbell bit for "park release" is the sole IRQ Core 1 watches.

### 5.7 MIE user-dictionary persistence (planned, M5+)

SRS §5.4 requires the MIE smart-correction layer to persist learned per-user weights
so that frequently-used candidates promote on repeat sessions. LittleFS lives in flash
region `0x10C00000 – 0x10FFFFFF` (§2.1) and is owned exclusively by Core 0 under the
single-writer flash-safety discipline (§6.7). MIE runs on Core 1, so every user-dict
write must tunnel through IPC.

**Proposed message additions (reserved IDs, pending implementation at M5):**

| ID     | Name                          | Dir   | Ring  | Payload                                   |
|--------|-------------------------------|-------|-------|-------------------------------------------|
| `0x0A` | `IPC_MSG_USER_DICT_ACK`       | C0→C1 | DATA  | `uint16_t chunk_id`, `uint8_t result`     |
| `0x0B` | `IPC_MSG_USER_DICT_CHUNK`     | C0→C1 | DATA  | boot-time streaming of stored user-dict entries to Core 1 |
| `0x8C` | `IPC_CMD_USER_DICT_PUT`       | C1→C0 | CMD   | `uint16_t chunk_id` + up to 240 B of serialised `(key_hash32, delta_i16)` tuples |
| `0x8D` | `IPC_CMD_USER_DICT_COMMIT`    | C1→C0 | CMD   | Empty — flush outstanding chunks to flash |
| `0x8E` | `IPC_CMD_USER_DICT_LOAD`      | C1→C0 | CMD   | Empty — request full-file stream at boot  |

The IDs `0x0A`, `0x0B`, `0x8C`..`0x8E` are currently unused in `IpcMsgId`; they will be
reserved in `ipc_protocol.h` at M5 kickoff.

**Flow — online learning:**

```
Core 1 IMETask accumulates dirty weights (candidate commits promoting tier)
  ──► dirty_count > 32  OR  idle ≥ 30 s:
        serialise as (key_hash32, delta_i16) tuples (6 B each)
        split into ≤ 240 B chunks (~40 tuples per chunk)
        for each chunk_id: ipc_ring_push(CMD, IPC_CMD_USER_DICT_PUT, ...)
        last chunk followed by ipc_ring_push(CMD, IPC_CMD_USER_DICT_COMMIT)
Core 0 UserDictStore handler:
  LittleFS open "/mie/user.dict" (append mode)
  for each received chunk: decode tuples, append log record
  on COMMIT: fsync via flash_safety_wrap
  reply IPC_MSG_USER_DICT_ACK{chunk_id, result} per chunk (DATA ring)
Core 1 on ERR ACK: retry once; on second failure emit toast + stop learning
  until next boot (writes dropped rather than blocking IME).
```

**Flow — boot-time reload:**

```
Core 1 boot (after c0_ready + c1_ready):
  IMETask → ipc_ring_push(CMD, IPC_CMD_USER_DICT_LOAD)
Core 0:
  read "/mie/user.dict" record-by-record;
  batch records into ≤ 240 B IPC_MSG_USER_DICT_CHUNK frames;
  final chunk has a sentinel flag (high bit of chunk_id)
Core 1 applies each record as a live weight overlay on the PSRAM dict.
```

**Storage format (`/mie/user.dict`):**

- Append-only log of 6-byte records: `uint32 key_hash + int16 delta_weight`.
- File is rewritten (compacted) as a deduplicated snapshot every 1024 records, or when
  the file exceeds 128 KB. Compaction runs entirely on Core 0 during an idle window.

**Why not let Core 1 write flash directly?**
P2-11 discipline requires a "park other core + disable IRQ" dance around every
`flash_range_erase` / `flash_range_program` (XIP is off while flash is writing). Having
a single flash writer keeps the protocol one-way and simple. Two-way flash ownership
would require each core to park the other, which doubles the complexity and introduces
a possible live-lock surface.

---

## 6. Cross-Image Non-Interference

Two independently-linked ELF images share the chip. The following mechanisms guarantee
they cannot corrupt each other's memory at compile-time, link-time, boot-time, or runtime.

### 6.1 Link-time — matching NOLOAD region + nm verification

Both `patch_arduinopico.py` (Core 0) and `memmap_core1_bridge.ld` (Core 1) declare the
identical `SHARED_IPC` MEMORY region:

```
SHARED_IPC (rw) : ORIGIN = 0x2007A000, LENGTH = 24K
```

Both emit `.shared_ipc (NOLOAD)` at `0x2007A000`. Post-link CI check: `nm` both ELFs and
assert `g_ipc_shared` resolves to the identical address. Any layout drift between the two
headers becomes a build-break, not a silent memory stomp.

### 6.2 Boot model — no IMAGE_DEF on Core 1

Core 1's linker script `/DISCARD/`s `.embedded_block`, `.embedded_end_block`, `.boot2`, and
`.binary_info_header`. Core 1's image therefore has **no IMAGE_DEF header** at word 0;
instead, the first two flash words are the M33 vector table — `SP = __StackTop`,
`PC = mokya_core1_reset_handler` (Thumb bit set).

Core 0 boots Core 1 with `multicore_launch_core1_raw(pc, sp, vtor)`, which reads SP and PC
directly from those two flash words. If Core 1 ever regained an IMAGE_DEF block it would
overwrite word 0 with `0xffffded3` and instantly fault on launch.

Core 1's SDK runtime_init hooks are also selectively disabled so Core 1 doesn't re-init
clocks or resets that Core 0 already owns:

```
PICO_RUNTIME_SKIP_INIT_CLOCKS=1
PICO_RUNTIME_SKIP_INIT_EARLY_RESETS=1
PICO_RUNTIME_SKIP_INIT_POST_CLOCK_RESETS=1
PICO_RUNTIME_SKIP_INIT_SPIN_LOCKS_RESET=1
PICO_RUNTIME_SKIP_INIT_BOOT_LOCKS_RESET=1
PICO_RUNTIME_SKIP_INIT_BOOTROM_LOCKING_ENABLE=1
PICO_RUNTIME_SKIP_INIT_USB_POWER_DOWN=1
```

### 6.3 SRAM non-overlap

Core 0 memmap has `LENGTH = 176 KB` starting at `0x20000000`. Core 1 memmap has
`LENGTH = 312 KB` starting at `0x2002C000`. Regions are back-to-back, zero gap, zero
overlap. The Core 1 linker asserts `__StackLimit >= __HeapLimit` inside its region. Core 1
never places data in `SCRATCH_X`/`SCRATCH_Y`; those 8 KB belong to Core 0.

### 6.4 Ring ownership — SPSC partition

Each SPSC ring is strictly single-producer, single-consumer:

- **`head`** is written only by the producer. Consumer reads it.
- **`tail`** is written only by the consumer. Producer reads it.
- **`slots[i]`** is owned by the producer while `(head - tail) < slot_count`; ownership
  transfers to the consumer when the producer publishes `head++`.

No shared mutable state is written by both sides. No spinlock, no atomic RMW.

### 6.5 Memory ordering

Every `head`/`tail` publish is surrounded by `__dmb()`:

- Producer: write `slots[head % N]` → `__dmb()` → `head++` → `__dmb()` → FIFO push.
- Consumer: read FIFO → `__dmb()` → read `head` → read `slots[tail % N]` → `__dmb()` →
  `tail++`.

### 6.6 GPS double-buffer atomicity

`write_idx` is a single `uint8_t` — reads and writes are naturally atomic on Cortex-M33.
The reader always reads `buf[write_idx ^ 1]`, guaranteeing it never observes an in-progress
write.

### 6.7 Flash-write safety (P2-11 discipline)

Only Core 0 writes flash. Before every `flash_range_erase` / `flash_range_program`, Core 0:
1. Raises the "park" doorbell bit to Core 1.
2. Spins until Core 1 acks that it has entered its RAM-resident park loop.
3. Disables local IRQs.
4. Calls the real function (XIP off during this window).
5. Re-enables IRQs, releases Core 1.

See `flash_safety_wrap.c`. Core 1's park loop is in `.data` (copied to RAM at boot), so it
runs without XIP.

---

## 7. Bus & Peripheral Ownership

Each peripheral has exactly one owner. No register of a foreign-owned peripheral is ever
read or written across the IPC boundary.

| Bus / Peripheral                | Owner  | Devices / Pins                                             |
|---------------------------------|--------|------------------------------------------------------------|
| Sensor bus (`i2c1`, GPIO 34/35) | Core 1 | LSM6DSV16X 0x6A · LIS2MDL 0x1E · LPS22HH 0x5D · Teseo 0x3A |
| Power  bus (`i2c1`, GPIO 6/7)   | Core 1 | BQ25622 0x6B · BQ27441 0x55 · LM27965 0x36                 |
| SPI1 (GPIO 24–27)               | Core 0 | SX1262 LoRa                                                |
| PIO0                            | Core 1 | LCD 8-bit 8080 parallel, NHD-2.4″                          |
| PIO1                            | Core 1 | 6×6 keypad matrix, GPIO 36–47                              |
| PWM                             | Core 1 | Haptic motor (GPIO 9)                                      |
| USB CDC                         | Core 1 | TinyUSB device — console + bridged CLI                     |
| Flash (QMI / XIP)               | shared | XIP read both cores; writes only Core 0                    |
| PSRAM (QMI / XIP)               | Core 1 | read only; bump-allocator heap + dict assets               |

Note: both I2C-like buses are `i2c1` in the Pico SDK (GPIO 6/7 and GPIO 34/35 are alternate
pin pairs on the same peripheral). Core 1 time-multiplexes `i2c1` between the two pin sets
by reinitialising the controller.

---

## 8. Build System & Toolchain Isolation

The dual-licence boundary is reinforced by using **two separate build tools**: Core 0 uses
PlatformIO (to stay on the same toolchain as upstream Meshtastic), Core 1 uses CMake+Ninja
(to track the Pico SDK directly without Arduino-Pico). Neither build can accidentally link
the other's objects.

### 8.1 Core 0 build — PlatformIO

Config: `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/platformio.ini`.

- Extends `rp2350_base` (upstream Meshtastic RP2350 variant).
- Pre-scripts:
  - `patch_arduinopico.py` — applies the five framework patches (see §3.2). Idempotent;
    CI re-runs safely.
  - `wire_shared_ipc.py` — injects the `.shared_ipc` NOLOAD region into the Arduino-Pico
    default linker script at build time.
- Injects `-D NO_USB` + `-D configNUMBER_OF_CORES=1`, adds the `MESHTASTIC_EXCLUDE_*`
  trim list.
- Wraps flash functions: `-Wl,--wrap=flash_range_erase -Wl,--wrap=flash_range_program`.
- `lib_ignore`s `BluetoothOTA`, `lvgl`, `SdFat`, `SD` — none belong on Core 0.
- Output: `firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware.elf` — flashed at
  `0x10000000`.

### 8.2 Core 1 build — CMake + Ninja

Config: `firmware/core1/m1_bridge/CMakeLists.txt`.

- `PICO_PLATFORM=rp2350-arm-s`, `PICO_BOARD=pico2` (closest match for RP2350B).
- Pulls in FreeRTOS-Kernel + `RP2350_ARM_NTZ` community port as subdirectory.
- Linker script override: `pico_set_linker_script(core1_bridge memmap_core1_bridge.ld)`.
- `pico_add_extra_outputs()` is **not** called (would invoke picotool and need IMAGE_DEF).
  Instead a POST_BUILD custom command `objcopy -O binary` produces
  `build/core1_bridge/core1_bridge.bin` — flashed raw at `0x10200000`.
- `core1_reset.S` is listed **first** in `add_executable()` so its `.vectors` input section
  lands at offset 0 of the output `.text`. The linker script `KEEP (*core1_reset.S.obj(.vectors))`
  belts-and-braces this.
- Compile defs disable SDK runtime_init hooks that would fight Core 0 for hardware state
  (see §6.2 list).
- Libraries: `pico_stdlib`, `hardware_resets`, `hardware_irq`, `hardware_sync`,
  `hardware_exception`, `tinyusb_device`, `FreeRTOS-Kernel`, `FreeRTOS-Kernel-Heap4`.
  **Not** linked: `pico_multicore` — doorbell uses raw SIO register writes to avoid
  multicore init side-effects.

### 8.3 Why Core 0 ships as ELF but Core 1 ships as raw `.bin`

The two images use different flash-load artefacts. This asymmetry is deliberate and
follows from the boot model, not a build-system quirk.

**Semantic — Core 1 is not a bootable Pico image.**
Core 0 boots via the RP2350 bootrom, which validates an IMAGE_DEF block at `0x10000000`.
ELF is the natural format: program headers carry the load address, and the IMAGE_DEF lives
inside a regular `.text` input section.

Core 1's linker script `/DISCARD/`s `.embedded_block` / `.binary_info_header` / `.boot2`
(§6.2). If Core 1 carried an IMAGE_DEF, its magic word would overwrite word 0 of the image
— but word 0 must be `SP = __StackTop` for `multicore_launch_core1_raw()` to read. So
Core 1 is semantically **a payload blob at a fixed offset**, not a bootable image. Using
`.bin` reflects that — there is no header, no metadata, just bytes destined for
`0x10200000`.

**Tooling — picotool needs IMAGE_DEF, which Core 1 has explicitly removed.**
`pico_add_extra_outputs()` would normally emit `.uf2` via picotool, but picotool refuses
to process an image without IMAGE_DEF. The CMakeLists therefore skips that call and runs
`objcopy -O binary` in a POST_BUILD step to produce `core1_bridge.bin` directly. Downstream
UF2 conversion (`picotool uf2 convert --offset 0x10200000`, §10.2) takes the `.bin` plus
an explicit offset — bypassing IMAGE_DEF inference entirely.

**Operational — explicit address is auditable.**
J-Link's `loadbin core1_bridge.bin 0x10200000` writes the load address on the same line as
the file. Using `loadfile core1_bridge.elf` would technically work (J-Link reads program
headers) but the address would be buried inside the linker script. For an image whose
load-bearing property **is** its offset, surfacing the offset in the flash command makes
mistakes harder.

**Core 1 ELF still exists.**
`build/core1_bridge/core1_bridge.elf` is produced by the linker and retained for
**debugging only** (GDB symbol load, SWD source-level stepping, `nm` for the
`g_ipc_shared` coherency check in §6.1 / §11.5). It is never written to flash.

Summary table:

| Artefact                 | Flash?     | Purpose                                              |
|--------------------------|------------|------------------------------------------------------|
| `firmware.elf` (Core 0)  | yes        | J-Link `loadfile`, debug symbols, UF2 source         |
| `firmware.uf2` (Core 0)  | yes (BOOTSEL) | End-user drag-drop install                         |
| `core1_bridge.elf`       | **no**     | Debug symbols, `nm` coherency checks only            |
| `core1_bridge.bin`       | yes        | J-Link `loadbin 0x10200000`, UF2 source (raw bytes)  |
| `core1.uf2`              | yes (BOOTSEL) | Produced from `.bin` + explicit `--offset`         |

### 8.4 Licence boundary enforcement at build

| Rule                                                                         | Enforced by                                             |
|------------------------------------------------------------------------------|---------------------------------------------------------|
| Core 1 cannot `#include` any Meshtastic header                               | Separate include paths; no Meshtastic path in CMake     |
| `firmware/shared/ipc/` may only `#include <stdint.h>` / `<stddef.h>`         | Code review + compile (`ipc_protocol.h` is checked)     |
| Core 0 cannot link Core 1 objects                                            | PlatformIO `build_src_filter` excludes `firmware/core1/`|
| Core 1 cannot link Core 0 objects                                            | CMake `add_executable()` list is explicit               |
| `nm g_ipc_shared` must match on both ELFs                                    | CI post-link check                                      |
| Layout drift in `ipc_shared_layout.h` breaks both builds                     | Single source of truth for offsets                      |

### 8.5 Dual-image build + flash (`scripts/build_and_flash.sh`)

```
  ┌─ Core 0 ─────────────────────────────────────────────────────────┐
  │ python -m platformio run -e rp2350b-mokya                        │
  │   -d firmware/core0/meshtastic                                   │
  │ → firmware.elf  (flash at 0x10000000)                            │
  └──────────────────────────────────────────────────────────────────┘
  ┌─ Core 1 ─────────────────────────────────────────────────────────┐
  │ cmake --build build/core1_bridge                                 │
  │ → core1_bridge.bin  (flash at 0x10200000)                        │
  └──────────────────────────────────────────────────────────────────┘
  ┌─ Flash via J-Link ───────────────────────────────────────────────┐
  │ JLink.exe -device RP2350_M33_0 -if SWD -speed 4000 \             │
  │   -CommanderScript <<EOF                                         │
  │     connect                                                      │
  │     r                                                            │
  │     loadfile  firmware.elf                                       │
  │     loadbin   core1_bridge.bin 0x10200000                        │
  │     r                                                            │
  │     g                                                            │
  │     qc                                                           │
  │   EOF                                                            │
  └──────────────────────────────────────────────────────────────────┘
```

Variants:

- `bash scripts/build_and_flash.sh` — full dual rebuild + flash both slots.
- `bash scripts/build_and_flash.sh --core1` — skip PlatformIO, rebuild + flash Core 1 only.
  Used when iterating on UI / IME / HAL without touching Meshtastic. ~4× faster.
- Core 0 only — run `pio run` directly and `loadfile firmware.elf` by hand.

After flash, USB CDC needs ~3 s to re-enumerate before `python -m meshtastic --port COMxx
--info` will succeed.

### 8.6 Core 1 CMake configure (one-time)

```
PICO_SDK_PATH=/c/pico-sdk cmake -S firmware/core1/m1_bridge \
    -B build/core1_bridge -G Ninja
```

Requires: VS Build Tools 2019 (MSVC for CMake host tools), ARM GCC (Thumb target),
Ninja, Pico SDK at `C:\pico-sdk`, FreeRTOS-Kernel submodule initialised.

---

## 9. Boot Sequence

Two cores, two timelines — Core 0 launches Core 1 and both then race to mark themselves
ready in the shared handshake block. Time flows downward.

```
    CORE 0                                        CORE 1
    ─────────────────────────────────             ─────────────────────────────
    Power-on / reset                              (halted — no clock yet)
      │
      ▼
    Bootrom
      │ reads IMAGE_DEF at 0x10000000
      ▼
    Arduino-Pico crt0
      │ clocks · resets · QSPI XIP on
      │ .data copy · .bss clear
      ▼
    Core 0 main()
      │ (1) ipc_shared_init()
      │     — zero 24 KB, write "MOKY" magic,
      │       clear c0_ready / c1_ready
      │ (2) multicore_launch_core1_raw(
      │         pc   = word[1] @ 0x10200000,
      │         sp   = word[0] @ 0x10200000,
      │         vtor = 0x10200000 ) ───────────► Core 1 wakes
      │                                              │
      │ (3) setup() — Meshtastic init                ▼
      │     SX1262, NodeDB, LittleFS,            Core 1 crt0
      │     IPCPhoneAPI                              │ selected runtime_init_*
      │                                              │ (no clocks, no resets)
      │ (4) busy-wait on                             ▼
      │     g_ipc_shared.c1_ready                Core 1 main()
      │     (timeout 2 s → set                       │ (1) hardware_exception setup
      │      g_ipc_shared.c1_failed,                 │ (2) tusb_init() → USB CDC
      │      continue without UI)                    │ (3) ipc_client_init()
      │                                              │     install SIO_IRQ_BELL ISR
      │                    ◄──── c1_ready = 1 ──     │ (4) publish c1_ready
      │                                              │ (5) vTaskStartScheduler()
      │ (5) c0_ready = 1 ────────►                   │     preemption begins
      │                                              ▼
      ▼                                          FreeRTOS tasks running
    OSThread loop() runs Meshtastic
      ─────────────── steady state ───────────────
      Core 0 and Core 1 exchange ring frames + doorbells (§5.5)
```

### 9.1 Handshake

- `boot_magic = "MOKY"` distinguishes a warm IPC region from uninitialised SRAM. Either
  core can detect a missing magic and skip stale-ring assumptions on first boot.
- Each core writes its `*_ready` flag after its transport is fully up. Once both flags are
  set, normal IPC is permitted.
- `IPC_BOOT_READY` may additionally be pushed on the LOG ring as a human-readable marker
  for the CDC console.

### 9.2 Failure modes

| Condition                                   | Core 0 action                               | User-visible effect                         |
|---------------------------------------------|---------------------------------------------|---------------------------------------------|
| Core 1 fails to set `c1_ready` within 2 s   | Set `c1_failed`; continue headless          | LoRa modem runs; LCD stays off              |
| `g_ipc_shared.boot_magic != "MOKY"`         | Reinit shared region; re-launch Core 1 once | Transient LCD blank, recovers within ~1 s   |
| Watchdog reset loop (>3 hits in 10 s)       | Enter safe mode — CDC only, radio off       | USB CDC responds to admin only              |

### 9.3 Breadcrumbs (post-mortem trace)

The final 64 B of the shared region (`0x2007FFC0 – 0x2007FFFF`) is a ring of one-byte
event codes written by both cores at key state transitions (boot stage, IRQ source, flash
lock acquire/release, task panic). On a watchdog-induced reset the region survives, and
the safe-mode console dumps it as the first post-mortem artefact. Codes are defined in
`ipc_shared_layout.h`.

### 9.4 Watchdog Discipline & Safe Mode

Hardware: the single RP2350 watchdog runs an 8 s timeout and is fed by Core 1's
`IdleHook` (§4.2). Core 0 does not feed the WDT directly — the WDT instance is claimed
by Core 1, and any Core 1 deadlock > 8 s (UI task starving IdleHook, or a task spinning
in a critical section) resets BOTH cores via chip reset. Core 0 has its own OSThread
watchdog watchdog'd by internal heartbeats; if it deadlocks, Core 1 stops receiving IPC
frames and the LCD posts a "radio disconnected" toast within 2 s, but this does not
reset the chip.

**Boot counter — `WATCHDOG_SCRATCH[0..3]`:**

On the reset path Core 0's earliest crt0 reads the four watchdog scratch registers,
which survive a warm reset but are cleared on cold power cycle. The layout is:

| Scratch | Purpose                                                      |
|---------|--------------------------------------------------------------|
| `[0]`   | `"MOKY"` magic — if absent, treat all fields below as zero   |
| `[1]`   | Consecutive WDT-triggered boots (clears on clean 10 s uptime)|
| `[2]`   | Reserved — last-known-good breadcrumb offset                 |
| `[3]`   | Pending-mode flag: 0 normal, 1 OTA-BOOTSEL, 2 safe-mode-lock |

If `SCRATCH[1] ≥ 3` **or** `SCRATCH[3] == 2`, boot diverts to safe mode instead of
normal init. A clean 10 s uptime in normal mode clears `SCRATCH[1]` back to 0.

**Safe-mode service matrix:**

| Service                     | State in safe mode | Rationale                                     |
|-----------------------------|--------------------|-----------------------------------------------|
| Core 0 Meshtastic stack     | minimal init       | bring up SPI1 + NodeDB read-only; no Tx       |
| LoRa radio (SX1262)         | OFF                | eliminate radio IRQ as WDT trigger            |
| Core 1 LVGL UI task         | suspended          | eliminate LVGL deadlock as WDT trigger        |
| Core 1 `IPCRxTask`          | running            | required to serve CDC console                 |
| Core 1 `USBTask` + TinyUSB  | running (Mode B forced) | primary out-of-band recovery path        |
| J-Link SWD                  | always on          | hardware — independent of firmware state      |
| LCD (PIO + DMA)             | OFF                | avoid any PIO/DMA path implicated in fault    |
| Keypad scan (PIO1)          | OFF                | only PWR key read via direct GPIO poll        |
| Sensor / Power I2C polling  | 5 s cadence        | battery status still reported over CDC        |
| Flash writes                | ALLOWED            | needed to clear config that caused fault      |
| Watchdog                    | disabled           | prevent loop if recovery command takes > 8 s  |

**User-visible indicators:**

- Status LED (LM27965 channel A) runs a distinctive 0.5 Hz red slow-blink pattern.
- CDC banner on enumeration: `MOKY-SAFE-MODE rev=<git-sha> wdt_count=<N>` followed by
  a `breadcrumb` dump from §9.3.
- No LCD output — by design: user must connect a PC + CDC to recover.

**Exit paths from safe mode:**

| Trigger                                             | Action                                              |
|-----------------------------------------------------|-----------------------------------------------------|
| CDC command `safe-mode exit`                        | Clear `SCRATCH[1,3]`; reboot                        |
| CDC command `factory-reset`                         | Wipe LittleFS; clear `SCRATCH[1,3]`; reboot         |
| CDC command `reboot-ota`                            | Set `SCRATCH[3]=1`; reboot into BOOTSEL (§10.5)     |
| Hold PWR 10 s continuously                          | Clear `SCRATCH[1,3]`; reboot                        |
| USB disconnect for > 60 s                           | Assume attempt made; reboot once into normal mode   |

**Instrumentation:** the breadcrumb ring (§9.3) is dumped over CDC immediately after
the banner. This is the primary post-mortem tool — any deadlock or fault visible through
the breadcrumb sequence is diagnosable remotely without J-Link access.

---

## 10. Release Distribution — UF2 Strategy

J-Link SWD is the development-time flashing path (§8.5). For end-user distribution
MokyaLora ships `.uf2` files that the RP2350 bootrom can write via drag-and-drop BOOTSEL
mass-storage mode — no J-Link required.

### 10.1 Why a combined UF2

UF2 blocks are 512 bytes each and **self-describe** their target flash address. That means
two UF2 streams covering non-overlapping address ranges can be concatenated into a single
file; the bootrom writes each block to its own target. MokyaLora's two slots are
`0x10000000` (Core 0) and `0x10200000` (Core 1) — non-overlapping — so one combined UF2 can
install both images in a single drag-drop.

### 10.2 Build flow (`scripts/build_uf2.sh`)

```sh
# Core 0 — ELF → UF2 (address taken from IMAGE_DEF, defaults to 0x10000000)
picotool uf2 convert \
    firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware.elf \
    build/dist/core0.uf2 \
    --family rp2350-arm-s

# Core 1 — raw .bin → UF2, MUST specify --offset because there is no IMAGE_DEF
picotool uf2 convert \
    build/core1_bridge/core1_bridge.bin \
    build/dist/core1.uf2 \
    --offset 0x10200000 \
    --family rp2350-arm-s

# Merge — each UF2 block is self-describing; cat is safe.
# Bootrom tolerates inconsistent numBlocks across concatenated streams.
cat build/dist/core0.uf2 build/dist/core1.uf2 > build/dist/mokyalora-full.uf2
```

### 10.3 Artefacts shipped

| File                    | Contents                         | Typical size | When to use                                   |
|-------------------------|----------------------------------|--------------|-----------------------------------------------|
| `core0.uf2`             | Core 0 only, target `0x10000000` | ~1.5 MB      | Iterating on Meshtastic without touching UI   |
| `core1.uf2`             | Core 1 only, target `0x10200000` | ~200–400 KB  | Iterating on UI/IME/HAL                       |
| `mokyalora-full.uf2`    | Core 0 + Core 1 concatenated     | ~1.5–2 MB    | First-time install, general end-user update   |

### 10.4 Hard rules

1. **Family ID = `rp2350-arm-s`** (`0xe48bff59`). Never use the `absolute` family — it
   would allow the UF2 to be written onto a mismatched RP2040/RP2350 variant.
2. **Core 1 UF2 must pass `--offset 0x10200000`.** Core 1's `.bin` has no IMAGE_DEF; the
   bootrom does not — and must not — infer its address from the image.
3. **Core 1 must stay without IMAGE_DEF** (see §6.2). The bootrom validates IMAGE_DEF only
   at `0x10000000` to decide which image to boot; raw bytes at `0x10200000` are simply
   programmed to flash and later launched by Core 0 via `multicore_launch_core1_raw()`.
4. **`.uf2` are byte-stream compatible** — do not run them through ELF-level strip/patch
   tools. If a block's target or payload changes, regenerate from the source ELF/BIN.

### 10.5 Entering BOOTSEL for in-place update

First-time users hold BOOTSEL during power-on. For in-place updates of a running device a
USB command is preferable. Two options, in order of effort:

- **Short-term (M5):** Meshtastic's existing `reboot_ota` admin command reboots Core 0;
  pair it with a Pico SDK `reset_usb_boot(0, 0)` call on reset path when a magic flag is
  set in the watchdog scratch register. User runs `meshtastic --reboot-ota` and the device
  re-appears as a BOOTSEL mass-storage drive.
- **Long-term (M6+):** full OTA via Meshtastic protocol. The 4 MB OTA region at
  `0x10400000` (see §2.1) stages a downloaded combined image, then a mini bootloader swaps
  slots on next reset. No BOOTSEL needed.

---

## 11. Licence Compliance for Binary Distribution

Shipping a combined `mokyalora-full.uf2` mixes GPL-3.0 (Core 0 / Meshtastic), Apache-2.0
(Core 1 / MIE), and MIT (shared IPC) in a single file. This is legally clean provided the
distribution is structured as **GPL-3.0 §5 mere aggregation**, not a combined work.

### 11.1 Why this is aggregation, not a combined work

The architecture already satisfies the separate-and-independent-works test:

- Separate flash slots, separate SRAM regions, separate ELF/BIN — no link-time overlap
- IPC is a wire protocol (shared-SRAM SPSC rings), structurally equivalent to UART / network
- Core 1 `#include`s zero Meshtastic headers; shared header is MIT only
- Two independent build tools (PlatformIO vs. CMake) — Core 0 objects cannot accidentally
  be linked into Core 1 or vice versa
- Each image is developed, built, and debugged independently. The MIE sub-library ships a
  full host-only build (`firmware/mie/tests`) that runs on PC without either core, proving
  Core 1's IME logic is not a derivative of Meshtastic code

UF2 concatenation is analogous to putting two independent packages onto the same ISO —
GPL-3.0 §5 explicitly permits this.

### 11.2 Hard rules that must not break (audit triggers)

| Rule                                                                      | Where enforced                              |
|---------------------------------------------------------------------------|---------------------------------------------|
| `firmware/shared/ipc/` headers `#include` only `<stdint.h>` / `<stddef.h>`| Code review; compile breakage if violated   |
| Core 1 image never `#include`s a Meshtastic header                        | Separate include paths; CMake sources list  |
| Core 0 image never `#include`s a Core 1 header                            | PlatformIO `build_src_filter`               |
| Release scripts never link Core 0 objects into Core 1 (or vice versa)     | `build_and_flash.sh`, `build_uf2.sh`        |
| The MIT shared header keeps MIT licence (compatible with both sides)      | File header + `LICENSE-MIT`                 |

A violation of any row promotes the two images from "aggregated" to "combined work", which
would force Apache-2.0 Core 1 code under GPL-3.0. Treat these as release blockers.

### 11.3 Release bundle contents

A distributable release zip alongside the UF2 MUST include:

| File                          | Source / notes                                                  |
|-------------------------------|-----------------------------------------------------------------|
| `mokyalora-full.uf2`          | combined image (plus optional `core0.uf2`, `core1.uf2`)         |
| `COPYING`                     | GPL-3.0 full text — applies to Core 0                           |
| `LICENSE-APACHE-2.0`          | applies to Core 1, MIE, shared code that is Apache              |
| `LICENSE-MIT`                 | applies to `firmware/shared/ipc/`                               |
| `NOTICE`                      | Apache-2.0 §4 attributions (Meshtastic + third-party NOTICEs too) |
| `THIRD_PARTY_LICENSES.md`     | RadioLib, FreeRTOS-Kernel, Pico SDK, TinyUSB, LVGL, Crypto, etc.|
| `WRITTEN_OFFER.txt`           | GPL-3.0 §6 three-year written offer for corresponding source    |
| `README.md`                   | Pointers to public source (Meshtastic fork + MokyaLora repo)    |
| `BUILD.md` (or link)          | How to rebuild the UF2 from source                              |

### 11.4 Source availability

Even though both repos are public on GitHub, GPL-3.0 §6 still requires a written offer
valid for three years from the distribution date. The `WRITTEN_OFFER.txt` must:

- Identify the exact Meshtastic fork branch (`tengigabytes/firmware@feat/rp2350b-mokya`)
  and commit hash built into the shipped binary
- Offer to provide the corresponding source on physical medium, or via a download URL
- Name a contact address that will remain valid for at least three years

The build system should embed the Core 0 git commit hash into the binary (`git describe`
at compile time) and print it in the release notes so downstream users can correlate a UF2
to an exact source tree.

### 11.5 Release pipeline additions

`scripts/build_uf2.sh` should, beyond building the UF2s:

1. Produce `build/dist/mokyalora-<version>.zip` containing all files in §11.3
2. Fail the build if `firmware/shared/ipc/` has grown any non-MIT `#include`
3. Fail the build if `nm core0.elf` and `nm core1_bridge.elf` disagree on
   `g_ipc_shared`'s address (same check already used during dev)
4. Embed `git describe --always --dirty` output for both repos into the zip's
   `VERSION.txt`

These checks are the automatic half of §11.2 — they catch licence-boundary drift before a
release is shipped.

---

## 12. Hardware Revision Boundary

MokyaLora firmware targets a specific hardware revision (currently Rev A), but the
architecture is intentionally partitioned between **revision-agnostic** components that
survive HW bumps unchanged and **Rev A-specific** components that will change or be
removed. This section is the forward-compatibility contract — any future HW revision
only needs to rewrite the "Rev A specific" column.

### 12.1 Revision-agnostic (survives HW bumps unchanged)

| Layer                     | Component(s)                                                     |
|---------------------------|------------------------------------------------------------------|
| Dual-core partition       | GPL Core 0 / Apache Core 1 separation (§1)                       |
| Flash + SRAM layout       | 2 MB / 2 MB firmware slots, 24 KB shared IPC region (§2)         |
| IPC protocol              | `ipc_protocol.h`, `ipc_shared_layout.h`, SPSC ring primitives (§5) |
| Boot model                | Core 0 ELF + Core 1 BIN + `multicore_launch_core1_raw` (§6, §9)  |
| Flash-write safety        | `flash_safety_wrap.c` discipline (§6.7)                          |
| Build tooling             | PlatformIO + CMake + `scripts/build_and_flash.sh` (§8)           |
| Licence compliance        | §11 aggregation rules and release pipeline                       |
| MIE engine                | `firmware/mie/` sub-library (Core 1 SERVICE layer, §4.1)         |
| UI framework              | LVGL + FreeRTOS tasks (§4.2)                                     |
| USB CDC byte bridge       | `IPC_MSG_SERIAL_BYTES` (§4.6, §5.5)                              |
| Watchdog / safe mode      | Scratch-register boot counter (§9.4)                             |

### 12.2 Rev A specific (may change or disappear)

| Layer / Component         | Rev A state                                                      | Expected in next revision |
|---------------------------|------------------------------------------------------------------|---------------------------|
| Audio frontend            | IM69D130 PDM mic + NAU8315 amp + CMS-131304 speaker populated    | **Removed.** No PDM PIO program, no I2S task; firmware already has no audio task — removing the HW drops ~0 KB of Core 1 RAM |
| Battery                   | Nokia BL-4C ~890 mAh (BQ27441 Design Capacity = 1000 mAh)        | Possibly larger; update `GaugeMonitor` init constant |
| Sensor bus mix            | IMU 0x6A · Mag 0x1E · Baro 0x5D · GPS 0x3A on `i2c1` GPIO 34/35  | Unchanged unless part numbers change |
| Power bus mix             | BQ25622 0x6B · BQ27441 0x55 · LM27965 0x36 on `i2c1` GPIO 6/7    | Unchanged |
| GNSS LNA / SAW            | BGA725L6 LNA + B39162B4327P810 SAW                               | Unchanged unless antenna retuned |
| Optional wireless         | None                                                             | Bluetooth may be added via companion nRF52 on SPI0 / I2C / UART. Core 0 already sets `-D MESHTASTIC_EXCLUDE_BLUETOOTH`, so the integration point would be Core 1 |
| USB transport             | TinyUSB CDC via RP2350 native USB                                | Unchanged |
| SWD / J-Link              | 3-pin test point                                                 | Unchanged |

### 12.3 Migration checklist (template for the next HW bump)

When producing Rev B (or later), run this checklist before Core 0 / Core 1 firmware is built:

1. Update `CLAUDE.md` Hardware section + GPIO table in `docs/design-notes/mcu-gpio-allocation.md`.
2. Update `docs/requirements/system-requirements.md` §2, §5, §8 (BOM, spec summary, BOM highlights).
3. Update `docs/requirements/hardware-requirements.md` power tree + keypad matrix if rails or
   switch layout changed.
4. If **audio is removed**: delete any PDM/I2S PIO program files; no firmware task to remove
   (none was ever created on Rev A). Update §12.2 to move audio row out of this table.
5. If **battery capacity** changes: update BQ27441 `Design Capacity` constant in
   `GaugeMonitor` init; re-run fuel-gauge learning cycle.
6. If a **new peripheral bus** is added: reserve the owner in §7 ownership table; allocate a
   new FreeRTOS task entry in §4.2 if polling is needed; extend `IpcConfigKey` if the
   peripheral exposes user-facing settings.
7. Re-run RAM usage SWD sweep (§2.2 Core 1 breakdown) and update this document.
8. Bump the firmware-architecture.md `Last updated` date at the top.
9. If the flash layout changes, update §2.1 **and** the two linker scripts; run the
   `nm g_ipc_shared` CI coherency check to catch any drift.

### 12.4 Forward compatibility guarantees

The shared IPC ABI (`ipc_protocol.h`) is **not** a backwards-compatible contract today.
When Core 0 and Core 1 images are built from incompatible ABI versions, results are
undefined. Until M6, the project ships the two images as a paired release (single
combined UF2 per §10), so ABI skew is avoided by construction rather than by version
negotiation.

**M6+ plan:** extend the boot handshake magic from `"MOKY"` (4 B) to `"MOKY" + uint16_t
ipc_abi_version + uint16_t reserved` (8 B). Core 0 refuses to launch Core 1 if the ABI
versions disagree, and the status LED enters safe mode with a distinct pattern.

---

## 13. Relationship to Other Documents

| Document                                         | Scope                                                   |
|--------------------------------------------------|---------------------------------------------------------|
| `docs/requirements/system-requirements.md`       | System & hardware spec — BOM, operating modes, mandatory HW rules |
| `docs/requirements/hardware-requirements.md`     | Full BOM, power tree, GPIO map, keypad matrix            |
| `docs/requirements/software-requirements.md`     | SRS — what the firmware must do (behaviour, performance) |
| `docs/design-notes/firmware-architecture.md`     | **This doc** — how the firmware is actually built (HOW)  |
| `docs/design-notes/ipc-ram-replan.md`            | Derivation / budget record for the 176/312 split        |
| `docs/design-notes/mie-architecture.md`          | MIE (MokyaInput Engine) internals                       |
| `docs/bringup/phase2-log.md`                     | Per-milestone bring-up log, issue IDs (P2-n)            |
| `firmware/shared/ipc/ipc_protocol.h`             | Message IDs + payload structs (normative)               |
| `firmware/shared/ipc/ipc_shared_layout.h`        | 24 KB byte map (normative)                              |
| `firmware/core1/m1_bridge/memmap_core1_bridge.ld`| Core 1 linker script (normative)                        |
| `firmware/core0/...patch_arduinopico.py`         | Core 0 framework patches (normative)                    |
| `scripts/build_and_flash.sh`                     | Dual-image build + flash driver                         |
