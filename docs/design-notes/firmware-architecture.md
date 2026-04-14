# Firmware Architecture

**Project:** MokyaLora (Project MS-RP2350)
**Status:** Phase 2 — Core 0/Core 1 dual-image production firmware (Rev A)
**Last updated:** 2026-04-14

This document is the integrated architecture reference for MokyaLora's dual-core firmware.
It consolidates what has been validated on Rev A hardware:

- Core 0 / Core 1 layered responsibilities
- SRAM partition (verified via SWD and linker script)
- Shared-SRAM IPC byte map (three SPSC rings + GPS double-buffer)
- The mechanisms that guarantee the two ELF images cannot corrupt each other

Primary sources of truth:

- `firmware/core1/m1_bridge/memmap_core1_bridge.ld` — Core 1 linker script
- `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/patch_arduinopico.py` — Core 0 memmap patch
- `firmware/shared/ipc/ipc_protocol.h`, `ipc_shared_layout.h`, `ipc_ringbuf.[ch]` — IPC transport
- `docs/design-notes/ipc-ram-replan.md` — budget derivation & SWD verification record

---

## 1. Dual-Core AMP Topology

MokyaLora runs the RP2350B's two Cortex-M33 cores as independent AMP images with separate
flash slots, separate SRAM regions, and separate licences. Core 0 is launched by the
bootrom from `0x10000000`; Core 0 then launches Core 1 via `multicore_launch_core1_raw()`
from `0x10200000`.

| Attribute             | Core 0                                           | Core 1                                            |
|-----------------------|--------------------------------------------------|---------------------------------------------------|
| Role                  | Meshtastic LoRa modem                            | UI host, IME, power manager, all I2C drivers      |
| Licence               | GPL-3.0                                          | Apache-2.0                                        |
| Flash slot            | `0x10000000` (2 MB)                              | `0x10200000` (2 MB)                               |
| SRAM region           | `0x20000000–0x2002BFFF` (176 KB)                 | `0x2002C000–0x20079FFF` (312 KB)                  |
| Framework             | Arduino-Pico + Meshtastic base                   | Pico SDK + FreeRTOS + LVGL                        |
| Scheduler             | `OSThread` cooperative (`concurrency::Scheduler`)| FreeRTOS V11.3.0 preemptive (single-core port)    |
| Boot model            | Bootrom → crt0 → `main()`                        | `multicore_launch_core1_raw()` (no IMAGE_DEF)     |
| Owns                  | SPI1 + SX1262                                    | I2C ×2, PIO (keypad, LCD), PWM, GPIO              |

The sole cross-core compile-time dependency is `firmware/shared/ipc/ipc_protocol.h` (MIT) —
POD types, `<stdint.h>` only.

---

## 2. Core 0 Architecture (Meshtastic Modem)

Core 0 is stock Meshtastic with exactly one structural addition: the USB/BLE host transport is
replaced with `IPCPhoneAPI`, a subclass of Meshtastic's `PhoneAPI` that writes protobuf bytes
into the shared-SRAM ring instead of a serial or BLE endpoint. Everything above
`IPCPhoneAPI` is unmodified Meshtastic code.

### 2.1 Layers (top-down)

