# Phase 2 Firmware Log

**Plan:** `~/.claude/plans/groovy-petting-alpaca.md` (Phase 2 — RP2350B 韌體生產化)
**Phase 2 started:** 2026-04-11
**Scope:** Dual-core production firmware — Core 0 Meshtastic LoRa modem, Core 1 FreeRTOS + LVGL + MIE UI/IME, IPC via shared-SRAM SPSC ring.

Phase 2 is tracked by **milestone** (M1.0, M1.0b, M1.1, ...), not by step number.
Hardware bring-up log (Steps 1–26) lives in [rev-a-bringup-log.md](rev-a-bringup-log.md).

---

## Milestone 1 — Meshtastic USB → IPC byte-level bridge

**Goal:** PC Web Serial Console can talk to Meshtastic through Core 1's USB CDC, with Core 0 reaching the host only via the shared-SRAM SPSC ring. Every line of code in M1 is production code — no throwaway loopback.

### M1.0 — Core 0 `NO_USB` + `IpcSerialStream` build spike

**Date:** 2026-04-11
**Result:** ✅ COMPLETE — build ✅ PASS, flash ✅ PASS, boot ✅ PASS (Meshtastic reaches `loop()`)

**Goal:** Prove Core 0 Meshtastic firmware can build with Arduino-Pico's built-in `SerialUSB` disabled, with the framework `Serial` global replaced by a stub `IpcSerialStream` that will later route bytes through the shared-SRAM SPSC ring to Core 1.

**Changes applied to `variants/rp2350/rp2350b-mokya/`:**

| File | Change |
|------|--------|
| `platformio.ini` | `-DNO_USB`; single-core FreeRTOS flags (`-DconfigNUMBER_OF_CORES=1` + `configUSE_CORE_AFFINITY=0`, `configRUN_MULTIPLE_PRIORITIES=0`, `configUSE_PASSIVE_IDLE_HOOK=0`, `configUSE_TASK_PREEMPTION_DISABLE=0`); `extra_scripts` registers `patch_arduinopico.py`; `build_src_filter` adds `ipc_serial_stub.cpp`; `lib_deps` override drops `environmental_base`/`environmental_extra`; `lib_ignore += SdFat SD`; adds `MESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` |
| `variant.h` | Removed `#define DEBUG_RP2040_PORT Serial`; added `#include "ipc_serial_stub.h"` so the `extern Serial` declaration reaches every Meshtastic TU via `configuration.h` |
| `ipc_serial_stub.h` (new) | `IpcSerialStream : public Stream` (no-op read/write, `begin()`/`end()` stubs, `operator bool`); `extern IpcSerialStream Serial;` |
| `ipc_serial_stub.cpp` (new) | Single `IpcSerialStream Serial;` global |
| `patch_arduinopico.py` (new) | Idempotent pre-build script that patches five framework locations: (1) `SerialUSB.h` — `extern SerialUSB Serial;` guarded by `#if !NO_USB` with `// MOKYA_NO_USB_PATCH` marker; (2) `freertos/freertos-main.cpp` — SMP-only calls (`vTaskCoreAffinitySet`, `vTaskPreemptionDisable/Enable`, IdleCoreN task creation) guarded by `#if configNUMBER_OF_CORES > 1`; (3) `freertos/freertos-lwip.cpp` — lwIP task core-affinity call guarded the same way; (4) `FreeRTOS-Kernel/.../RP2350_ARM_NTZ/non_secure/portmacro.h` — added missing `extern volatile uint32_t ulCriticalNesting;` declaration for the single-core path; (5) `.../port.c` — removed the `static` qualifier on `ulCriticalNesting` so `wiring_private.cpp` can link against `portGET_CRITICAL_NESTING_COUNT()`. All blocks marked with `// MOKYA_SMP_PATCH` or `// MOKYA_NO_USB_PATCH`. |

**Upstream Meshtastic latent bug worked around:**
`src/modules/Telemetry/Sensor/AddI2CSensorTemplate.h:17` unconditionally references `ScanI2CTwoWire` under `#if WIRE_INTERFACES_COUNT > 1`, but `ScanI2CTwoWire.h` wraps its body in `#if !MESHTASTIC_EXCLUDE_I2C`. Two-phase name lookup resolves the non-dependent name at template definition time, so the header fails to parse whenever `MESHTASTIC_EXCLUDE_I2C=1` is combined with a telemetry sensor TU that hits `AirQualityTelemetry.cpp`. Workaround: `-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` (orthogonal to M1.0 — aligned with existing Core 0 no-sensor policy).

**Build result (final, single-core FreeRTOS):**

| Metric | Value |
|--------|-------|
| RAM   | 10.4 % (54 384 / 524 288 bytes) |
| Flash |  7.7 % (283 128 / 3 665 920 bytes) |
| ELF   | 4 594 508 bytes |
| BIN   | 295 436 bytes |
| UF2   | 591 360 bytes |
| Build time | 79.3 s |

**Flash + boot verification (J-Link, `RP2350_M33_0`):**

1. Download: 327 680 bytes @ 264 KB/s, verified OK.
2. Reset + run, then halt after 1.5 s with HW breakpoint on `setup` (`0x10005AD4`) →
   **hit**. PC = `0x10005AD4`, using PSP (`CONTROL=0x02`), LR = `__core0` FreeRTOS task caller.
   Meshtastic `setup()` reached through Arduino-Pico FreeRTOS task dispatch — proves
   `initVariant()` → `setup()` path works under `-DNO_USB` and Core 0 does not hang
   waiting for `USB.initted` (the `#ifndef NO_USB` guard in `freertos-main.cpp::__core0`
   correctly skips the wait loop).
3. Reset + run, then halt after 5 s with HW breakpoint on `loop` (`0x10005944`) →
   **hit** at CycleCnt ≈ 346 M (~2.3 s at 150 MHz). Meshtastic `setup()` completed and
   `loop()` is executing — full Core 0 firmware boot verified.
4. Between breakpoints, idle PC samples land on `vApplicationIdleHook` (`0x1002A472`),
   i.e. FreeRTOS scheduler is alive and switching to the idle task when `__core0`
   task yields.