```
┌─────────────────────────────────────────────────────────────────┐
│  Transport Adapter                                              │
│    IPCPhoneAPI  (PhoneAPI subclass — replaces USB/BLE)          │
├─────────────────────────────────────────────────────────────────┤
│  Application                                                    │
│    Meshtastic Modules:                                          │
│      TextMessage · Position · NodeInfo · Telemetry · Admin      │
│      Traceroute · NeighborInfo · Waypoint · PKI                 │
├─────────────────────────────────────────────────────────────────┤
│  Routing & State                                                │
│    Router · MeshService · NodeDB · Config                       │
├─────────────────────────────────────────────────────────────────┤
│  Radio Abstraction                                              │
│    RadioInterface · CryptoEngine · MeshPacket queue             │
├─────────────────────────────────────────────────────────────────┤
│  HW Driver                                                      │
│    SX1262 driver (SPI1, GPIO 24–27)                             │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Scheduler

Arduino-Pico cooperative `OSThread` scheduler (`concurrency::Scheduler`). FreeRTOS inside the
Arduino-Pico core is forced to **single-core mode** (`configNUMBER_OF_CORES=1`) because the
upstream SMP port assumes it owns Core 1, which MokyaLora gives to a separate image. The
five framework patches that achieve this are applied idempotently by
`patch_arduinopico.py` at build time (see phase2-log Issue P2-2).

### 2.3 Exclusions (via MESHTASTIC_EXCLUDE flags)

Core 0 is trimmed hard to fit in 176 KB: no GPS driver, no Screen, no WiFi, no BT, no I2C
master, no MQTT, no StoreAndForward, no CannedMessages, no ATAK, no sensor telemetry. GPS
data arrives via the shared-SRAM GPS buffer — Core 0 never talks I2C. Only the modules
listed in §2.1 are compiled in.

---

## 3. Core 1 Architecture (UI / App Host)

Core 1 is a fresh Apache-2.0 image built with the Pico SDK directly (not Arduino-Pico). It
owns every peripheral that isn't LoRa: the LCD, keypad, all I2C sensors and PMICs, PWM
haptic motor, status LED. Audio (PDM mic + I2S amp) is **not** on the target architecture —
the next hardware revision removes both devices.

### 3.1 Layers (top-down)

```
┌─────────────────────────────────────────────────────────────────┐
│  Application / UI                                               │
│    LVGL Apps:                                                   │
│      Messages · Nodes · Map · Compass · Settings · Status       │
├─────────────────────────────────────────────────────────────────┤
│  Services                                                       │
│    MIE (IME) · Power Manager SM · IPC Client                    │
├─────────────────────────────────────────────────────────────────┤
│  HAL Drivers                                                    │
│    Charger (BQ25622)  · Gauge (BQ27441)                         │
│    EnvSensor (LSM6DSV16X / LIS2MDL / LPS22HH)                   │
│    GPS (Teseo-LIV3FL) · StatusLED (LM27965)                     │
│    HapticMotor (PWM) · KeypadScan (PIO+DMA) · LCDDriver (PIO)   │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Scheduler

FreeRTOS V11.3.0, RP2350 Cortex-M33 port (community-supported, NTZ variant), **single-core
build** — Core 1 runs one FreeRTOS scheduler instance alone. Heap scheme: `heap_4`
(`ucHeap[configTOTAL_HEAP_SIZE]` inside `.bss`).

### 3.3 Display path

Single full-screen framebuffer (240 × 320 × RGB565 = 150 KB) in SRAM, LVGL in
`LV_DISPLAY_RENDER_MODE_DIRECT`. LVGL redraws dirty rectangles; DMA pushes dirty rects to
the NHD 2.4″ LCD via PIO 8080. Target 60–80 FPS. PSRAM is **not** used for display — the
8 MB APS6404L tops out at 12.8 FPS for full-frame read (QMI per-word overhead) and has an
unresolved DMA burst-read anomaly (Issue 14 / Step 25).

PSRAM is used only for: MIE dictionary data (4 MB) and application heap (message history,
node cache).

---

## 4. SRAM Partition (520 KB total)

Verified by reading the linker scripts and by SWD inspection of both cores' SPs +
`g_ipc_shared.boot_magic` after boot.

```
0x20000000 ┌────────────────────────────────────────────────────┐
           │  Core 0 — Meshtastic (GPL-3.0)                     │
           │    .vector + .data + .bss          54 KB           │  176 KB
           │    .heap                          122 KB           │
0x2002C000 ├────────────────────────────────────────────────────┤
           │  Core 1 — FreeRTOS + LVGL + MIE (Apache-2.0)       │
           │    framebuffer (240×320×16bpp)    150 KB           │
           │    LVGL + FreeRTOS                 83 KB           │  312 KB
           │    HAL + App + Stack               48 KB           │
           │    margin                          31 KB           │
0x2007A000 ├────────────────────────────────────────────────────┤
           │  Shared IPC (NOLOAD, MIT)                          │   24 KB
           │    three SPSC rings + GPS buffer                   │
0x20080000 ├────────────────────────────────────────────────────┤
           │  SCRATCH_X  (Core 0 task stack, Arduino-Pico)      │    4 KB
0x20081000 ├────────────────────────────────────────────────────┤
           │  SCRATCH_Y  (Core 0 MSP stack)                     │    4 KB
0x20082000 └────────────────────────────────────────────────────┘
           __StackTop
```

| Region      | Range                       | Size   | Owner / Contents                                   |
|-------------|-----------------------------|--------|----------------------------------------------------|
| Core 0      | `0x20000000 – 0x2002BFFF`   | 176 KB | Meshtastic image: static 54 KB + heap 122 KB       |
| Core 1      | `0x2002C000 – 0x20079FFF`   | 312 KB | FB 150 KB + LVGL+FreeRTOS 83 KB + HAL/App/Stack 48 KB + margin 31 KB |
| Shared IPC  | `0x2007A000 – 0x2007FFFF`   | 24 KB  | NOLOAD — three SPSC rings + GPS double-buffer      |
| SCRATCH_X   | `0x20080000 – 0x20080FFF`   | 4 KB   | Core 0 stack (Arduino-Pico managed)                |
| SCRATCH_Y   | `0x20081000 – 0x20081FFF`   | 4 KB   | Core 0 MSP / ISR stack                             |

The 176/312 split was chosen after an M3+ budget review showed 192/296 left Core 1 with
only ~5 % margin once the 150 KB framebuffer was added. At 312 KB Core 1 has ~10 % margin;
Core 0 has ~20 % margin at its worst-case NodeDB size (200 nodes, ~140 KB heap peak).

See `docs/design-notes/ipc-ram-replan.md` §2 for the detailed per-component budget.

---

## 5. Shared IPC — 24 KB Byte Map

All offsets are relative to `0x2007A000`. The layout is declared once in
`firmware/shared/ipc/ipc_shared_layout.h` and both ELF images place a `.shared_ipc`
section at this exact address (NOLOAD).

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

### 5.1 Three rings — why, and their slot counts

Core 0's `writeStream()` loop on a `want_config_id` interleaves ~40 protobuf frames with
`LOG_DEBUG/LOG_INFO` lines. On a single ring the log bytes consume slots that the protobuf
stream needs, creating 12–49 ms per-frame gaps. Splitting into three rings:

- **`c0_to_c1` (DATA, 32 slots, blocking on full)** — protobuf only. Producer blocks if
  full; consumer (Core 1) drains with priority.
- **`c0_log_to_c1` (LOG, 16 slots, best-effort)** — `IPC_MSG_LOG_LINE` only.
  **If the ring is full, the producer drops the line rather than blocking.** Prevents log
  starvation of the data path.
- **`c1_to_c0` (CMD, 32 slots)** — commands from Core 1 (send text, set config, etc.).

Each slot is an `IpcRingSlot` of 264 bytes: `IpcMsgHeader` (4 B) + `uint8_t payload[256]`
+ 4 B ring metadata.

### 5.2 Ring API

```c
bool ipc_ring_push(IpcRingCtrl *ctrl, IpcRingSlot *slots,
                   uint32_t slot_count,      // 32 for data/cmd, 16 for log
                   uint8_t msg_id, uint8_t seq,
                   const void *payload, uint16_t payload_len);

bool ipc_ring_pop (IpcRingCtrl *ctrl, IpcRingSlot *slots,
                   uint32_t slot_count,
                   IpcRingSlot *out);
```

The `slot_count` parameter lets one implementation serve all three rings — the log ring
uses modulo-16 indexing while the data/cmd rings use modulo-32.

### 5.3 GPS double-buffer

`IpcGpsBuf` at `0x5300`: `uint8_t buf[2][128]`, plus a single-byte `write_idx` (0 or 1)
that the writer (Core 1) flips atomically after a full NMEA sentence is committed. The
reader (Core 0) always reads `buf[write_idx ^ 1]`, so producer and consumer never touch the
same slot. No locking, no doorbell needed for GPS — the write_idx flip *is* the signal.

---

## 6. Cross-Image Non-Interference

Two independently-linked ELF images share the chip. The following mechanisms guarantee
they cannot corrupt each other's memory at compile-time, link-time, boot-time, or runtime.

### 6.1 Link-time: matching NOLOAD region + nm verification

Both `patch_arduinopico.py` (Core 0) and `memmap_core1_bridge.ld` (Core 1) declare the
identical `SHARED_IPC` MEMORY region:

```
SHARED_IPC (rw) : ORIGIN = 0x2007A000, LENGTH = 24K
```

Both emit `.shared_ipc (NOLOAD)` at `0x2007A000`. Post-link CI check: `nm` both ELFs and
assert `g_ipc_shared` resolves to the identical address. Any layout drift between the two
headers becomes a build-break, not a silent memory stomp.

### 6.2 Boot model: no IMAGE_DEF on Core 1

Core 1's linker script `/DISCARD/`s `.embedded_block`, `.embedded_end_block`, `.boot2`,
and `.binary_info_header`. Core 1's image therefore has **no IMAGE_DEF header** at word 0;
instead, the first two flash words are the M33 vector table — `SP = __StackTop`,
`PC = mokya_core1_reset_handler` (Thumb bit set).