**Rationale notes:**
- `DEBUG_RP2040_PORT` removal (A1-lite): Arduino-Pico's `DEBUGV` macro expands to `Serial.printf(...)` inside framework library TUs (e.g. `LittleFS.h`), but those TUs are compiled independently and cannot see our variant-level `Serial` injection. When `DEBUG_RP2040_PORT` is undefined, `debug_internal.h:23-27` makes `DEBUGV` a no-op, which is exactly what Core 0 wants.
- `lib_deps` override dropping `environmental_base`/`environmental_extra`: those sets pull in Adafruit sensor libraries (`Adafruit_BusIO`, `Adafruit_Sensor`, `Adafruit_BMP280`, `Adafruit_DPS310`, `Adafruit_MCP9808`, `Adafruit_INA219/260`, `Adafruit_MPU6050`, `Adafruit_LSM6DS`, `Adafruit_AHTX0`, `Adafruit_LIS3DH`). Their .cpp files reference the framework `Serial` global unconditionally (e.g. `Adafruit_Sensor.cpp:11` has an unguarded `Serial.println(...)`) and compile as independent TUs that never see our stub, so `-DNO_USB` is fatal for them. Core 0 has no sensors anyway.
- `lib_ignore += SdFat SD`: `framework-arduinopico/libraries/SdFat/` has header-inline `ls(&Serial, ...)` methods in `FatFile.h`, `FsFile.h`, `ExFatFile.h`, `FsVolume.h`, `FatVolume.h`, `ExFatVolume.h`. Any TU including these headers fails under `-DNO_USB`. PIO's LDF drags SdFat in because `src/FSCommon.cpp:17` has `#include <SD.h>` gated by `HAS_SDCARD` — LDF doesn't evaluate preprocessor, so the include is always scanned. Core 0 has no SD card.
- Single-core FreeRTOS (option C of M1.0 A3 triage): `rp2350_base.build_flags` inherits `-D__FREERTOS=1` which in Arduino-Pico 5.4.4 defaults to FreeRTOS **SMP** (`configNUMBER_OF_CORES=2`). SMP is fatal for MokyaLora at M1.0: the RP2350 port (`FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2350_ARM_NTZ/non_secure/port.c`) launches Core 1 from Core 0's `xPortStartScheduler` via `multicore_launch_core1(prvDisableInterruptsAndPortStartSchedulerOnCore)`, and Core 1's passive-idle task crashes in `vStartFirstTask` before any user code can run (HardFault with CFSR 0x101 IACCVIOL+IBUSERR, MMFAR = `0x2000C5AC` inside Core 1's own PSP). MokyaLora's architecture plan puts Core 1 under a **separate Apache-2.0 image** (loaded from `0x10200000` in M1.0b onwards), so Core 0's FreeRTOS must stay single-core. Override with `-DconfigNUMBER_OF_CORES=1` (and the three SMP-only feature flags that FreeRTOS requires off in single-core mode: `configUSE_CORE_AFFINITY`, `configRUN_MULTIPLE_PRIORITIES`, `configUSE_TASK_PREEMPTION_DISABLE`). Arduino-Pico's framework sources assume SMP unconditionally, so `patch_arduinopico.py` also guards the SMP call sites in `freertos-main.cpp` / `freertos-lwip.cpp` and fixes the single-core linkage of `ulCriticalNesting` in port.c / portmacro.h.

### M1.0b — Dual-image Core 1 boot spike

**Date:** 2026-04-11
**Result:** ✅ COMPLETE — Core 0 hands execution to a separate Apache-2.0 Core 1 image at flash `0x10200000`; both cores run concurrently and Core 1 writes its sentinel.

**Goal:** Prove Core 0 (Meshtastic, GPL-3.0) can load a minimal Apache-2.0 Core 1 image from a second flash region and launch it via `multicore_launch_core1_raw()`, without sharing any toolchain, linker script, or source tree between the two cores. This validates the license boundary and the dual-image flash layout before M1.1 adds the SPSC IPC ring.

**New files in this repo:**

| File | Purpose |
|------|---------|
| `firmware/core1/test/m1_bootspike/src/main_core1_bootspike.c` | Bare-metal Core 1 proof-of-life: zero .bss, write sentinel `0xC1B00701` to `0x20078000`, `dmb`, `wfi` forever. Apache-2.0. No Pico SDK, no libc, no peripherals. |
| `firmware/core1/test/m1_bootspike/memmap_core1_bootspike.ld` | Linker script — FLASH origin `0x10200000`/64 KB, RAM `0x20040000`/64 KB, `__stack_top__ = 0x20050000`, vector table aligned to 128 B in `.vectors`. No boot2, no IMAGE_DEF — Core 1 is launched by Core 0, not by bootrom. |
| `firmware/core1/test/m1_bootspike/CMakeLists.txt` | Standalone CMake project (`-mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16`, `-nostdlib -nostartfiles`, `--gc-sections`). Emits `.elf` + `.bin` via objcopy. |
| `firmware/core1/test/m1_bootspike/toolchain-arm-none-eabi.cmake` | Toolchain file pointing at `arm-none-eabi-gcc` (Arm GNU Toolchain). Sets `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` so CMake doesn't try to link a crt0 during compiler detection. |

**New/changed files in `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/` (submodule):**

| File | Change |
|------|--------|
| `variant.cpp` (new) | Implements `initVariant()` — reads the Core 1 vector table at `0x10200000` (word[0]=MSP, word[1]=reset handler), calls `multicore_reset_core1()`, then `multicore_launch_core1_raw(entry, sp, 0x10200000)`. Writes four 32-bit debug breadcrumbs to `0x20078010..0x2007801C` (phase marker, Core 1 SP, Core 1 entry, post-launch sentinel snapshot) so the whole boot path can be verified over SWD without a working UART. |
| `platformio.ini` | `build_src_filter += +<../variants/rp2350/rp2350b-mokya/variant.cpp>` |

**Memory layout:**

```
Flash (16 MB W25Q128JW, XIP @ 0x10000000):
  0x10000000..0x101FFFFF   Core 0 Meshtastic image (2 MB window)
  0x10200000..0x1020FFFF   Core 1 bootspike image  (64 KB window)

SRAM (520 KB, base 0x20000000):
  0x20040000..0x2004FFFF   Core 1 bootspike RAM (64 KB; __stack_top__ = 0x20050000)
  0x20078000               Core 1 sentinel (0xC1B00701 when alive)
  0x20078010..0x2007801C   Core 0 debug breadcrumbs (see table below)
```

**Debug breadcrumbs (`0x20078010`):**

| Offset | Meaning |
|--------|---------|
| `+0x00` | Phase marker: `0x11` entered `initVariant`, `0x12` vector table read, `0x12A` `multicore_reset_core1` returned, `0x13` `multicore_launch_core1_raw` returned, `0x14` sentinel snapshot captured |
| `+0x04` | Core 1 initial SP (should be `0x20050000`) |
| `+0x08` | Core 1 reset handler address (should be `0x102000Dx` with Thumb bit) |
| `+0x0C` | Sentinel value snapshot after Core 0 spins ~100k NOPs (should be `0xC1B00701`) |

**Build commands:**

```sh
# Core 1 bootspike (standalone CMake, bare-metal arm-none-eabi)
cmake -S firmware/core1/test/m1_bootspike \
      -B build/core1_bootspike \
      -DCMAKE_TOOLCHAIN_FILE="$(pwd)/firmware/core1/test/m1_bootspike/toolchain-arm-none-eabi.cmake" \
      -DCMAKE_MAKE_PROGRAM="C:/ProgramData/chocolatey/bin/ninja.exe" \
      -G Ninja
cmake --build build/core1_bootspike

# Core 0 Meshtastic (PlatformIO, from submodule root)
cd firmware/core0/meshtastic
pio run -e rp2350b-mokya
```

**Flash sequence (J-Link Commander, `RP2350_M33_0`):**

```
connect
r
loadfile "<core0 .elf>"                       // flashes 0x10000000 region
loadbin "<core1_bootspike.bin>" 0x10200000    // flashes 0x10200000 region
r
g
qc
```

**Verification (J-Link Commander after run, both cores):**

1. Halt Core 0 (`RP2350_M33_0`), dump memory:
   - `mem32 0x20078000 1` → `0xC1B00701` ✅ (sentinel written by Core 1)
   - `mem32 0x20078010 4` → `0x00000014 0x20050000 0x102000Dx 0xC1B00701` ✅ (all four phases hit)
   - Core 0 PC sample inside `loop()` (Meshtastic) ✅
2. Attach to Core 1 (`RP2350_M33_1`), halt:
   - PC ∈ `0x102000C0..0x102000E0` (inside the bootspike reset handler / WFI loop) ✅
   - MSP = `0x20050000` ✅ (the SP written by our vector table)

**Why `multicore_reset_core1()` is needed (Issue P2-3):**
`multicore_launch_core1_raw()` expects Core 1 to be sitting in the bootrom FIFO handler so the four-word launch handshake (`0, 0, 1, VTOR, SP, ENTRY`) is echoed back correctly. On first entry to `initVariant()` on a cold reset, Core 1 is **not** in that clean state — something in Arduino-Pico / Pico SDK / FreeRTOS single-core startup leaves it either parked somewhere else or with residual FIFO state. Without `multicore_reset_core1()`, Core 0 hangs inside `multicore_launch_core1_raw` (breadcrumb stuck at phase `0x12` — vector table read but `_raw` never returns). Calling `multicore_reset_core1()` first (which asserts `PSM_FRCE_OFF_PROC1` and pushes Core 1 back to the bootrom handler, exactly like Arduino-Pico's own `restartCore1()`) makes the handshake complete immediately. Root cause of the disturbance is deferred — tracked as Issue P2-3.

**Build result (Core 1 bootspike):**

| Metric | Value |
|--------|-------|
| .text / .rodata | ~240 bytes |
| .bss | 0 |
| ELF | `build/core1_bootspike/core1_bootspike.elf` |
| BIN | `build/core1_bootspike/core1_bootspike.bin` |

**Rationale notes:**
- Bare-metal, no Pico SDK: the spike touches no peripherals and has no libc, so pulling in `pico_sdk` would only add boot2, XIP setup, and runtime init that Core 1 must avoid (Core 0 has already configured XIP and clocks by the time this image runs).
- Vector table at start of flash image: `multicore_launch_core1_raw(entry, sp, vtor)` writes `vtor` into Core 1's `VTOR` register via the bootrom handshake, so the first two words at `0x10200000` *must* be `{MSP, reset_handler|1}`. We don't rely on this for Phase 2 interrupts yet — the spike runs with interrupts off — but keeping the layout honest means M1.1+ can enable Core 1 exceptions without relocating anything.
- `reset_handler` cannot be `static`: the linker script's `ENTRY(reset_handler)` requires an externally visible symbol. Without this the ELF silently falls back to `0x102000C0` and emits a linker warning.
- Stack at `0x20050000`: we reserve 64 KB of SRAM (`0x20040000..0x2004FFFF`) for Core 1 bootspike scratch, well clear of Core 0 FreeRTOS heap (top of SRAM, downward) and the SPSC ring / sentinel area planned at `0x20078000+`.
- `initVariant()` as the launch hook: Arduino-Pico calls `initVariant()` from the `__core0` FreeRTOS task before Meshtastic `setup()`, which means (a) FreeRTOS is already running so we can use normal C++ globals, but (b) no Meshtastic subsystem has started yet so there is no chance of FIFO contention with code that might also want to talk to Core 1 later.

### M1.1-A — Core 1 `m1_bridge` ring validator (bare metal)

**Date:** 2026-04-12
**Result:** ✅ COMPLETE — Core 1 drains `c0_to_c1` ring to completion; head advances from `0x9CB` to `0x1391` (2 502 slots consumed); overflow counter stays at 0.

**Goal:** Successor to the M1.0b bootspike. Same dual-image launch path (`multicore_launch_core1_raw` from Core 0 `initVariant`), but this image is linked against the shared SPSC ring code and actively drains `c0_to_c1` messages + echoes to `c1_to_c0`. No USB yet — M1.1-A proves the ring contract, M1.1-B layers TinyUSB on top.

**New shared/ipc files (MIT, compiled into both cores):**

| File | Purpose |
|------|---------|
| `firmware/shared/ipc/ipc_shared_layout.h` | `IpcSharedSram` POD at `0x2007A000..0x20080000` (24 KB), two 32-slot `IpcRingSlot` arrays + 32 B `IpcRingCtrl` blocks with SPSC head/tail/overflow fields. `_Static_assert` on total size. |
| `firmware/shared/ipc/ipc_ringbuf.c/.h` | SPSC push/pop using GCC `__atomic_*` builtins (acquire/release DMB on Cortex-M33); defines `g_ipc_shared` in `.shared_ipc` NOLOAD section; `ipc_shared_init()` zeroes the region and publishes `IPC_BOOT_MAGIC`. |
| `firmware/shared/ipc/ipc_protocol.h` | Added `IPC_MSG_SERIAL_BYTES` (0x06), `IPC_MSG_PANIC` (0xFE), `IPC_MSG_PAYLOAD_MAX`, `IPC_RING_SLOT_COUNT`. |

**New `firmware/core1/m1_bridge/` image (Apache-2.0, bare-metal):**

| File | Purpose |
|------|---------|
| `memmap_core1_bridge.ld` | Carves RAM `0x20040000..0x20050000` (64 KB), maps `.shared_ipc` NOLOAD to `SHARED_IPC` region at `0x2007A000` so `nm` reports matching `g_ipc_shared` addresses on both cores. |
| `src/main_core1_bridge.c` | Own `reset_handler` that copies `.data`, zeros `.bss` (but NOT the NOLOAD `.shared_ipc` region), spins on `boot_magic`, pushes a LOG_LINE greeting, stamps `c1_ready`, and loops popping `SERIAL_BYTES` messages and echoing them back. Tiny local `memcpy`/`memset` stubs satisfy `ipc_ringbuf.c` without pulling in newlib (`-nostdlib`). |
| `CMakeLists.txt` | Standalone CMake project, same `-mcpu=cortex-m33 -mthumb -mfloat-abi=softfp` + `-nostdlib -nostartfiles` as the M1.0b bootspike. |

**Critical gotcha:** The bridge MUST NOT `wfe` when the ring is empty — Core 0 `ipc_ring_push` issues no `sev`, so a WFE sleep is terminal. Plain spin works until M1.1-B replaces it with FreeRTOS task scheduling.

**SWD verification:**

| Breadcrumb | Before | After traffic | Meaning |
|------------|--------|--------------|---------|
| `c0_to_c1_ctrl.head` | `0x9CB` | `0x1391` | Core 0 pushed 2 502 slots |
| `c0_to_c1_ctrl.tail` | — | `0x1391` | Core 1 fully drained |
| `c0_to_c1_ctrl.overflow` | 0 | 0 | No producer back-pressure |
| `boot_magic` | — | `'MOKY'` | Core 0 published `IPC_BOOT_MAGIC` |
| `c1_ready` | 0 | 1 | Core 1 reached `main()` loop |

### M1.1-B — Core 1 `m1_bridge` USB↔SPSC bridge (Pico SDK + FreeRTOS + TinyUSB)

**Date:** 2026-04-12
**Result:** ✅ COMPLETE — `meshtastic --port COM16 --info` protobuf round-trip returns Owner / Metadata / full mesh node list. Core 0 Meshtastic is reachable from a host PC through Core 1's USB CDC endpoint, with every byte crossing the shared-SRAM SPSC rings.

**Goal:** Upgrade M1.1-A's bare-metal ring validator into a real USB CDC bridge. Same boot contract (`multicore_launch_core1_raw` @ `0x10200000`, no IMAGE_DEF), but the image is now a full Pico SDK application with FreeRTOS RP2350_ARM_NTZ + TinyUSB device stack, so a PC running the Meshtastic CLI can talk to Core 0 over a standard `/dev/ttyACM*`-style endpoint.

**New/changed files in `firmware/core1/m1_bridge/`:**

| File | Change |
|------|--------|
| `CMakeLists.txt` | Full rewrite — now `pico_sdk_init()` + `add_subdirectory(FreeRTOS-Kernel/.../RP2350_ARM_NTZ)` + `target_link_libraries(… tinyusb_device FreeRTOS-Kernel FreeRTOS-Kernel-Heap4 pico_unique_id hardware_resets hardware_irq hardware_sync hardware_exception)`. `PICO_RUNTIME_SKIP_INIT_*` defines skip hardware-state hooks (CLOCKS, EARLY_RESETS, POST_CLOCK_RESETS, SPIN_LOCKS_RESET, BOOT_LOCKS_RESET, BOOTROM_LOCKING_ENABLE, USB_POWER_DOWN) that Core 0 already ran. `configNUMBER_OF_CORES=1` + `PICO_NO_BINARY_INFO=1`. Source order puts `src/core1_reset.S` **first** so its `.vectors` wins slot 0 of the output image. No `pico_add_extra_outputs()` — we emit raw `.bin` for `loadbin 0x10200000`. |
| `memmap_core1_bridge.ld` | Extended from M1.1-A: keeps FLASH `0x10200000`/2 MB + RAM `0x20040000`/64 KB carve, adds `ENTRY(_entry_point)` (SDK's ELF entry), explicit `KEEP(*core1_reset.S.obj(.vectors))` before the generic `KEEP(*(.vectors))`, `/DISCARD/` for `.embedded_block` + `.embedded_end_block` + `.boot2` + `.binary_info_header` + `.note.*` (the raw-launch boot model forbids any IMAGE_DEF header at word 0). Provides zero-size `.scratch_x_stub` / `.scratch_y_stub` NOLOAD sections so SDK crt0's `data_cpy_table` references `__scratch_[xy]_{source,start,end}__` resolve to a no-op copy. `__default_isrs_start/end = 0` so `hardware_exception`'s "is this still the compile-time default" check never matches. |
| `src/core1_reset.S` (new) | Custom vector table (68 entries — 16 system + 52 IRQ slots, all weakly aliased to `mokya_core1_default_isr`) + reset handler that **skips the SDK `crt0.S` CPUID check**. SDK `_reset_handler` unconditionally bounces any core with `CPUID != 0` back to the bootrom FIFO handler (it assumes Core 1 is launched via `multicore_launch_core1(func)`, not `_raw`), so we provide our own: zero `.bss`, copy `.data`, then call `runtime_init → main → exit`. The linker-script file-name match + source ordering ensures our `.vectors` input section lands at `0x10200000`. |
| `src/main_core1_bridge.c` (new) | Stamps sentinel, spins on `g_ipc_shared.boot_magic`, force-resets USBCTRL (re-enumeration for J-Link SWD reset case), `tusb_init()`, busy-polls `tud_task()` for ~2 s so the host enumerates CDC before Meshtastic starts logging, publishes `c1_ready`, pushes an `IPC_MSG_LOG_LINE` greeting, then hands off to FreeRTOS. Two tasks: `usb_device_task` (priority `configMAX_PRIORITIES-1`, polls `tud_task()` + 1-tick yield) and `bridge_task` (priority `tskIDLE_PRIORITY+2`, pops `c0_to_c1` → `tud_cdc_write`, reads `tud_cdc_read` → pushes `c1_to_c0`, bounded FIFO-full retry then drop). |
| `src/usb_descriptors.c` (new) | Device + configuration + CDC interface descriptors. VID/PID **0x2E8A:0x000F** (placeholder — Raspberry Pi vendor, must switch to an official MokyaLora PID via the `raspberrypi/usb-pid` PR process before Rev B). Manufacturer "MokyaLora", product "MokyaLora Meshtastic", serial number derived from the RP2350 unique ID via `pico_unique_id`. |
| `src/tusb_config.h` (new) | Device-only CDC, 1024 B RX + 1024 B TX FIFO, `CFG_TUSB_RHPORT0_MODE = OPT_MODE_DEVICE \| OPT_MODE_FULL_SPEED`. Does **not** override `CFG_TUSB_OS` — SDK's `tinyusb_device` target sets `OPT_OS_PICO` (hardware mutex), which works both pre-scheduler (during the `tusb_init` busy-poll) and post-scheduler (from the FreeRTOS USB task). |
| `src/FreeRTOSConfig.h` (new) | Single-core RP2350_ARM_NTZ port, Heap4, 15 priorities, 256-word minimal stack, `configTOTAL_HEAP_SIZE = 32 KB`. |

**Critical gotchas encountered during bring-up:**

1. **SDK `crt0.S` CPUID bounce:** `_reset_handler` starts with `ldr r0, =SIO_CPUID; ldr r0, [r0]; cbz r0, 1f; hold_non_core0_in_bootrom: ldr r0, =BOOTROM_VTABLE_OFFSET; b _enter_vtable_in_r0`. There is no SDK define to disable this, so any standalone image launched via `_raw` on Core 1 must provide its own reset handler. Fixed with `core1_reset.S` (see above).
2. **`tud_cdc_connected()` is not the right gate:** `tud_cdc_connected()` returns true only after the host sends `SET_CONTROL_LINE_STATE` with the DTR bit set. `pyserial` (and hence the Meshtastic CLI) does this by default, but Chrome WebSerial, Meshtastic web console, and some Linux `/dev/ttyACM` consumers do not. Original bridge gated `tud_cdc_write` on `tud_cdc_connected()` and dropped every burst when DTR was low, so the host saw nothing even after opening the port. Fix: gate on `tud_mounted()` (USB enumerated) and rely on a bounded FIFO-full retry (~10 ms) to drop bursts when no client is actively reading. CDC-connected bit kept as a diagnostic breadcrumb bit.
3. **Breadcrumb collision with Core 0 heap:** M1.1-A placed SWD breadcrumbs at `0x20078000..0x2007800C` on the assumption that this 8 KB window below `.shared_ipc` was untouched. Wrong — Core 0's linker script extends RAM up to `0x2007A000`, so the window is **inside Core 0's FreeRTOS heap**. Fine at M1.1-A (no heap pressure), fatal at M1.1-B once Meshtastic's heap usage climbed: the sentinel and counters were overwritten by task-stack contents (code pointers in the `0x1002xxxx` range). Fix: move all Core 1 breadcrumbs to the last 64 B of `.shared_ipc` (`0x2007FFC0..0x20080000`) — inside `g_ipc_shared._tail_pad`, reserved by both linker scripts as NOLOAD shared SRAM, and never touched by ring traffic.

**Breadcrumbs (SWD-readable, inside `.shared_ipc` tail):**

| Address | Meaning |
|---------|---------|
| `0x2007FFC0` | Sentinel `0xC1B01200` — stamped by `main()` after the SDK runtime hands over |
| `0x2007FFC4` | `rx_total` — bytes drained from `c0_to_c1` ring and pushed to CDC IN |
| `0x2007FFC8` | `tx_total` — bytes read from CDC OUT and pushed to `c1_to_c0` ring |
| `0x2007FFCC` | `usb_state` — bit0 = `tud_mounted`, bit1 = `tud_cdc_connected` (DTR) |
| `0x2007FFD0` | `loop_count` — `bridge_task` iterations (liveness) |

**Build commands:**

```sh
# Core 1 m1_bridge (Pico SDK project, requires PICO_SDK_PATH + arm-none-eabi + MSVC for picotool host build)
cmd //c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && \
  cmake -S firmware/core1/m1_bridge -B build/core1_bridge -G Ninja'
cmd //c 'call "...vcvarsall.bat" x64 && cmake --build build/core1_bridge --target core1_bridge'
```

**Flash sequence:**

```
loadfile "<core0 .elf>"                    // Core 0 Meshtastic @ 0x10000000
loadbin  "<core1_bridge.bin>" 0x10200000   // Core 1 bridge      @ 0x10200000
r; g
```

**End-to-end verification — `meshtastic --port COM16 --info`:**

```
Connected to radio
Owner: Meshtastic b862 (b862)
My info: { "myNodeNum": 2975709282, "minAppVersion": 30200, "rebootCount": 0 }
Metadata: { "firmwareVersion": "2.7.21.01c5b67", ..., "hwModel": 79, "role": "CLIENT" }
Nodes in mesh: { "!b15db862": {...}, "!851a5f16": {...}, "!db2faab8": {...}, ... }
```

- `myNodeNum = 0xb15db862` matches Core 0's `initNodeDB` log line observed on raw serial capture
- `firmwareVersion 2.7.21.01c5b67` matches the boot log observed via PowerShell `SerialPort.ReadExisting` (1 682 B / 2.5 s window)
- `hwModel: 79` is the Mokya RP2350B variant ID
- Full node list (86 entries from `/prefs/nodes.proto`) decoded — proves the host→device→host protobuf path handles multi-packet responses cleanly

This closes Milestone 1: Core 0 Meshtastic is reachable from a host PC through Core 1's USB CDC endpoint, with every byte crossing the shared-SRAM SPSC rings defined in `firmware/shared/ipc/`. No direct Core 0 USB — Arduino-Pico's `Serial` is `IpcSerialStream`, which pushes into `c0_to_c1` and reads from `c1_to_c0`.

**Outstanding items (non-blocking):**

- **VID/PID `0x2E8A:0x000F` is a placeholder.** Must submit a `raspberrypi/usb-pid` PR to get an official MokyaLora PID before Rev B.
- **Issue P2-4 — `.shared_ipc` placement confusion in `ipc_shared_layout.h`.** The file's header comment claims `0x20078000..0x2007A000` is "preserved untouched" as a secondary SWD debug channel. This is wrong — Core 0's linker script (`memmap_default.ld` MOKYA_SHARED_IPC_PATCH) sets RAM to `0x20000000..0x2007A000`, so the window is **inside** Core 0's FreeRTOS heap. Left alone for now (M1.0b sentinel observation still happens to land early in the boot before heap pressure), but the comment should be fixed when M1.1-B's lessons are rolled back upstream into the shared header.

### P2-5 / P2-6 Root Cause Analysis (post-M1.1-B)

**Date:** 2026-04-12
**Scope:** Architecture-level analysis of the byte-bridge drop and latency problems.

#### Finding 1 — Pop-before-delivery is a structural defect (P2-5 root cause)

The `bridge_task` in `main_core1_bridge.c` has an asymmetric flow control design:

| Direction | Pattern | Result |
|---|---|---|
| CDC OUT → c1_to_c0 ring | Read USB first, then spin-push to ring until success | ✅ Guaranteed delivery |
| c0_to_c1 ring → CDC IN | **Pop ring slot first** (destructive, `tail` advances immediately), then attempt CDC write | ❌ Drop on CDC backpressure |

`ipc_ring_pop()` (`ipc_ringbuf.c:111`) immediately advances `tail` via `__atomic_store_n`. Once popped, the slot is gone forever. If `tud_cdc_write_available() == 0` persists for ≥10 ticks (~10 ms), the remaining bytes in `scratch[]` are dropped (`main_core1_bridge.c:163-167`). Since the bridge is frame-unaware (opaque byte stream), this truncates protobuf frames at arbitrary byte boundaries. The host-side Meshtastic deserializer sees a broken `0x94C3` framed packet, silently discards it, and the message is permanently lost.

**This is not a parameter-tuning problem.** Increasing the stall timeout or ring depth delays the symptom but does not fix the root cause — any transient CDC backpressure (WebSerial reader paused, JS event loop busy, browser tab not focused) can still trigger data loss.

**The `ipc_ringbuf` API has no peek function** — only destructive `pop`, `push`, `pending`, `free_slots`.

#### Finding 2 — Dual 1 ms yield stacks round-trip latency (P2-6 root cause)

Two FreeRTOS `vTaskDelay(pdMS_TO_TICKS(1))` calls add per-iteration latency:

1. `usb_device_task:113` — `tud_task()` called once per ms. FS USB SOF is 1 ms, but the endpoint needs re-arming after each transfer completion. If `tud_task()` runs only 1000×/s, each transfer completion waits up to 1 ms before the next transfer is armed.
2. `bridge_task:220` — idle yield sleeps 1 ms before re-checking the ring, even if Core 0 pushed a new slot nanoseconds after the check.

Combined effect on the `want_config_id` handshake burst (~10–20 KB, 100+ protobuf frames):
- Each ring slot drain cycle adds 0–2 ms of bridge latency
- ~100 ring slots × ~1–2 ms/slot ≈ 100–200 ms added latency vs native `SerialUSB`
- Plus Core 0 `IpcSerialStream::write()` busy-wait when ring fills (up to 50 ms/chunk)

No cross-core notification exists: Core 0's `ipc_ring_push` does not wake Core 1. Core 1 discovers new data only on its next 1 ms yield wake-up.

#### Finding 3 — Meshtastic serial framing vs ring slot size

Meshtastic's `StreamAPI` framing: `0x94 0xC3` + 2-byte big-endian length + protobuf payload. Max frame = 516 B (`MAX_TO_FROM_RADIO_SIZE=512` + 4-byte header). Ring slot = 256 B.

`IpcSerialStream::write()` auto-chunks: a 516 B frame becomes 3 ring slots (256 + 256 + 4 B). Core 1 bridge pops each slot and streams bytes to CDC. The host reassembles frames from the byte stream using `0x94C3` framing — **this is correct and not a problem**, as long as bytes are not dropped (see Finding 1).

#### Finding 4 — IPC protocol completeness for full product

Current `ipc_protocol.h` uses only `IPC_MSG_SERIAL_BYTES` (byte tunnel). This covers 100% of Meshtastic functionality for M1 (transparent bridge). However, for M4+ (Core 1 LVGL UI that directly displays/edits Meshtastic state), the protocol is missing:

| Feature | Status |
|---|---|
| Config get/set (10 Config types + 16 ModuleConfig types) | ❌ Missing — needed for settings UI |
| Telemetry data (temperature, humidity, air quality) | ❌ Missing |
| Waypoint | ❌ Missing |
| Admin messages (remote config) | ❌ Missing |
| Firmware update / OTA | ❌ Missing (Rev B) |

Config get/set is the most critical gap — it is a foundational UI feature. Design below in M1.2 plan.

### P2-8 — CLI version mismatch invalidated all M1.2 regression tests

**Date:** 2026-04-12
**Severity:** Critical (false regression — wasted an entire debugging session)

**Root cause:** The system PATH resolved `meshtastic` to a Python 3.11 (Microsoft Store) installation at version **2.3.11**, while `pip install meshtastic` on the active Python 3.13 installed version **2.7.8**. The firmware is **2.7.21** — the protobuf schema between 2.3.x and 2.7.x is completely incompatible, so `meshtastic --info` always times out regardless of bridge code changes.

**Discovery:** Flashing a stock Pico2 Meshtastic device (COM7) and testing with `python -m meshtastic --port COM7 --info` (CLI 2.7.8) succeeded immediately — 34 nodes, full config dump. Then flashing the MokyaLora dual-image (M1.1-B baseline) and testing with `python -m meshtastic --port COM16 --info` also succeeded — 64 nodes, full config, `pioEnv: "rp2350b-mokya"`, `firmwareVersion: "2.7.21.01c5b67"`.

**Impact:**
- All `meshtastic --info` timeout failures observed during M1.2 development were false negatives — the bridge code was working correctly the entire time.
- The M1.2 staged-delivery code was **not** causing the timeout regression — the code may have been correct all along.
- P2-5 (pop-before-delivery) architectural analysis remains valid as a theoretical risk under CDC backpressure, but was **not** the cause of the observed symptom.
- P2-6 (slow handshake) needs re-evaluation with the correct CLI — the 1 ms vTaskDelay overhead may be negligible compared to the protobuf exchange time.

**Fix:** Use `python -m meshtastic` (resolves to 2.7.8) for all future testing. Consider uninstalling the Python 3.11 meshtastic package or adjusting PATH.

### M1.2 — Bridge architecture fix + Config IPC definition

**Status:** ✅ COMPLETE (2026-04-12)
**Goal:** (A) Eliminate packet drops under CDC backpressure, (B) reduce bridge latency to match native SerialUSB, (C) define Config IPC messages for M4+ settings UI.

**Re-assessment after P2-8:** M1.1-B baseline is fully functional with the correct CLI. Parts A and B are hardening improvements (not blockers). Part C (Config IPC) is the main deliverable for M4+ UI readiness.

#### Part A — Staged-delivery bridge (fixes P2-5)

Replace the pop-then-drop pattern with a pop-then-hold pattern. Only `main_core1_bridge.c` changes; `ipc_ringbuf.c` / `ipc_shared_layout.h` / `ipc_serial_stub.cpp` are untouched.

**Design:**

```
Current:   pop slot → try CDC write → drop remainder on stall → pop next
Proposed:  pop slot → try CDC write → hold remainder in staging buffer
           → next iteration: drain staging first → only pop when staging empty
```

New state in `bridge_task`:

```c
static uint8_t  staged[IPC_MSG_PAYLOAD_MAX];
static uint16_t staged_len = 0;   // valid bytes in staging buffer
static uint16_t staged_pos = 0;   // bytes already delivered to CDC
```

Flow:

1. If `staged_pos < staged_len` — drain staging to CDC first (`tud_cdc_write` as much as `available()` allows). Do NOT pop a new ring slot.
2. If staging is empty (`staged_pos >= staged_len`) — pop next slot into `staged`, reset `staged_pos = 0`.
3. If `!tud_mounted()` — discard staging (no host, intentional drop).
4. **No stall timeout, no drop-burst.** CDC backpressure naturally propagates:
   - staging buffer full → bridge stops popping → ring fills → Core 0 `IpcSerialStream::write()` busy-wait triggers → end-to-end backpressure.

**Watchdog safety:** Add a `bridge_stall_ticks` counter. If staging data hasn't moved for 500 ms AND `tud_mounted()` is false, discard — covers the cable-unplug-during-transfer edge case.

#### Part B — Yield optimization (fixes P2-6)

| Change | File:Line | Before | After | Rationale |
|---|---|---|---|---|
| `usb_device_task` yield | `main_core1_bridge.c:113` | `vTaskDelay(pdMS_TO_TICKS(1))` | `taskYIELD()` | USB endpoint re-arm within µs instead of 1 ms |
| `bridge_task` idle yield | `main_core1_bridge.c:220` | `vTaskDelay(pdMS_TO_TICKS(1))` | `taskYIELD()` | React to new ring data within µs of Core 0 push |

`taskYIELD()` gives up the current timeslice but does not sleep — if no equal-or-higher priority task is ready, the same task runs again immediately. This is safe because:
- `usb_device_task` is priority `configMAX_PRIORITIES-1` (highest) — yields to nothing, effectively busy-polls `tud_task()` at max rate.
- `bridge_task` is priority `tskIDLE_PRIORITY+2` — yields to `usb_device_task` (and vice versa via round-robin), but both productive tasks keep running without forced 1 ms sleeps.
- Idle task still runs when both bridge and USB tasks yield with no work → CPU power draw is fine.

**M2 follow-up (not M1.2 scope):** Replace polling with cross-core notification. Core 0 `ipc_ring_push` → RP2350 SIO FIFO doorbell → Core 1 ISR → `xTaskNotifyFromISR(bridge_task)`. Bridge task uses `xTaskNotifyWait` with `portMAX_DELAY` timeout. This eliminates all polling and makes bridge latency ~µs. Deferred because it requires ISR setup + SIO FIFO integration — larger scope.

#### Part C — Config IPC definition (for M4+)

Add config get/set messages to `ipc_protocol.h`. Design principles:
- **Generic envelope + typed key**: one config value message type with a `uint16_t key` discriminator, not one message type per config field.
- **MIT licensed, self-contained**: Core 1 includes `ipc_protocol.h` only — no Meshtastic headers.
- **Core 0 adapter**: A GPL-3.0 module in `core0/` that translates between `IpcConfigKey` and Meshtastic's `AdminModule` — implemented at M4, not M1.2.
- **Key namespace**: `0xCCNN` where `CC` = category, `NN` = field index. Categories: `0x01`=Device, `0x02`=LoRa, `0x03`=Position, `0x04`=Power, `0x05`=Display, `0x06`=Channel, `0x07`=Owner. `0x10`–`0x1F` reserved for ModuleConfig (Telemetry, CannedMessage, etc.).

**New message IDs:**

| ID | Direction | Name | Purpose |
|---|---|---|---|
| `0x07` | C0→C1 | `IPC_MSG_CONFIG_VALUE` | Config value response (or unsolicited push on change) |
| `0x08` | C0→C1 | `IPC_MSG_CONFIG_RESULT` | Set/commit result (OK / error code) |
| `0x89` | C1→C0 | `IPC_CMD_GET_CONFIG` | Request config value by key |
| `0x8A` | C1→C0 | `IPC_CMD_SET_CONFIG` | Set config value by key |
| `0x8B` | C1→C0 | `IPC_CMD_COMMIT_CONFIG` | Commit pending changes (save + reboot if needed) |

**Initial config key set (MokyaLora feature phone UI):**

| Key | Category | Field | Type |
|---|---|---|---|
| `0x0100` | Device | `DEVICE_NAME` | string, max 40 B |
| `0x0101` | Device | `DEVICE_ROLE` | uint8 (CLIENT, ROUTER, etc.) |
| `0x0200` | LoRa | `LORA_REGION` | uint8 |
| `0x0201` | LoRa | `LORA_MODEM_PRESET` | uint8 (LONG_FAST, SHORT_TURBO, etc.) |
| `0x0202` | LoRa | `LORA_TX_POWER` | int8 (dBm) |
| `0x0203` | LoRa | `LORA_HOP_LIMIT` | uint8 (1–7) |
| `0x0204` | LoRa | `LORA_CHANNEL_NUM` | uint8 |
| `0x0300` | Position | `GPS_MODE` | uint8 (DISABLED/ENABLED/NOT_PRESENT) |
| `0x0301` | Position | `GPS_UPDATE_INTERVAL` | uint32 (seconds) |
| `0x0302` | Position | `POSITION_BCAST_SECS` | uint32 |
| `0x0400` | Power | `POWER_SAVING` | uint8 (bool) |
| `0x0401` | Power | `SHUTDOWN_AFTER_SECS` | uint32 |
| `0x0500` | Display | `SCREEN_ON_SECS` | uint32 (0=default 60 s) |
| `0x0501` | Display | `UNITS_METRIC` | uint8 (bool) |
| `0x0600` | Channel | `CHANNEL_NAME` | string, max 12 B |
| `0x0601` | Channel | `CHANNEL_PSK` | bytes, max 32 B |
| `0x0700` | Owner | `OWNER_LONG_NAME` | string, max 40 B |
| `0x0701` | Owner | `OWNER_SHORT_NAME` | string, max 5 B |

**Payload structures:**

```c
typedef struct {
    uint16_t key;                ///< IpcConfigKey
} IpcPayloadGetConfig;           /* 2 B */

typedef struct {
    uint16_t key;                ///< IpcConfigKey
    uint16_t value_len;          ///< Byte length of value[]
    uint8_t  value[];            ///< Type-dependent: uint8/int8/uint32/string/bytes
} IpcPayloadConfigValue;         /* 4 B + variable */

typedef struct {
    uint16_t key;                ///< Which key this result is for
    uint8_t  result;             ///< 0=OK, 1=UNKNOWN_KEY, 2=INVALID_VALUE, 3=BUSY
} IpcPayloadConfigResult;        /* 3 B */
```

#### Diagnostic breadcrumbs (Part A verification)

Add 7 counters at `0x2007FFD4..0x2007FFEC` (inside existing `_tail_pad`, after the 5 M1.1-B breadcrumbs):

| Address | Name | Meaning |
|---|---|---|
| `0x2007FFD4` | `drop_burst_count` | Times the 10-tick stall timeout was hit (should go to zero after fix) |
| `0x2007FFD8` | `drop_burst_bytes` | Total bytes dropped by stall timeout |
| `0x2007FFDC` | `c0c1_overflow_snap` | Snapshot of `c0_to_c1_ctrl.overflow` (Core 0 ring-full events) |
| `0x2007FFE0` | `c1c0_overflow_snap` | Snapshot of `c1_to_c0_ctrl.overflow` |
| `0x2007FFE4` | `max_write_deficit` | Largest `payload_len` seen when `tud_cdc_write_available()==0` |
| `0x2007FFE8` | `stall_tick_total` | Cumulative staging-drain ticks (backpressure duration) |
| `0x2007FFEC` | `usb_task_polls` | `tud_task()` call count (verify USB polling rate) |

#### Execution order

1. Add diagnostic breadcrumb definitions to `main_core1_bridge.c` (addresses + zero-init in `main()`)
2. Implement staged-delivery in `bridge_task` (Part A)
3. Switch both tasks to `taskYIELD()` (Part B)
4. Add Config IPC messages + key enum + payload structs to `ipc_protocol.h` (Part C)
5. Build Core 1 + flash dual-image
6. SWD verify breadcrumbs zero-init
7. `meshtastic --info` round-trip (regression check)
8. Web console test: idle handshake + LoRa send/receive + SWD read counters
9. Compare handshake speed vs M1.1-B baseline

#### M1.2 Part C — Implementation complete (2026-04-12)

Config IPC definition committed to `firmware/shared/ipc/ipc_protocol.h`:
- 5 new message IDs (`IPC_MSG_CONFIG_VALUE`, `IPC_MSG_CONFIG_RESULT`, `IPC_CMD_GET_CONFIG`, `IPC_CMD_SET_CONFIG`, `IPC_CMD_COMMIT_CONFIG`)
- `IpcConfigKey` enum with 18 keys across 7 categories (Device, LoRa, Position, Power, Display, Channel, Owner)
- 3 payload structs (`IpcPayloadGetConfig`, `IpcPayloadConfigValue`, `IpcPayloadConfigResult`)
- Build verified: Core 1 bridge compiles clean with new definitions
- Round-trip verified: `python -m meshtastic --port COM16 --info` succeeds — Config IPC header additions do not break existing bridge functionality

#### Benchmark: Bridge vs Native SerialUSB (2026-04-12)

Measured with `python -m meshtastic --port COMxx --info` (v2.7.8), 3 runs each.

**Pre-M1.2B baseline (dual `vTaskDelay(pdMS_TO_TICKS(1))`):**

| Configuration | Port | Avg wall time | Response size | Node count | Success |
|---|---|---|---|---|---|
| Stock Pico2 (native SerialUSB, firmware 2.7.20) | COM7 | **4.39 s** | 55 325 B | 100 nodes | 3/3 |
| MokyaLora Bridge (IPC ring + TinyUSB CDC, firmware 2.7.21) | COM16 | **15.02 s** | 7 549 B | 1 node | 3/3 |

**Post-M1.2B (`taskYIELD()` + equal task priority):**

| Configuration | Port | Run 1 | Run 2 | Run 3 | Response size | Node count |
|---|---|---|---|---|---|---|
| Stock Pico2 | COM7 | 4.36 s | — | — | 55 364 B | 100 |
| MokyaLora Bridge | COM16 | 8.07 s | 6.53 s | 6.51 s | 7 591 B | 1 |

**M1.2B changes:** (a) `usb_device_task` and `bridge_task` both changed from `vTaskDelay(pdMS_TO_TICKS(1))` to `taskYIELD()`; (b) both tasks set to equal priority (`tskIDLE_PRIORITY + 2`) — required because `taskYIELD()` only yields to equal-or-higher priority tasks; original `configMAX_PRIORITIES - 1` for `usb_device_task` starved `bridge_task`.

**Post-M1.2B + TX accumulation buffer + newline flush:**

| Configuration | Port | Run 1 | Run 2 | Run 3 | Avg | Response size | Node count |
|---|---|---|---|---|---|---|---|
| MokyaLora Bridge | COM16 | 5.84 s | 5.97 s | 5.83 s | **5.88 s** | ~4 500 B | 1 |

**M1.2B full change list:**
1. `usb_device_task` and `bridge_task` yield: `vTaskDelay(pdMS_TO_TICKS(1))` → `taskYIELD()`
2. Both tasks set to equal priority `tskIDLE_PRIORITY + 2` (was `configMAX_PRIORITIES - 1` for `usb_device_task` — `taskYIELD()` only yields to equal-or-higher, so unequal priority starved `bridge_task`)
3. TX accumulation buffer in `IpcSerialStream::write(uint8_t)`: batches single-byte writes (from `RedirectablePrint` log output) into a 256-byte buffer, flushed on newline or buffer full — previously each byte occupied a full 264-byte ring slot
4. `IpcSerialStream::write(const uint8_t*, size_t)`: flushes accumulated bytes first then pushes protobuf frames directly — no extra copy, ordering preserved
5. `IpcSerialStream::flush()`: now calls `flush_tx_acc_()` instead of no-op

**Analysis:**
- Total improvement from pre-M1.2B baseline: **15.02 s → 5.88 s (2.6× faster)**
- Per-KB throughput: **0.50 → ~0.76 KB/s** (1.5× improvement)
- Custom benchmark (`bench_bridge.py`, bypasses CLI Win11 sleep penalty): first-byte latency 60–120 ms, data transfer ~2.3 s for ~4.5 KB = **~1.95 KB/s** raw bridge throughput
- The 1.1 s gap between first chunk and steady-state data is Meshtastic state machine processing time (MyInfo → Config state transitions), not an IPC bottleneck
- **Per-KB throughput still ~16× slower than native:** stock Pico2 = 0.080 s/KB, MokyaLora = 1.31 s/KB. CLI Win11 sleep penalty is identical for both (same Python CLI), so it cancels out — the gap is entirely IPC bridge overhead
- **Remaining bottlenecks (ranked):**
  1. **SerialConsole polling interval** — Meshtastic cooperative scheduler: `readStream()` returns 5 ms (recent data) or 250 ms (idle). Core 0 checks for incoming ring data at most every 5 ms per OSThread tick — each CLI request→response round trip costs ≥5 ms of Core 0 polling latency on top of USB wire time
  2. **No cross-core notification** — `ipc_ring_push()` does not wake the other core. Core 1 discovers new c0→c1 data only on its next `taskYIELD()` round-robin cycle; Core 0 discovers new c1→c0 data only on its next `SerialConsole::runOnce()` poll. Cross-core SIO FIFO doorbell + `xTaskNotifyFromISR` (M2 scope) would cut this to µs
  3. **Ring buffer memcpy overhead** — every push/pop does a full payload copy; native `SerialUSB` is zero-copy into TinyUSB's endpoint FIFO
- **Root cause of byte-at-a-time inefficiency (fixed):** `RedirectablePrint::write(uint8_t c)` → `dest->write(c)` iterated each log byte through the IPC ring. Without the accumulation buffer, a 60-char log line consumed 60 × 264 = 15,840 bytes of ring bandwidth (99.6% overhead). With batching, same line costs 1 × (4 + 60) = 64 bytes
- **Verdict:** TX accumulation buffer + yield optimization delivered 2.6× wall-time improvement. Per-KB throughput gap (16×) remains significant and is dominated by polling latency, not data copy. Cross-core interrupt notification (M2) is the next high-impact optimisation — it addresses bottlenecks #1 and #2 above

#### M1.2 Close-out (2026-04-12)

**M1.2 delivered all three parts:**

| Part | Deliverable | Key result |
|------|-------------|------------|
| A | Staged-delivery bridge | Pop-then-hold replaces pop-then-drop; CDC backpressure propagates end-to-end |
| B | Yield optimization + TX accumulation buffer | `vTaskDelay(1)` → `taskYIELD()`, single-byte writes batched into 256 B buffer; CLI `--info` 15.0 s → 5.9 s (2.6×) |
| C | Config IPC definition | 5 msg IDs, `IpcConfigKey` enum (18 keys / 7 categories), 3 payload structs in `ipc_protocol.h` |

**M1 milestone (IPC byte bridge) is now complete.** Summary of M1 sub-milestones:

| Sub-milestone | Deliverable |
|---------------|-------------|
| M1.0 | Core 0 `NO_USB` + `IpcSerialStream` stub + single-core FreeRTOS patches |
| M1.0b | Dual-image Core 1 boot spike (`multicore_launch_core1_raw`) |
| M1.1-A | Core 1 `m1_bridge` ring validator (SPSC ring + shared SRAM layout) |
| M1.1-B | Core 1 USB CDC bridge — full Meshtastic serial passthrough via IPC ring |
| M1.2-A/B | Staged delivery + yield/accumulation optimization (2.6× speedup) |
| M1.2-C | Config IPC messages for M4+ LVGL settings UI |

**Remaining throughput gap (16× vs native) deferred to M2** — root cause is polling latency (SerialConsole 5 ms + no cross-core notification), not data copy overhead. M2 will add SIO FIFO doorbell + `xTaskNotifyFromISR` for µs-latency IPC wake-up.

**Open issues carried forward:** P2-7 (Core 0 ISR stack overflow into breadcrumb area), P2-9 (first-boot HardFault on empty flash).

---

## Issues Log (Phase 2)

| # | Date | Area | Issue | Resolution |
|---|------|------|-------|-----------|
| P2-1 | 2026-04-11 | Meshtastic `AddI2CSensorTemplate.h` | Latent upstream bug — non-dependent `ScanI2CTwoWire` name fails to resolve under `MESHTASTIC_EXCLUDE_I2C=1` due to two-phase template lookup | Workaround: `-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` in Core 0 variant. Upstream fix would be to also guard the `ScanI2CTwoWire*` parameter declaration under `#if !MESHTASTIC_EXCLUDE_I2C`. |
| P2-2 | 2026-04-11 | Arduino-Pico 5.4.4 FreeRTOS SMP on RP2350 | Core 1's passive-idle task HardFaults in `vStartFirstTask` the instant `xPortStartScheduler` launches it, blocking Core 0 from ever reaching `setup()`. CFSR=0x101 (IACCVIOL+IBUSERR), MMFAR=BFAR=0x2000C5AC (inside Core 1's own PSP region). Independent of `-DNO_USB` — architecture change, not a stub bug. | Switched to single-core FreeRTOS (`-DconfigNUMBER_OF_CORES=1`). Requires guarding the SMP-only framework code (`vTaskCoreAffinitySet`, `vTaskPreemptionDisable/Enable`, IdleCoreN task creation) in `freertos-main.cpp` / `freertos-lwip.cpp` and fixing the missing `extern` decl of `ulCriticalNesting` in the port — all done idempotently by `patch_arduinopico.py`. Core 1 is now left under Pico SDK reset and will be launched separately by the M1.0b Apache-2.0 boot image. Upstream fix would be to (a) provide an `extern` decl of `ulCriticalNesting` in single-core mode in `portmacro.h`, (b) drop `static` from `ulCriticalNesting` in `port.c`, and (c) either make `freertos-main.cpp`/`freertos-lwip.cpp` compile under `configNUMBER_OF_CORES==1` or gate the SMP-only bits. |
| P2-3 | 2026-04-11 | `multicore_launch_core1_raw` in `initVariant()` | On cold reset, by the time Core 0 reaches `initVariant()` Core 1 is **not** in the clean bootrom FIFO handler that `multicore_launch_core1_raw` expects — the four-word launch handshake never completes and Core 0 hangs inside `_raw` (debug breadcrumb stuck at phase `0x12`). Candidates for the disturbance: Arduino-Pico's early `rp2040.fifo.begin(2)`, `multicore_doorbell_claim_unused`, or Pico SDK `runtime_init_per_core_bootrom_reset`. Not yet isolated. | Workaround: call `multicore_reset_core1()` immediately before `multicore_launch_core1_raw()` in `initVariant()`. This asserts `PSM_FRCE_OFF_PROC1` and returns Core 1 to the bootrom handler (same mechanism as Arduino-Pico's `restartCore1()`), after which the handshake completes instantly and Core 1 starts executing our Apache-2.0 bootspike image at `0x10200000`. Safe — mirrors an existing framework path — but root cause investigation deferred to M1.1 when the Core 0 boot path is fully instrumented. |
| P2-4 | 2026-04-12 | `ipc_shared_layout.h` comment + legacy breadcrumb placement (M1.0b / M1.1-A) | The header comment claims `0x20078000..0x2007A000` is "preserved untouched" as a secondary SWD debug channel. This is **wrong** — Core 0's patched `memmap_default.ld` sets `RAM(rwx) : ORIGIN = 0x20000000, LENGTH = 512k - 0x6000` = `0x20000000..0x2007A000`, so the window is inside Core 0's FreeRTOS heap / task stack region. M1.0b + M1.1-A both used `0x20078000` for Core 1 sentinel + breadcrumbs and it worked by luck — at those phases Core 0's heap pressure was low enough that the sentinel bytes were never reached. M1.1-B triggered the collision: once Meshtastic task stacks + heap allocations reached the window, the breadcrumb region was overwritten with code pointers (observed `0x1002xxxx` / `0x2000xxxx` values instead of the `0xC1B01200` sentinel). | M1.1-B breadcrumbs moved to the last 64 B of `.shared_ipc` (`0x2007FFC0..0x20080000`), which lives inside `g_ipc_shared._tail_pad` — both cores' linker scripts reserve this range as NOLOAD shared SRAM and ring traffic never touches it. The M1.0b / M1.1-A legacy breadcrumbs at `0x20078000..0x2007801C` (initVariant phase marker + Core 1 SP/entry capture) are still used by Core 0 `variant.cpp` on cold boot before heap grows; those reads are valid for the first ~100k NOPs after `multicore_launch_core1_raw` returns. `ipc_shared_layout.h` comment should be rewritten to reflect reality when the header is next touched. |
| P2-5 | 2026-04-12 | M1.1-B USB CDC bridge — intermittent message loss via Meshtastic web console | **Symptoms (observed during post-M1.1-B smoke test):** (a) Sending a text message from Mokya out over LoRa works **sometimes** — neighbouring nodes receive it — but other times the send silently fails with no error surfaced in the web console. (b) Neighbouring nodes sending TO Mokya: the sender's device UI shows "delivered" (ACK received), and Core 0 Meshtastic presumably processed the packet, but the message never appears in the Meshtastic web console text view. One-way `meshtastic --info` protobuf round-trip (M1.1-B close-out test) still works — so the bridge is not totally broken, only lossy under sustained traffic. **Hypotheses (ranked):** (1) `bridge_task` bounded-retry drop-burst at `main_core1_bridge.c:131-168` — when TinyUSB TX FIFO is full for 10 ticks (≈10 ms) the remainder of the ring-slot payload is dropped. If the web console's WebSerial reader isn't draining fast enough, protobuf frames get truncated mid-stream and Meshtastic's web client silently discards partial packets. (2) c1_to_c0 single-slot push from `bridge_task:158-176` reads up to 256 B with `tud_cdc_read` and pushes as one `IPC_MSG_SERIAL_BYTES` slot — if the host sends a frame >256 B in one USB transaction, it may be chunked across reads in a way Core 0's `IpcSerialStream::read` doesn't reassemble cleanly. (3) `usb_device_task` 1-tick (1 ms) yield between `tud_task()` calls — under WebSerial burst traffic, 1 ms may be too slow and endpoint FIFOs back up. (4) FreeRTOS heap fragmentation in the Core 1 image under sustained traffic. **Verification plan (next session, M1.1-B sign-off):** Instrument `bridge_task` with counters for (a) drop-burst events (FIFO stall timeout hit), (b) c0_to_c1 ring overflow count, (c) c1_to_c0 ring overflow count, (d) max observed `tud_cdc_write_available()` deficit. Place counters at `0x2007FFE0+` in `.shared_ipc` tail. Then reproduce the lossy scenarios over web console while watching counters via SWD. Fix strategy depends on which counter moves: if drop-burst, raise retry budget or move to blocking-with-watchdog; if ring overflow, extend slot count or coalesce; if neither, suspect CDC endpoint latency and tune `usb_device_task` yield. **Status:** Theoretical risk confirmed (pop-before-delivery defect exists in code). However, the observed symptom (`meshtastic --info` timeout) was actually caused by P2-8 (CLI version mismatch 2.3.11 vs 2.7.21). M1.1-B baseline works correctly with CLI 2.7.8. Pop-before-delivery fix (M1.2 Part A) remains a hardening improvement for CDC backpressure scenarios but is no longer a blocker. |
| P2-7 | 2026-04-12 | Core 0 ISR stack overflow into breadcrumb area | SWD reads at `0x2007FFC0` show Core 0 SRAM/code pointers (`0x2000F5E4`, `0x100419E8`) instead of the expected sentinel `0xC1B01200`. Core 0 MSP starts at `0x20082000`, SCRATCH_X+Y = 8 KB ends at `0x20080000` — deep ISR nesting or large stack frames overflow into `0x2007FFxx` (the `.shared_ipc` tail where breadcrumbs live). Breadcrumb corruption is cosmetic (counters still readable via offsets), but a deep stack overflow could eventually reach ring control structures at lower addresses. | Investigation deferred. Candidate fixes: (a) increase Core 0 MSP stack reservation, (b) move breadcrumbs to a region above MSP, (c) add stack canary monitoring. |
| P2-8 | 2026-04-12 | CLI version mismatch — false regression in all M1.2 testing | System PATH resolved `meshtastic` to Python 3.11 (Microsoft Store) version **2.3.11**. Active Python 3.13 has `meshtastic` **2.7.8** (via `python -m meshtastic`). Firmware is **2.7.21** — protobuf schema between 2.3.x and 2.7.x is incompatible. All `meshtastic --info` tests returned "Timed out waiting for connection completion" regardless of bridge code changes. Discovered when stock Pico2 on COM7 also timed out with `meshtastic` but succeeded with `python -m meshtastic`. MokyaLora dual-image then also confirmed working with correct CLI — 64 nodes, full config, `pioEnv: "rp2350b-mokya"`. An entire M1.2 debugging session (staged-delivery, drain mode, taskYIELD experiments) was wasted chasing a phantom regression. | Use `python -m meshtastic` for all future testing. Uninstall or deprioritize the Python 3.11 meshtastic package. |
| P2-9 | 2026-04-12 | 2.7.21 mokya variant HardFault on first boot (empty flash / no LittleFS) | **Symptom:** After a full `erase` via J-Link (which wipes the entire 16 MB flash including LittleFS filesystem area), flashing the 2.7.21 mokya variant ELF results in an immediate HardFault on boot. CFSR=0x00008201 (IACCVIOL + IBUSERR), MMFAR/BFAR=0x2008204C — 76 bytes beyond SRAM end (0x20082000). The crash occurs during Meshtastic filesystem initialization when LittleFS finds no valid superblock. **Comparison:** The older 2.7.15 mokya variant ELF does **not** have this bug — it successfully initialises a fresh LittleFS filesystem on empty flash and boots normally. **Recovery procedure:** (1) Flash old 2.7.15 ELF via J-Link → boots and creates LittleFS filesystem. (2) Flash 2.7.21 ELF on top → finds existing filesystem and boots normally. **Root cause:** Not yet investigated. Likely a regression in the Meshtastic LittleFS init path between 2.7.15 and 2.7.21, or a mokya variant config change that triggers an uninitialised pointer / unbounded stack allocation during filesystem format. | Workaround: always ensure LittleFS filesystem exists before flashing 2.7.21. Use 2.7.15 ELF for filesystem init on erased flash. Root cause investigation deferred. |
| P2-6 | 2026-04-12 | M1.1-B USB CDC bridge — slow Meshtastic web console initial handshake | **Symptom:** When the Meshtastic web console attaches to COM16 (MokyaLora via Core 1 bridge), the initial device-configuration handshake (`want_config_id` → full `FromRadio` stream: MyNodeInfo, Metadata, Channel×8, Config×, ModuleConfig×, NodeInfo×86) takes **noticeably longer** than the same firmware build running with Arduino-Pico's native `SerialUSB` (no bridge). One-shot `meshtastic --info` via `pyserial` also feels slower than a reference Meshtastic device but is tolerable; web console's dozens of small-packet exchanges amplify the per-round-trip penalty. **Hypotheses (ranked):** (1) **`usb_device_task` 1-tick yield (`vTaskDelay(pdMS_TO_TICKS(1))`) at `main_core1_bridge.c:97`** — between every `tud_task()` poll. Full-speed USB bulk-IN polling interval is 125 µs, so a 1 ms quantum means the TX endpoint only has ~8 opportunities/ms to push out data, and every request→response round trip eats at least 1 ms on the USB side. (2) **`bridge_task` idle 1-tick yield at `main_core1_bridge.c:192`** — when both directions drain empty, we sleep 1 ms before re-checking. Web console's synchronous request/response protocol means every host→device→host round trip adds 1–2 ms of bridge latency on top of the USB wire time. (3) **No FreeRTOS task notification on ring push** — Core 0's `ipc_ring_push` writes the slot then releases `head`, but does not wake Core 1's `bridge_task`. Core 1 only sees the new slot on its next idle-yield wake-up. Similarly, CDC OUT data only triggers `bridge_task` wake-up 1 ms later. A cross-core sev / FIFO doorbell + `xTaskNotifyFromISR` would let the bridge react within tens of µs. (4) **`tud_cdc_write` micro-chunking** — the `bridge_task` splits each ring slot across `tud_cdc_write_available()` chunks; if avail is small and we re-loop with a 1-tick yield, a single 256 B ring slot can cost multiple ms. **Verification plan:** Measure end-to-end handshake time with `meshtastic --port COMxx --info` scripted against (a) native `SerialUSB` reference build on the same board, (b) current M1.1-B bridge, (c) M1.1-B bridge with `usb_device_task` switched to `taskYIELD()` instead of `vTaskDelay(1)`, (d) M1.1-B bridge with FreeRTOS task-notify on ring push (requires cross-core notify via RP2350 SIO FIFO IRQ). Pick the minimal change that matches (a). **Status:** Confirmed by benchmark (2026-04-12). With correct CLI (2.7.8): bridge ~0.50 KB/s vs native ~12.6 KB/s (**~25× slower per-KB**). Zero drops. Root cause is dual 1 ms vTaskDelay, not the bridge protocol itself. M1.2 Part B (`taskYIELD()` replacement) is the next fix. Cross-core notification deferred to M2. |