Core 0 boots Core 1 with `multicore_launch_core1_raw(pc, sp, vtor)`, which reads SP and PC
directly from those two flash words. If Core 1 ever regained an IMAGE_DEF block it would
overwrite word 0 with `0xffffded3` and instantly fault on launch.

### 6.3 SRAM non-overlap

Core 0 memmap has `LENGTH = 176 KB` starting at `0x20000000`. Core 1 memmap has
`LENGTH = 312 KB` starting at `0x2002C000`. The two regions are back-to-back with zero
gap and zero overlap. The linker asserts `__StackLimit >= __HeapLimit` inside Core 1's
region. Core 1 never places data in `SCRATCH_X`/`SCRATCH_Y`; those 8 KB belong to
Core 0's Arduino-Pico task stacks.

### 6.4 Ring ownership model (producer/consumer partition)

Each SPSC ring is strictly single-producer, single-consumer:

- **`head`** is written only by the producer. Consumer reads it.
- **`tail`** is written only by the consumer. Producer reads it.
- **`slots[i]`** is owned by the producer while `(head - tail) < slot_count`; ownership
  transfers to the consumer when the producer publishes `head++`.

There is no shared mutable state that both sides write to. No spinlock, no atomic
read-modify-write.

### 6.5 Memory ordering

Every `head`/`tail` publish is surrounded by `__dmb()` (Data Memory Barrier):

- Producer: write `slots[head % N]` → `__dmb()` → `head++` → `__dmb()` → FIFO push.
- Consumer: read FIFO → `__dmb()` → read `head` → read `slots[tail % N]` → `__dmb()` →
  `tail++`.

This matches the RP2350 (Armv8-M) memory model: data writes to the slot must be visible
before the head pointer advance is visible to the other core.

### 6.6 GPS double-buffer atomicity

`write_idx` is a single `uint8_t` — reads and writes are naturally atomic on Cortex-M33.
The reader always reads `buf[write_idx ^ 1]`, guaranteeing it can never observe an
in-progress write. The writer commits a sentence, then flips `write_idx` last. No
`__dmb()` needed because the write pattern already serialises through a single byte.

---

## 7. Bus & Peripheral Ownership

Each peripheral has exactly one owner. No register of a foreign-owned peripheral is ever
read or written across the IPC boundary.

| Bus / Peripheral              | Owner  | Devices / Pins                                         |
|-------------------------------|--------|--------------------------------------------------------|
| Sensor bus (`i2c1`, GPIO 34/35)| Core 1 | LSM6DSV16X 0x6A · LIS2MDL 0x1E · LPS22HH 0x5D · Teseo 0x3A |
| Power bus  (`i2c1`, GPIO 6/7) | Core 1 | BQ25622 0x6B · BQ27441 0x55 · LM27965 0x36            |
| SPI1 (GPIO 24–27)             | Core 0 | SX1262 LoRa                                            |
| PIO — keypad scan             | Core 1 | 6×6 matrix, GPIO 36–47                                 |
| PIO — LCD                     | Core 1 | 8-bit 8080 parallel, NHD-2.4″                          |
| PWM — haptic motor            | Core 1 | MTR_PWM GPIO 9                                         |

Note: both I2C-like buses are `i2c1` in the Pico SDK (GPIO 6/7 and GPIO 34/35 are
alternate pin pairs on the same peripheral). Core 1 time-multiplexes `i2c1` between the
two pin sets by reinitialising the controller.

---

## 8. Relationship to Other Documents

| Document                                         | Scope                                                   |
|--------------------------------------------------|---------------------------------------------------------|
| `docs/requirements/software-requirements.md`     | Normative SRS — what the firmware must do               |
| `docs/design-notes/firmware-architecture.md`     | **This doc** — how Core 0 + Core 1 are actually built   |
| `docs/design-notes/ipc-ram-replan.md`            | Derivation / budget record for the 176/312 split        |
| `docs/bringup/phase2-log.md`                     | Per-milestone bring-up log, issue IDs (P2-n)            |
| `firmware/shared/ipc/ipc_protocol.h`             | Message IDs + payload structs (normative)               |
| `firmware/shared/ipc/ipc_shared_layout.h`        | 24 KB byte map (normative)                              |
| `firmware/core1/m1_bridge/memmap_core1_bridge.ld`| Core 1 linker script (normative)                        |
