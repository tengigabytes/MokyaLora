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

**Open issues carried forward:** None — P2-9 resolved by P2-11 fix (see Issues Log).

---

## Milestone 2 — Interrupt-driven IPC + graceful reboot

**Status:** ✅ Complete (Parts A + B + P2-11 fix; Part C deferred to M5)
**Goal:** (A) Replace polling with cross-core interrupt notification for µs-latency IPC, (B) fix config-change reboot hanging the COM port, (C) establish IPC handshake v2 for Core 0 restart resilience.

### M2 scope

#### Part A — Cross-core interrupt notification

Replace `taskYIELD()` polling with SIO FIFO doorbell + FreeRTOS task notification:
- Core 0 `ipc_ring_push()` → write SIO FIFO doorbell → Core 1 ISR → `xTaskNotifyFromISR(bridge_task)`
- Core 1 `ipc_ring_push()` → write SIO FIFO doorbell → Core 0 ISR → wake `SerialConsole` (or equivalent)
- `bridge_task` uses `xTaskNotifyWait(portMAX_DELAY)` instead of `taskYIELD()` — zero CPU when idle, µs wake on data
- **Risk:** SIO FIFO IRQ is shared with `multicore_launch_core1_raw()` handshake. Must ensure doorbell usage doesn't conflict with the boot-time FIFO protocol. May need to use RP2350 doorbell registers (separate from FIFO) instead.

#### Part B — Graceful reboot on config change (fixes P2-10)

**Problem:** Meshtastic `Power::reboot()` calls `rp2040.reboot()` → `watchdog_reboot(0, 0, 10)` with no USB disconnect. Core 1's TinyUSB CDC is hard-killed. Windows COM port handle enters error state; Web Console WebSerial connection hangs.

**Fix:** Core 0 `notifyReboot` observer → send `IPC_MSG_PANIC` (or new `IPC_MSG_REBOOT_PENDING`) via ring → Core 1 receives, calls `tud_disconnect()`, waits ~200 ms for host to process disconnect → Core 1 sends `IPC_BOOT_READY` ack back → Core 0 proceeds with `watchdog_reboot()`. Fallback: if Core 1 doesn't ack within 500 ms, reboot anyway (covers Core 1 hang scenario).

#### Part C — IPC handshake v2 (Core 0 restart resilience)

After watchdog reset, both cores cold-start. Handshake v2:
1. Core 0 `initVariant()` zeroes its ring control struct (`c0_to_c1_ctrl.head = tail = 0`)
2. Core 0 sets `c0_ready = 0` before any ring init
3. Core 1 detects `c0_ready == 0` → pauses ring reads, flushes stale data
4. Core 0 completes init → sets `c0_ready = 1` → sends `IPC_BOOT_READY`
5. Core 1 resumes normal bridge operation

This handshake also prepares for M5's Core 0 selective reset (PSM proc0 only), where Core 1 stays alive and needs to survive a Core 0 restart without losing USB or UI state.

### M2 success criteria

1. `python -m meshtastic --port COMxx --info` per-KB throughput within 2× of native SerialUSB (currently 16× slower)
2. Config change via Web Console → device reboots → COM port re-enumerates cleanly → Web Console auto-reconnects (or user can manually reconnect without browser refresh)
3. SWD breadcrumb: `usb_task_polls` rate increases 10×+ vs M1.2-B (confirms polling → interrupt transition)
4. 10-minute sustained Web Console session with periodic config changes — no COM port hangs

### Not in M2 scope

- Core 0 selective reset (PSM proc0 only) — deferred to M5 when `IPCPhoneAPI` + Config IPC adapter are ready
- LVGL UI, display driver, keypad driver — M3/M4
- `IPCPhoneAPI` structured messages — M5

### M2 implementation log (2026-04-13)

#### Part A — Cross-core interrupt notification ✅

Used RP2350 SIO doorbell registers (not SIO FIFO — avoids conflict with `multicore_launch_core1_raw` handshake).

- **Core 1 ISR** (`ipc_doorbell_isr` in `main_core1_bridge.c`): handles `IPC_DOORBELL_NUM` (bit 0) — clears doorbell, fires `xTaskNotifyFromISR(bridge_task)`. Placed in `.time_critical` (RAM) via `__no_inline_not_in_flash_func`.
- **Core 1 bridge_task**: `xTaskNotifyWait(0, UINT32_MAX, NULL, pdMS_TO_TICKS(10))` replaces bare `taskYIELD()`. 10 ms timeout handles CDC OUT events not signaled by doorbell.
- **Core 1 → Core 0**: `doorbell_set_other_core(IPC_DOORBELL_NUM)` after each `ipc_ring_push` in bridge_task.
- **Core 0 side**: No doorbell ISR — FreeRTOS port's `pico_sync_interop` doorbell handler was disabled (MOKYA_DOORBELL_PATCH). Core 0 still discovers c1→c0 data via `SerialConsole::runOnce()` polling (~5 ms). Cross-core doorbell wake for Core 0 deferred to M5 when `IPCPhoneAPI` replaces `SerialConsole`.

**FreeRTOS doorbell deadlock fix (prerequisite):** The RP2350_ARM_NTZ FreeRTOS port registers `prvDoorbellInterruptHandler` on `SIO_IRQ_BELL` at scheduler start. This is a shared IRQ for all 8 doorbell bits. When Core 1 fires doorbell 0 for IPC data, Core 0's handler takes `spin_lock_blocking(pxCrossCoreSpinLock)` and deadlocks (or re-enters endlessly). Fix: `patch_arduinopico.py` wraps the entire doorbell registration block in `#if 0` (MOKYA_DOORBELL_PATCH). Safe because MokyaLora does not use `pico_sync` cross-core primitives.

#### Part B — Graceful reboot (fixes P2-10) ✅

- **Core 0**: `RebootNotifier` (registered via `concurrency::notifyReboot`) pushes `IPC_MSG_REBOOT_NOTIFY` via ring + fires doorbell before `watchdog_reboot()` proceeds.
- **Core 1**: `bridge_task` receives `IPC_MSG_REBOOT_NOTIFY` → calls `tud_disconnect()` → sets `reboot_pending = true` → idles (`vTaskDelay(100 ms)` loop) until watchdog fires chip-wide reset.
- **Result**: Windows sees USB disconnect event before the hard reset. COM port handle closes cleanly. After reboot, Core 1 re-enumerates CDC and the COM port reappears.

#### P2-11 fix — Flash write safety ✅

**Root cause**: Meshtastic config save → `EEPROM.commit()` → `flash_range_erase()` → ROM `flash_exit_xip()` disables XIP. With `__FREERTOS` defined but single-core FreeRTOS, both the framework's `noInterrupts()` guard and `rp2040.idleOtherCore()` are no-ops. SysTick fires during XIP-off → instruction fetch from flash → IACCVIOL HardFault.

**Fix**: Linker `--wrap` for `flash_range_erase` and `flash_range_program` (in `platformio.ini`). Wrapper functions in `flash_safety_wrap.c` (all in `.time_critical` / SRAM):

1. **Core 0 wrapper** (`mokya_flash_park_core1`):
   - If `c1_ready == 1`: sets `flash_lock = REQUEST`, fires `IPC_FLASH_DOORBELL`, bounded spin (~5 ms) until `flash_lock == PARKED`
   - If `c1_ready == 0` (Core 1 not booted): skips park (handles first-boot / P2-9 scenario)
   - Calls `save_and_disable_interrupts()` to prevent SysTick during XIP-off
2. **Core 1 handler** (`flash_park_handler` in `main_core1_bridge.c`):
   - Called from `ipc_doorbell_isr` when `IPC_FLASH_DOORBELL` fires
   - ACKs with `flash_lock = PARKED` + `__sev()`
   - Disables all interrupts, WFE loop until `flash_lock == IDLE`
3. **Core 0 unpark** (`mokya_flash_unpark_core1`): restores interrupts, clears `flash_lock = IDLE`, `__sev()`

**Shared SRAM protocol**: `flash_lock` field at offset 12 of `IpcSharedSram` (was `_reserved0`). States: IDLE(0) → REQUEST(1) → PARKED(2) → IDLE(0). Hardcoded addresses in `flash_safety_wrap.c` (0x2007A008 = `c1_ready`, 0x2007A00C = `flash_lock`) to avoid `#include` dependency on `ipc_shared_layout.h` from the Pico SDK wrapper layer.

**Coverage**: All flash write callers (EEPROM, LittleFS, Preferences, btstack_flash_bank, Updater) are intercepted by the single `--wrap` pair.

**RAM cost**: +136 bytes (.time_critical placement of wrap functions).

#### Part C — IPC handshake v2 — deferred to M5

Watchdog reset cold-starts both cores, so the existing boot handshake (boot_magic + c0_ready + c1_ready) is sufficient for M2. Handshake v2 (Core 0 selective restart resilience) is only needed when M5 implements PSM proc0-only reset.

#### Benchmark: M2 vs M1.2 (2026-04-13)

| Metric | M1.2 | M2 | M2 + P2-13 fix | Change (M1.2 → final) |
|--------|------|----|----------------|----------------------|
| `--info` wall time | 5.9 s | 4.8 s | **4.5 s** | **−24%** |
| Per-frame gap | 12–50 ms | 12–50 ms | **0–1.6 ms** | **30–85×** |
| `--set` + reboot | HardFault (P2-11) | ✅ clean reboot | ✅ clean reboot | Fixed |
| COM port after reboot | Hung (P2-10) | ✅ re-enumerates | ✅ re-enumerates | Fixed |

Throughput improvement is from interrupt-driven IPC (Part A): Core 1 wakes on doorbell instead of waiting for next `taskYIELD()` round-robin cycle. The P2-13 fix (XIP cache re-enable) eliminated the dominant per-frame bottleneck — instruction fetch latency dropped ~100× from uncached QSPI to cached XIP. Burst throughput now exceeds stock Pico2 (2.5× faster frame rate). Remaining `--info` wall-time gap (~0.1 s) is first-byte latency from Core 0's 5 ms `SerialConsole::runOnce()` polling — addressable in M5 when Core 0 gets its own doorbell ISR.

#### Benchmark: `bench_raw_serial2.py` — MokyaLora vs Stock Pico2 (2026-04-13)

Raw protobuf burst profiler (`scripts/bench_raw_serial2.py`): sends `want_config_id`, measures per-frame timestamps. Stock Pico2 = native SerialUSB (firmware 2.7.20, 100 nodes, COM7). MokyaLora = IPC ring + TinyUSB CDC bridge (firmware 2.7.21, 1 node, COM16). 3 runs each; Stock Run 1 excluded (cold-start outlier), averages from Run 2/3.

**Stock Pico2 (COM7, native SerialUSB) — Run 2/3 avg:**

| Metric | Run 2 | Run 3 | Avg |
|--------|-------|-------|-----|
| Send → first byte | 3.0 ms | 4.4 ms | 3.7 ms |
| Send → first frame | 41.5 ms | 43.7 ms | 42.6 ms |
| First → last frame | 195.9 ms | 196.0 ms | 196.0 ms |
| Total frames | 100 | 100 | 100 |
| Total payload | 6 460 B | 6 460 B | 6 460 B |
| Frame gap (typical) | 1.2–3.3 ms | 2.9–3.3 ms | 1.2–3.3 ms |
| Frame gap (tail, large frames) | 20.4 ms | 20.5 ms | 20.5 ms |
| Read syscalls | 44 | 50 | 47 |

**MokyaLora (COM16, M2 + P2-13 XIP cache fix, LOG enabled) — 3-run avg:**

| Metric | Run 1 | Run 2 | Run 3 | Avg |
|--------|-------|-------|-------|-----|
| Send → first byte | 562.7 ms | 932.4 ms | 977.2 ms | 824.1 ms |
| Send → first frame | 588.5 ms | 958.1 ms | 1003.2 ms | 849.9 ms |
| First → last frame | 42.8 ms | 24.5 ms | 42.9 ms | 36.7 ms |
| Total frames | 48 | 47 | 47 | 47 |
| Total payload | 879 B | 873 B | 873 B | 875 B |
| Frame gap (typical) | 0.0–1.6 ms | 0.0–1.6 ms | 0.0–1.6 ms | 0–1.6 ms |
| Frame gap (max, tail LOG spike) | 20.4 ms | 0.0 ms | 20.4 ms | ~20 ms |
| Read syscalls | 8 | 6 | 8 | 7.3 |

**Head-to-head comparison (stable runs):**

| Metric | Stock Pico2 | MokyaLora | Ratio |
|--------|-------------|-----------|-------|
| Burst duration (first→last) | 196 ms / 100 frm | 37 ms / 47 frm | — |
| Frame rate | 0.51 frm/ms | 1.28 frm/ms | **MokyaLora 2.5× faster** |
| Avg per-frame time | 1.96 ms | 0.78 ms | **MokyaLora 2.5× faster** |
| Per-frame gap (typical) | 1.2–3.3 ms | 0–1.6 ms | **MokyaLora better** |
| Per-frame gap (tail spike) | 20.5 ms | 20.4 ms | **Parity** (LOG output) |
| First byte latency | 3.7 ms | 824 ms | **Stock 223× faster** |
| `--info` wall time | 4.4 s | 4.5 s | **Parity** |

**Analysis:**
- Burst throughput (frame rate during config exchange) **exceeds** stock Pico2 by 2.5×, thanks to XIP cache fix. Both platforms have LOG enabled; MokyaLora's IPC ring batches multiple frames into single USB transactions.
- Per-frame gap 0–1.6 ms matches stock's 1.2–3.3 ms range. Both show ~20 ms tail spikes from LOG output on large frames (NodeInfo).
- `DEBUG_MUTE` was tested during investigation and gave an additional ~45% improvement, but was **removed** — LOG overhead is negligible with XIP cache enabled, and debuggability is more important.
- **Sole remaining bottleneck: first byte latency (~824 ms).** Root cause: Core 0 `SerialConsole::runOnce()` returns 5 ms (recent) / 250 ms (idle) poll interval — `want_config_id` sits in the IPC ring until the next `readStream()` poll. Stock Pico2's `SerialConsole` reads directly from USB endpoint buffer (no IPC hop) but still has the same 5 ms polling architecture — the 3.7 ms first-byte time means the `want_config_id` arrived during an active poll window. MokyaLora's ~824 ms is 250 ms idle poll + Meshtastic state machine warm-up. Fix: M5 Core 0 doorbell ISR to wake `SerialConsole` immediately on ring data arrival.

**Full evolution across all stages:**

| Stage | `--info` wall | Burst (first→last) | Per-frame gap | vs Stock |
|-------|--------------|--------------------|--------------|---------| 
| Stock Pico2 (native USB) | 4.4 s | 196 ms / 100 frm | 1.2–3.3 ms | baseline |
| M1.1-B (dual vTaskDelay) | 15.0 s | — | 12–50 ms | 3.4× slower |
| M1.2-B (taskYIELD + TX accum) | 5.9 s | — | 12–50 ms | 1.3× slower |
| M2 (doorbell IPC) | 4.8 s | — | 12–50 ms | 1.1× slower |
| **M2 + P2-13 (XIP cache fix)** | **4.5 s** | **37 ms / 47 frm** | **0–1.6 ms** | **parity (burst 2.5× faster)** |

#### M2 close-out (2026-04-13)

**Files changed (parent repo):**
- `firmware/shared/ipc/ipc_shared_layout.h` — added `IPC_FLASH_DOORBELL`, `IPC_FLASH_LOCK_*` defines, replaced `_reserved0` with `flash_lock`
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` — added `flash_park_handler()` + multi-doorbell ISR dispatch

**Files changed (Meshtastic submodule `firmware/core0/meshtastic/`):**
- `variants/rp2350/rp2350b-mokya/flash_safety_wrap.c` — NEW, `--wrap` flash safety wrappers + P2-13 XIP cache re-enable after flash ops
- `variants/rp2350/rp2350b-mokya/variant.cpp` — P2-13 XIP cache enable at boot (initVariant)
- `variants/rp2350/rp2350b-mokya/platformio.ini` — added `-Wl,--wrap` flags + `flash_safety_wrap.c` to build
- `variants/rp2350/rp2350b-mokya/patch_arduinopico.py` — removed dead flash.c patch code, added NOTE

#### P2-12 fix — detachInterrupt ISR-unsafe under FreeRTOS ✅ (2026-04-13)

**Symptom:** Device HardFaults when receiving a LoRa message. CFSR=0x00020001 (INVSTATE+IACCVIOL), IPSR=0x25 (IO_IRQ_BANK0 = SX1262 DIO1 GPIO interrupt). MSP=0x20079F30 — stack overflow of ~34 KB from `__StackTop` (0x20082000), penetrating SCRATCH, shared IPC, and heap regions.

**Root cause chain:**
1. SX1262 DIO1 fires IO_IRQ_BANK0
2. `RadioLibInterface::isrLevel0Common()` calls `disableInterrupt()` → `SX126x::clearDio1Action()` → `detachInterrupt(GPIO 29)`
3. Arduino-Pico `detachInterrupt()` constructs `CoreMutex(&_irqMutex)` → calls `__get_freertos_mutex_for_ptr(&_irqMutex)`
4. If the FreeRTOS semaphore wrapper for `_irqMutex` hasn't been lazily created yet, `__get_freertos_mutex_for_ptr` → `xQueueCreateMutex` → `pvPortMalloc` — **pvPortMalloc uses a critical section, `vPortExitCritical` detects ISR context** → `rtosFatalError`
5. `rtosFatalError` → `panic()` → `puts()` (to print error) → `_puts_r` → `__retarget_lock_acquire_recursive` (stdout lock) → `__get_freertos_mutex_for_ptr` (lazy init for stdout lock) → `pvPortMalloc` → **recursive malloc from ISR** → stack overflow (~177 recursive iterations)
6. Eventually stack corruption → `xQueueSemaphoreTake` branches to corrupted function pointer 0x2006F01C → INVSTATE HardFault

**Why this didn't crash before:** LoRa reception was not tested until this session. The ISR path through `detachInterrupt` → `CoreMutex` → malloc is always ISR-unsafe under FreeRTOS, but the crash only triggers when a LoRa packet is actually received.

**Fix:** `patch_arduinopico.py` patches `wiring_private.cpp` — when `portCHECK_IF_IN_ISR()` returns true, `detachInterrupt` calls `_detachInterruptInternal(pin)` directly (just `gpio_set_irq_enabled` + bitmask clear), skipping `CoreMutex` entirely. This is safe because ISR context is already at interrupt priority — no lower-priority interrupt can preempt. Patch marker: `MOKYA_ISR_DETACH_PATCH`.

**Verified:** LoRa message reception now works without crash.

**Open issues carried forward:** None — P2-9 resolved by P2-11 fix (see Issues Log).

#### P2-13 fix — XIP cache disabled since boot ✅ (2026-04-13)

**Symptom:** `meshtastic --info` takes 15.0 s vs stock Pico2's ~3.2 s. DWT CYCCNT profiling of `writeStream()` showed `getFromRadio()` consuming 5–39 ms per frame (750K–5.8M cycles at 150 MHz). Calibration loop (10000 volatile ADD iterations): ~690 cycles/iter vs expected ~7 → ~100× instruction fetch slowdown.

**Investigation sequence:**
1. **DWT CYCCNT profiling v1/v2** — instrumented `StreamAPI::writeStream()` with per-frame cycle counters at `0x2007FE00`. Confirmed 98.5% of frame time in `getFromRadio()`, only 1.5% in `emitTxBuffer()` (IPC ring push).
2. **`-DDEBUG_MUTE` experiment** — compile-time suppression of all `LOG_*` macros. Result: ~45% improvement (15 s → 5.9 s). Significant but not the dominant factor — early frames still 10–40 ms.
3. **FreeRTOS preemption check** — enumerated all Core 0 tasks: CORE0 (pri 4), Timer Svc (7), Idle (0). No preemption issue — OSThreads are cooperative within the single CORE0 FreeRTOS task.
4. **SWD register reads** — PLL: FBDIV=125, PD1=5, PD2=2 → 150 MHz confirmed. QMI M0_TIMING: CLKDIV=2 → 37.5 MHz SPI. **XIP_CTRL at 0x400C8000 = 0x00000000** — both EN_SECURE and EN_NONSECURE cleared, 4 KB XIP cache disabled.
5. **Root cause trace** — searched Pico SDK boot2, `psram.cpp`, `flash.c`, `runtime_init.c`, `xip_cache.c`. No SDK code explicitly clears XIP_CTRL. The register's hardware reset value is 0x00000083 (cache ON). Clearing happens via: (a) PSRAM detection's QMI direct mode entry/exit during `runtime_init_setup_psram` (priority 11001), and/or (b) ROM `flash_exit_xip()` called during any `flash_range_erase/program`. Boot2 copyout (`boot2_generic_03h.S`) only restores QMI registers (M0_TIMING, M0_RCMD, M0_RFMT) — never touches XIP_CTRL. This is a Pico SDK gap affecting all RP2350 boards, but stock boards don't notice because native SerialUSB masks the latency.

**Fix (two sites):**
1. `variant.cpp:initVariant()` — writes `0x03` to XIP_CTRL SET alias (`0x400CA000`) at boot, before any Meshtastic code. Uses SET alias for atomic bit-set without disturbing other XIP_CTRL fields (e.g. WRITABLE_M1 from `psram_init`).
2. `flash_safety_wrap.c:__wrap_flash_range_erase/program` — after each `__real_*` call (which internally runs `flash_exit_xip` → flash op → `flash_enable_xip_via_boot2`), re-enables cache via `MOKYA_XIP_CTRL_SET = 0x03`.

**Verified (final — LOG enabled, no DEBUG_MUTE):**
- XIP_CTRL = 0x00000003 (SWD confirmed)
- Calibration: 72,274 cycles / 10000 iter = 7.2 cycles/iter (was ~690 → **96× improvement**)
- `getFromRadio()`: 0.13–0.69 ms/frame (was 5–39 ms → **30–85× improvement**)
- `--info` wall time: 4.5 s (was 15.0 s → **3.3× improvement**)
- Per-frame gap: 0–1.6 ms typical, ~20 ms tail spike on large frames (identical to stock Pico2)
- Burst rate: 1.28 frm/ms = **2.5× faster** than stock Pico2 (0.51 frm/ms)
- `DEBUG_MUTE` tested during investigation (+45% with cache off), then removed — LOG overhead negligible with cache on (~3%)

**DWT profiling data (first 24 of 51 frames, with cache ON):**

| Frame | getFromRadio (cycles) | ms | emitTxBuffer (cycles) | ms | Payload (B) |
|-------|----------------------|-----|----------------------|-----|-------------|
| 0 (MY_INFO) | 40,260 | 0.27 | 5,847 | 0.04 | 29 |
| 2 (NODEINFO) | 103,035 | 0.69 | 2,810 | 0.02 | 115 |
| 5–11 (CHANNELS) | 19K–22K | 0.13–0.15 | 2.5K–2.8K | 0.02 | 6 |
| 13–17 (CONFIG) | 30K–43K | 0.20–0.29 | 2.6K–3.3K | 0.02 | 7–28 |

**`-DDEBUG_MUTE` experiment (reverted):** Compile-time LOG suppression was tested during investigation and gave ~45% independent improvement (15 s → 5.9 s with cache still off). After XIP cache fix, LOG overhead became negligible (~3% of `--info` wall time), and both MokyaLora and stock Pico2 show identical ~20 ms tail spikes from LOG on large frames. `DEBUG_MUTE` was removed to preserve debuggability.

**Files changed (Meshtastic submodule):**
- `variants/rp2350/rp2350b-mokya/variant.cpp` — added XIP cache enable in `initVariant()`
- `variants/rp2350/rp2350b-mokya/flash_safety_wrap.c` — added XIP cache re-enable after each wrapped flash op
- `src/mesh/StreamAPI.cpp` — DWT profiling instrumentation added then removed (clean)

---

## Milestone 3 — Core 1 HAL drivers + LVGL UI runtime

**Status:** 🚧 In progress (M3.1 ✅, M3.2 ✅, M3.3 ✅, M3.4.1/.2/.3 ✅, M3.4.4+ next)
**Goal:** Bring up Core 1's user-facing hardware (display, keypad, sensors, power) as FreeRTOS-task driven HAL modules under `firmware/core1/src/`, and stand up LVGL v9.2.2 as the rendering runtime. Keypad driver is written from day-1 against the multi-producer `KeyEvent` queue with `key_source_t` flag (G2 from DEC-2), so M9 USB Control injection only adds a producer — never refactors the queue.

### M3 sub-milestones

| Sub-milestone | Deliverable |
|---------------|-------------|
| M3.1 | ST7789VI display driver standalone (PIO 8080-8 + DMA, panel init, partial flush, TE polling, LM27965 backlight) |
| M3.2 | LVGL v9.2.2 integration — `lv_display_t` flush_cb wired to `display_flush_rect`, FreeRTOS tick source, `lv_timer_handler` task, hello-world screen |
| M3.3 | 6×6 keypad PIO scanner + debounce → `keymap_matrix.h` translation → multi-producer `KeyEvent` queue (HW source flag) |
| M3.4 | Sensor + power HAL — IMU / mag / baro on sensor+GNSS bus (GPIO 34/35) + charger / fuel gauge / LED driver on power bus (GPIO 6/7). Rev A: both pin pairs are I2C1-only on RP2350, so firmware time-muxes the single `i2c1` peripheral via a FreeRTOS mutex (`firmware/core1/src/i2c/i2c_bus.c`). GPS bridge to Core 0 via shared-SRAM double-buffer. |

### Not in M3 scope

- LVGL custom font driver loading `font_glyphs.bin` from PSRAM — M4
- MIE RP2350 PIO HAL (KeyEvent queue → MIE processor) — M4
- USB Control Interface (`UsbCtrlTask` second producer) — M9
- Audio drivers — removed from project scope

### M3 implementation log

#### M3.1 — Display driver standalone ✅ (2026-04-15)

LVGL v9.2.2 vendored under `firmware/core1/lvgl/` (commit `af405ad`). ST7789VI driver landed under `firmware/core1/src/display/` (commit `e70c28b`):

- **Bus**: PIO1 program drives nWR + D[7:0] at 80 ns write cycle (`DISPLAY_PIO_CLKDIV = 3.0` @ 150 MHz); nCS / DCX / nRST / TE stay on SIO. Single DMA channel feeds the SM TX FIFO with autopull pacing.
- **Panel init**: ST7789VI sequence (SLPOUT, COLMOD 0x55, MADCTL, INVON, NORON, DISPON) per `st7789vi.c`.
- **Backlight**: LM27965 on power-bus i2c1 (GPIO 6/7), Bank A duty `0x16`, GP `0x21` to enable TFT rail.
- **Public API**: `display_init()`, `display_flush_rect(x0,y0,x1,y1, pixels)` (RGB565 big-endian byte order — bytes[0]=hi, bytes[1]=lo for COLMOD 0x55), `display_wait_te_rise()` (M3.1 polls GPIO 22; M3.2 will switch to GPIO IRQ + task notify), `display_fill_solid(rgb565)`.
- **Standalone test**: `display_test_task` cycles red → green → blue → white → black at 1 Hz via `display_fill_solid`. Runs at the same FreeRTOS priority as `usb_device_task` / `bridge_task` so the round-robin scheduler hands it CPU. Will be replaced by the LVGL flush path in M3.2.

**P3-1 fix — Core 1 SysTick reload silently 0xFFFFFF:** The RP2350_ARM_NTZ port's default `vPortSetupTimerInterrupt()` derives the SysTick reload from `clock_get_hz(clk_sys)`. But Core 1 skips `runtime_init_clocks` (Core 0 owns clock init), so `configured_freq[clk_sys]` stays 0 and the reload silently wraps to `0xFFFFFF` — tick rate collapses to ~9 Hz, making `vTaskDelay(1000 ms)` wait ~112 s. Fix: override `vPortSetupTimerInterrupt` in `main_core1_bridge.c` to write `(configCPU_CLOCK_HZ / configTICK_RATE_HZ) - 1` directly (150 MHz / 1000 Hz = 150 000 - 1). Tick rate now matches `configTICK_RATE_HZ` and the 1-second colour cycle ticks over correctly.

**Files added (parent repo):**
- `firmware/core1/lvgl/` — LVGL v9.2.2 vendored (examples/demos/ThorVG disabled via CMake options)
- `firmware/core1/m1_bridge/src/lv_conf.h` — `LV_COLOR_DEPTH=16`, `LV_USE_OS=LV_OS_FREERTOS`, `LV_USE_FREERTOS_TASK_NOTIFY=1`, `LV_MEM_SIZE=48 KB`, `LV_DEF_REFR_PERIOD=5 ms`
- `firmware/core1/src/display/{display.[ch], st7789vi.[ch], tft_8080.pio, tft_8080.pio.h}`
- `firmware/core1/m1_bridge/CMakeLists.txt` — added LVGL subdirectory + display sources + `pico_generate_pio_header`
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` — `display_test_task`, `vPortSetupTimerInterrupt` override

#### M3.2 — LVGL flush_cb integration ✅ (2026-04-15)

`display_test_task` replaced by LVGL-driven render loop in
`firmware/core1/src/display/lvgl_glue.[ch]`. Red → green → blue background
rotation visually confirmed on the Rev A panel; flush throughput ~48
flushes/sec (= 6 full 240×320 frames/sec with the 40-line partial buffer).

- **Draw buffer**: single `240 × 40` RGB565 buffer = 19 200 B, BSS static, 4-byte aligned. Partial render mode.
- **Tick source**: `lv_tick_set_cb(xTaskGetTickCount)` — `configTICK_RATE_HZ=1000` so the value is already in ms.
- **flush_cb**: `lv_draw_sw_rgb565_swap(px_map, pixel_count)` in place (LVGL renders little-endian, the panel under COLMOD 0x55 expects big-endian), then blocking `display_flush_rect(x0,y0,x1,y1, px_map)`, then `lv_display_flush_ready(disp)` synchronously. M3.3 will replace the blocking wait with a DMA-complete task notification + TE IRQ for tearing avoidance.
- **Task**: `lvgl_task` (priority `tskIDLE_PRIORITY + 2`, 16 KB stack, 4096 words). Runs `display_init()` → `lv_init()` → `lv_display_create(240, 320)` → buffer + flush_cb setup, then loops `lv_timer_handler()` + `vTaskDelay(pdMS_TO_TICKS(next))` where `next` is clamped to `[LV_DEF_REFR_PERIOD, 100 ms]`.
- **Smoke test**: active screen `bg_color` cycles red → green → blue on a 1 Hz `xTaskGetTickCount`-based timer inside the task loop.

**P3-2 fix — LVGL FreeRTOS OSAL recursive-mutex deadlock:** First call to `lv_timer_handler()` blocked forever. Root cause: LVGL v9.2.2's FreeRTOS OSAL creates its global mutex with `xSemaphoreCreateRecursiveMutex()` (`lv_freertos.c:438`) but acquires it with the *non-recursive* `xSemaphoreTake()` (`lv_freertos.c:132`). On FreeRTOS that combination blocks on the first `lv_lock()` inside `lv_timer_handler()`. Fix: switch `LV_USE_OS` from `LV_OS_FREERTOS` to `LV_OS_NONE` in `lv_conf.h`. Our LVGL access is serialised through a single task (`lvgl_task`) so the OSAL lock is redundant anyway; revisit if a future milestone needs multi-task LVGL access.

**Files added:**
- `firmware/core1/src/display/lvgl_glue.[ch]`
- `firmware/core1/m1_bridge/CMakeLists.txt` — added `lvgl_glue.c` to sources

**Files changed:**
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` — `display_test_task` removed, `lvgl_glue_start(tskIDLE_PRIORITY + 2)` wired in its place
- `firmware/core1/m1_bridge/src/lv_conf.h` — `LV_USE_OS = LV_OS_NONE` (see P3-2), `LV_ASSERT_HANDLER` stamps `0xA55E1700` at `0x2007FFF8` before spinning so an LVGL assert is distinguishable from a generic hang over SWD

**P3-3 fix — LM27965 backlight I2C clock misconfiguration:** Backlight only ~20% brightness instead of target 40%. Root cause: Core 1 skips `runtime_init_clocks` (Core 0 owns clock init), so `clock_get_hz(clk_peri)` returns 0. `i2c_init()` uses this to compute the baudrate divisor, resulting in garbage SCL timing. BANKA duty write consistently NACKed while GP write succeeded by timing luck. Fix: add `i2c_set_baudrate_core1()` that manually computes SCL timing from the known clk_peri frequency (150 MHz). Also adds `bus_b_recovery()` (9 SCL pulses + manual STOP) and SWD diagnostic breadcrumb at `0x2007FFF4`.

**P3-4 perf — Display flush optimization (4.5× speedup):**

| Change | Before | After |
|--------|--------|-------|
| PIO clkdiv | 3.0 (80 ns) | 2.0 (53 ns) |
| Byte-swap | Per-pixel C loop | ARM REV16 (2 pixels/iter) |
| Flush time | 6.3 ms | 1.4 ms |

Also enabled `LV_USE_SYSMON = 1` for benchmark FPS overlay, `LV_FONT_MONTSERRAT_24` for benchmark demo, adjusted `LV_DEF_REFR_PERIOD` to 33 ms (realistic for DIRECT mode blocking flush).

#### M3.3 — Keypad driver ✅ (2026-04-18)

Delivered in two phases. **Phase A** (commit `b6a9665`) switched the 6×6
matrix scanner from CPU polling to PIO + 2 DMA channels (zero CPU after
`keypad_init()`), with an RP2350B board-header fix (`mokya_rev_a.h`) so
GPIO 36–47 pads stop silently no-op'ing (see Gotcha in
`core1-driver-development.md` §7.2).

**Phase B** (this commit) layers per-key debounce, matrix→keycode
translation, and the multi-producer `KeyEvent` queue on top of the
Phase A raw scanner.

- **Debounce**: `keypad_scan_task` runs every 5 ms and maintains three
  bytes of per-key state (`stable`, `pending`, `count`). A new reading
  must match the pending candidate for 4 consecutive ticks (= 20 ms)
  before it commits and triggers an enqueue. File-static arrays so the
  task stack stays at 512 words (§4.1 heap budget).
- **Keymap**: `firmware/core1/src/keypad/keymap_matrix.h` holds the sole
  `(r, c) → mokya_keycode_t` LUT (Apache-2.0, Core 1 private per DEC-1).
  The (r, c) order is the firmware scan order from Step 6 of the Rev A
  bring-up log — not the hardware-requirements electrical matrix.
- **Queue**: `key_event.[ch]` wraps a 16-slot × 2-byte FreeRTOS queue
  with a 64-bit "HW currently pressed" bitmap. `key_event_push_hw()`
  updates the bitmap before enqueuing. `key_event_push_inject()`
  (reserved for M9 `UsbCtrlTask`) rejects with `ERR_BUSY` if HW already
  holds the same keycode — the §9.1 arbitration rule locked in so M9
  only adds a producer, never refactors the queue.
- **Observability (kept after bring-up)**: `g_kp_scan_tick` counts scan
  iterations, `g_kp_stable[6]` mirrors the debounced state bitmap,
  `g_key_event_pushed / dropped / rejected` are the SWD counters, and a
  16-entry `g_key_event_log[]` ring captures `(pressed<<7 | keycode)`
  for every enqueued event so keymap translation is verifiable over SWD
  without a downstream consumer. All symbols are name-resolved (not
  fixed-address), so they sit in Core 1's BSS and don't consume the
  §9.3 breadcrumb region.

**Verification (2026-04-18)**: 5 physical press/releases across R1 C0–C4
produced exactly 10 events (`0x81 0x01 0x82 0x02 0x83 0x03 0x84 0x04
0x85 0x05`) decoding to `MOKYA_KEY_1, 3, 5, 7, 9` press/release pairs —
keymap translation correct. `meshtastic --info` full-config round-trip
still works → no bridge regression.

**Files added:**
- `firmware/core1/src/keypad/keymap_matrix.h`
- `firmware/core1/src/keypad/key_event.{h,c}`

**Files changed:**
- `firmware/core1/src/keypad/keypad_scan.{h,c}` — added `keypad_scan_task` with debounce + keymap + queue push
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` — replaced `keypad_probe_task` with `keypad_scan_task`, added `key_event_init()` before scheduler start
- `firmware/core1/m1_bridge/CMakeLists.txt` — added `key_event.c` source and `firmware/mie/include` to include dirs (for `mie/keycode.h`)

**Phase C** — LVGL keypad visualiser + landscape display (2026-04-18).

The Phase B queue had no consumer; this phase adds one inside `lvgl_task`
so `g_key_event_dropped` actually exercises the pop path, and switches
the panel to landscape so the diagnostic view matches how the PCB is held.

- **Display rotation**: `ST7789_MADCTL` changed from `0x00` (portrait)
  to `0x60` (MV=1, MX=1 → 320×240 landscape, keypad-side top-left).
  `DISPLAY_W/H` in `display.h` swapped to 320/240. Framebuffer byte
  count unchanged (150 KB); `s_flush_scratch` grows from 480 B to 640 B.
  `LCMCTRL` left at its portrait value `0x2C` — no visual regression
  observed. If colour order ever looks wrong revisit this first.
- **Consumer**: `keypad_view_tick()` drains `key_event_pop(..., 0)` at
  the end of every `lvgl_task` iteration. No new FreeRTOS task — LVGL
  runs with `LV_USE_OS = LV_OS_NONE`, so all widget mutations must stay
  on the `lvgl_task` context (P3-2 lesson).
- **Layout** mirrors the physical PCB:
  - upper-left: FUNC / BACK (stacked)
  - upper-centre: DPAD cross (UP / LEFT / OK / RIGHT / DOWN)
  - upper-right: 2×2 column-major — left col SET (top) / DEL (bot),
    right col V+ (top) / V- (bot)
  - lower: 5×5 half-keyboard (digits row → MODE/TAB/SPACE/SYM1/SYM2)
  - status strip at y=0..13 showing `LAST: <name> P|R` and
    `p=<pushed> d=<dropped> r=<rejected>`

**Verification (2026-04-18)**: `meshtastic --info` full round-trip still
works (no bridge regression). SWD snapshot after boot: `g_key_event_pushed`
monotonic, `g_key_event_dropped = 0`, `xFreeBytesRemaining ≈ 5.1 KB`
(unchanged from Phase B — LVGL widgets come out of the separate 48 KB
`lv_mem` pool, not FreeRTOS heap). Visual press/release colour toggle
confirmed on the panel.

**Files added:**
- `firmware/core1/src/keypad/key_name.h` — keycode → short display name
- `firmware/core1/src/ui/keypad_view.{h,c}` — 6×6 LVGL view + consumer

**Files changed:**
- `firmware/core1/src/display/display.h` — `DISPLAY_W=320`, `DISPLAY_H=240`
- `firmware/core1/src/display/st7789vi.c` — `MADCTL = 0x60` (landscape)
- `firmware/core1/src/display/lvgl_glue.c` — removed benchmark, calls
  `keypad_view_init()` + `keypad_view_tick()` each lv_timer iteration
- `firmware/core1/m1_bridge/CMakeLists.txt` — new `UI_DIR` + `keypad_view.c`

#### M3.4.1 — Shared I2C bus module ✅ (2026-04-18)

**Discovery:** both power (GPIO 6/7) and sensor+GNSS (GPIO 34/35) pin pairs
route to the `i2c1` SDK peripheral only. RP2350's I2C pinmux follows a
strict mod-4 rule (see `docs/design-notes/mcu-gpio-allocation.md` §I2C Bus
Allocation) — `GPIO mod 4 == 2/3` has no I2C0 alternative. The two buses
therefore cannot run as separate SDK peripherals as the earlier plan
assumed; the original `commit c18cf8a` "power=i2c0, sensor+GNSS=i2c1" was
based on incorrect docs.

**Resolution:** time-mux a single `i2c1` peripheral between the two pin
pairs. `firmware/core1/src/i2c/i2c_bus.c` owns the peripheral, a FreeRTOS
mutex, and the FUNCSEL swap (~200 ns per switch vs 90 µs/byte at 100 kHz
— negligible). Drivers call `i2c_bus_acquire(id, timeout)` to take the
mutex + remux, `i2c_bus_release()` when done. Cold-boot regression
verified: backlight lights after a power-cycle (initial bug was traced
to GPIO 6/7 muxed to the wrong peripheral).

**Rev B action** logged as Issue #15 in `rev-a-bringup-log.md`: reroute
sensor + GNSS bus to a mod-4 = 0/1 pair (candidate GPIO 32/33 → `i2c0`)
so the two peripherals can run concurrently. Also avoids any temptation
to merge both rails into one bus (power = 1.8 V pull-up, sensor = 3.3 V —
different voltage domains, cannot share without a level shifter).

Docs updated across `CLAUDE.md`, `mcu-gpio-allocation.md`,
`firmware-architecture.md`, `power-architecture.md`,
`hardware-requirements.md`, and this file.

#### M3.4.2 — BQ25622 charger driver ✅ (2026-04-18)

First production driver on top of the shared I2C module. Datasheet
SLUSEG2D §8.5/§8.6 is followed end-to-end:

- **Field packing from bit-step constants** (no magic numbers): VREG 10 mV,
  ICHG 80 mA, IINDPM 20 mA per §8.6.2.1/2/3.
- **16-bit registers via 3-byte multi-write** (§8.5.1.7) so both halves of
  VREG/ICHG/IINDPM land atomically from the chip's perspective.
- **ADC block read as a single 12-byte burst** from REG0x28 (repeated
  start, no STOP between pointer-set and block-read).
- **Read-modify-write on CTRL1** preserves non-owned bits (`EN_AUTO_IBATDIS`
  etc. stay at POR default while we only change `WATCHDOG` / `WD_RST` /
  `EN_CHG`).

Production settings:
- VREG = 4100 mV (BL-4C, 100 mV below max for cycle life)
- ICHG = 480 mA (~0.5C)
- IINDPM = 500 mA (Step 20 value)
- **WATCHDOG = 50 s + 1 Hz kick** (shortest window, fastest fault detection)
- ADC = 12-bit continuous, all six channels

**Watchdog expiry recovery:** `charger_task` checks `WD_STAT` each cycle;
if set, the task re-runs `hw_init()` in place so the configured
VREG/ICHG/IINDPM/WATCHDOG/ADC_CTRL are re-applied. `wd_expired_count`
exposes the event counter via the public `bq25622_state_t` snapshot.

**Verification (battery + USB, charging disabled by HW nCE):**
- VBUS 5006 mV · VBAT 4065 mV · VSYS 4123 mV · VPMID 4986 mV
- VSYS − VBAT = 58 mV, matching datasheet §8.3.4.1 NVDC spec
  (VSYS = VBAT + 50 mV typ when charging disabled). This specific
  relationship is the strongest evidence the field decode / register
  writes / ADC pipeline are all correct end-to-end — it is very hard to
  produce this exact 58 mV offset by coincidence.
- CHG_STAT = NoCHG (correct — nCE HW-disables the charger path)
- All fault bits clear, `wd_expired_count=0`, `i2c_fail_count=0`

**API:**
- `bq25622_start_task(priority)` — creates 1 Hz `charger_task` (stack
  512 words = 2 KB, fits the 32 KB heap with ~3 KB free post-boot)
- `bq25622_get_state()` → pointer to the globally-updated snapshot
- `bq25622_set_charge_enabled(bool)`
- `bq25622_set_watchdog(window)` — OFF / 50 s / 100 s / 200 s, kicks
  WD_RST in the same write to prevent old-timer expiry mid-transition
- `bq25622_set_hiz(bool)` — high-impedance mode; used by the future
  sleep/DORMANT state machine. Auto-cleared on WATCHDOG expiry (§8.6.2.12).
- `bq25622_set_batfet_mode(mode)` — NORMAL / SHUTDOWN / SHIP / SYSRESET
  via CTRL3[1:0]. BATFET_DLY left at POR 1 (12.5 s delay) so the host has
  time to finish shutdown work. SHIP fully disconnects the battery — do
  not call on battery-only power.

**ADC block extension:** after first validation, the ADC burst was extended
from 12 to 16 bytes (REG0x28..REG0x37) to include TS_ADC (bits [11:0]
unsigned, 0.0961 %/LSB) and TDIE_ADC (bits [11:0] 2's complement,
0.5 °C/LSB). State adds `ts_pct_x10` and `tdie_cx10`. Verification:
TDIE = 22.0 °C at room temperature, TS ≈ 56.1 % for the 10 kΩ NTC
divider — both independently confirm the field decoders.

**Files added:**
- `firmware/core1/src/power/bq25622.{h,c}`

**Files changed:**
- `firmware/core1/m1_bridge/CMakeLists.txt` — `POWER_DIR` + source
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` — `bq25622_start_task()`
  alongside other task creates

#### M3.4.3 — LM27965 LED driver refactor ✅ (2026-04-18)

Replaces the provisional `backlight_init()` inline in `display.c` with a
standalone 3-bank LED driver under `firmware/core1/src/power/`. The
driver owns all GP / Bank-A / Bank-B / Bank-C register writes and
maintains a cached GP byte so partial updates (e.g. toggling EN3B) do
not clobber other enables.

**API:**
- `lm27965_init(tft_duty)` — writes Bank A duty before asserting ENA to
  avoid a current-spike flash at the previous POR duty (31/31 full
  scale). Bank B / C start off.
- `lm27965_set_tft_backlight(duty)` — 5-bit code; duty 0 auto-clears ENA
  for software-driven fade-to-off.
- `lm27965_set_keypad_backlight(kbd_on, green_on, duty)` — Rev A Issue
  #6 couples D3B green and D1B/D2B through the Bank-B duty register, so
  the two enables are independent but the duty is shared.
- `lm27965_set_led_red(on, duty)` — Bank C, 2-bit code (4 steps).
- `lm27965_all_off()` — clears all GP enables, keeps the reserved bit 5
  set. Bank duty registers preserved so a subsequent re-enable restores
  the last brightness (fast sleep ↔ wake).
- `lm27965_get_state()` — cached snapshot for UI / SWD observers.

Call-site move: `lvgl_task` now calls `display_init()` (panel up,
backlight still off) followed by `lm27965_init(0x16)` so the panel has
already loaded GRAM contents before the backlight lights up — no flash
of garbage. `display.c` loses all I2C / LM27965 code.

**Smoke test (2026-04-18):** boot → TFT 40 % → red 100 % → kbd+green
40 % → TFT 100 % → all off → TFT 40 %. All six transitions visually
confirmed on the board.

**Files added:**
- `firmware/core1/src/power/lm27965.{h,c}`

**Files changed:**
- `firmware/core1/src/display/display.c` — backlight_init removed; i2c /
  i2c_bus includes dropped
- `firmware/core1/src/display/lvgl_glue.c` — calls `lm27965_init()`
  post `display_init()`
- `firmware/core1/m1_bridge/CMakeLists.txt` — new `lm27965.c` source

#### M3.4.4 — BQ27441 fuel gauge driver stub ✅ (2026-04-18)

Rev A's BQ27441-G1 has two production-blocking defects (Issues #9 + #10
in the bringup log): BIN pin unconnected and a cold-boot I2C NACK
latchup that resists 9-clock bus recovery. Rev B is evaluating removing
the part from the BOM in favour of a BQ25622 VBAT ADC + coulomb counter
inside `charger_task`.

Rather than port `bringup_gauge.c` to a driver we might delete, M3.4.4
ships an API-only stub under `firmware/core1/src/power/bq27441.{h,c}`:

- `bq27441_start_task(priority)` — no-op, returns true.
- `bq27441_get_state()` — returns a pointer to a static
  `{ .online = false, ... }` snapshot. UI / battery-monitor code can
  bind against this and light up automatically once Rev B's decision
  lands and the real driver fills the struct.

No I2C traffic is generated and no FreeRTOS task is created. The stub
is `#include`d nowhere outside the power module; it only has to
compile and link. Bridge regression test passes — `python -m
meshtastic --port COM16 --info` returns `nodedbCount: 11`.

**Files added:**
- `firmware/core1/src/power/bq27441.{h,c}`

**Files changed:**
- `firmware/core1/m1_bridge/CMakeLists.txt` — new `bq27441.c` source

#### M3.4.5a — LPS22HH barometer driver + sensor-bus baudrate fix ✅ (2026-04-18)

First Core 1 driver on the sensor bus (GPIO 34/35, time-muxed i2c1).
Exposes `lps22hh_init()` / `lps22hh_poll()` / `lps22hh_get_state()`; a
new `sensor_task` owns the 10 Hz master tick and calls LPS22HH once per
second. LIS2MDL (M3.4.5b) and LSM6DSV16X (M3.4.5c) slot into the same
tick with divider counters.

**Production config** (DS DocID030890 §9.6/§9.7):
- ODR = 1 Hz, LPF ODR/9, BDU=1, LOW_NOISE_EN=1, IF_ADD_INC=1 (POR)
- Address = 0x5D (SA0=high, Rev A Issue #4)

**State struct** reports `pressure_hpa_x100`, `temperature_cx10`,
`online`, `i2c_fail_count`. Decoder sign-extends 24-bit pressure, uses
`(raw × 25) / 1024` to stay in int32 for the 260..1260 hPa range; temp
is `raw / 10`.

**Issue P2-14 — sensor-bus-wide I2C NACK on Core 1** uncovered during
bring-up. Root cause: `i2c_set_baudrate_core1()` only configured SS
counters and SPEED=STANDARD; FS counters and `sda_hold` were left at
whatever `i2c_init()` wrote based on `clock_get_hz(clk_peri)=0`
(garbage). Power bus electrical margins were lucky enough to tolerate
the broken timing, but the sensor bus's four devices all NACKed from
cold. Fix rewrites the override to mirror the SDK exactly (populate FS
**and** SS counters, set proper `sda_hold`, pick SPEED per baudrate),
runs a full `i2c_init()` + baudrate re-apply on every pinmux switch,
and lifts the default baud to 400 kHz matching the bringup firmware.

**Validation (2026-04-18):**
- I2C scan both buses — power finds 0x36 (LM27965) + 0x6B (BQ25622);
  sensor finds 0x1E (LIS2MDL) + 0x3A (Teseo-LIV3FL) + 0x5D (LPS22HH) +
  0x6A (LSM6DSV16X). First time sensor bus has ever ACKed under
  Core 1 firmware.
- LPS22HH SWD read: pressure 1008.17 hPa, temp 31.9 °C, online=true,
  fail_count=0.
- `python -m meshtastic --port COM16 --info` returns
  `nodedbCount: 11` (bridge unaffected).

**Files added:**
- `firmware/core1/src/sensor/lps22hh.{h,c}`
- `firmware/core1/src/sensor/sensor_task.{h,c}`

**Files changed:**
- `firmware/core1/src/i2c/i2c_bus.c` — baudrate override rewrite;
  full re-init on pinmux switch; default baud 100 kHz → 400 kHz.
- `firmware/core1/src/i2c/i2c_bus.h` — corrected pull-up rail comment
  (both buses are 1.8 V, not mixed 1.8/3.3 V).
- `firmware/core1/m1_bridge/CMakeLists.txt` — new SENSOR_DIR + sources.
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` —
  `sensor_task_start()` at scheduler launch.

#### M3.4.5b — LIS2MDL magnetometer driver ✅ (2026-04-18)

Second sensor-bus driver, slotted into `sensor_task` at 10 Hz (one poll
per master tick) to match the device's lowest ODR. Same shape as LPS22HH.

**Production config** (DS12144 Rev 6 §5.3/§8.5-§8.7):
- ODR = 10 Hz, MD = continuous, LP = 0 (high-resolution), COMP_TEMP_EN = 1
- OFF_CANC = 1, LPF = 1 (ODR/4), BDU = 1
- Address = 0x1E (§6.1.1 Table 20)

**State struct** reports `mag_raw[3]`, `mag_ut_x10[3]`, `temperature_cx10`,
`online`, `i2c_fail_count`. Decoder uses `raw × 3 / 2` for µT×10 (1.5
mgauss/LSB). Temperature follows ST HAL convention: zero point = 25 °C
so `cx10 = 250 + (raw × 5) / 4`. Datasheet doesn't document this offset
explicitly — confirmed via ST's official `lis2mdl_from_lsb_to_celsius`
(`raw/8 + 25`).

**I²C auto-increment wrap — `TEMP_OUT` must be a separate transaction.**
Initial implementation did a single 8-byte burst from `0x68` covering
X/Y/Z + TEMP. Mag values looked correct but temperature reported 11 °C
at 25 °C ambient. Debug patch added a parallel 2-byte read of `0x6E`:
direct read returned +28 (≈28 °C, matches), burst bytes 6-7 returned
the same value as OUTX_L/H. Conclusion: LIS2MDL's address auto-increment
wraps at the end of the mag OUT block (`0x6D → 0x68`) instead of
crossing into `TEMP_OUT`, even with `SUB[7]` set. Driver now reads mag
(0x68, 6 bytes) and temp (0x6E, 2 bytes) as two back-to-back
transactions on a single bus acquire. Datasheet §6.1.1 doesn't document
this wrap — discovered empirically.

**Soft-reset.** Datasheet doesn't guarantee `SOFT_RST` is self-clearing
(app-note §5.3 skips reset entirely), so driver writes `CFG_REG_A = 0x20`
then waits a fixed 10 ms instead of polling.

**Temperature zero-point.** ST's official HAL `lis2mdl_from_lsb_to_celsius`
is `raw/8 + 25`; datasheet Table 3 only lists sensitivity. Driver matches
ST HAL: `cx10 = 250 + (raw × 5) / 4`.

**Validation (2026-04-18):**
- SWD read `s_state` at `0x20053580`: `online=1`, `fail_count=0`,
  `mag_ut_x10 ≈ (-16.6, -34.0, +147.6) µT`, scale check
  `raw × 3/2 == mag_ut_x10` on all three axes, values vary when the
  board is moved.
- `temperature_cx10 = 283` → **28.3 °C** at 25 °C ambient (PCB self-heating
  accounts for the few-°C offset; consistent with bringup LPS22HH temp
  of 31.9 °C on the same bus).
- `python -m meshtastic --port COM16 --info` returns normal node info
  (bridge + sensor_task coexist cleanly).

**Files added:**
- `firmware/core1/src/sensor/lis2mdl.{h,c}`

**Files changed:**
- `firmware/core1/src/sensor/sensor_task.c` — init LIS2MDL, poll every
  master tick (`LIS2MDL_PERIOD_TICKS = 1`).
- `firmware/core1/m1_bridge/CMakeLists.txt` — add `lis2mdl.c`.

#### M3.4.5c — LSM6DSV16X IMU driver ✅ (2026-04-18)

Third sensor-bus driver. 6-axis (3× accel + 3× gyro) + internal temp, 10 Hz
poll from `sensor_task` with the device running 30 Hz high-performance so
BDU always has a fresh sample between reads.

**Production config** (DS13510 Rev 4 §9.14-§9.21):
- `CTRL1 = 0x04` — ODR_XL = 30 Hz (Table 52), OP_MODE_XL = high-performance
- `CTRL2 = 0x04` — ODR_G  = 30 Hz (Table 55), OP_MODE_G  = high-performance
- `CTRL3 = 0x44` — BDU = 1, IF_INC = 1 (matches post-reset default)
- `CTRL6 = 0x01` — FS_G = ±250 dps (8.75 mdps/LSB)
- `CTRL8 = 0x00` — FS_XL = ±2 g (0.061 mg/LSB)
- Address = 0x6A; WHO_AM_I = 0x70 (§9.13)

**Single 14-byte burst.** Unlike LIS2MDL, auto-increment traverses
`OUT_TEMP_L` → gyro → accel cleanly, so one read of 0x20..0x2D captures
T + G + A atomically under BDU.

**Decoders:**
- accel_mg = `raw × 61 / 1000` (fits int16_t at ±2000 mg)
- gyro_dps_x10 = `raw × 875 / 10000` (fits int16_t at ±2500)
- temperature_cx10 = `250 + raw × 5 / 128` (256 LSB/°C, zero = 25 °C,
  per ST HAL `lsm6dsv16x_from_lsb_to_celsius_t`)

**Soft-reset.** `CTRL3 |= SW_RESET(0x01)`, then poll bit 0 every 1 ms up
to 50 ms (datasheet §9.16 states self-clearing).

**Validation (2026-04-18):**
- SWD read `s_state` at `0x20053598`: `online=1`, `fail_count=0`.
- `accel_mg ≈ (+54, +17, -993)` mg — ≈ 1 g on Z (board face-down),
  X/Y within ±60 mg noise.
- `gyro_dps_x10 ≈ (-5, +1, +3)` → ≈ (-0.5, +0.1, +0.3) dps static bias,
  well under the ±50 tolerance.
- `temperature_cx10 = 322` → **32.2 °C**, consistent with LPS22HH 32.4 °C
  and LIS2MDL 28.3 °C on the same bus.
- `python -m meshtastic --port COM16 --info` returns normal node info.

**Spec correction.** Plan handed in `CTRL1 = CTRL2 = 0x03` for 30 Hz, but
Table 52/55 encode 30 Hz as ODR=0100 → value 0x04. Driver uses 0x04;
the 0x03 value would have programmed 15 Hz.

**Files added:**
- `firmware/core1/src/sensor/lsm6dsv16x.{h,c}`

**Files changed:**
- `firmware/core1/src/sensor/sensor_task.c` — init LSM6DSV16X, poll
  every master tick (`LSM6DSV16X_PERIOD_TICKS = 1`).
- `firmware/core1/m1_bridge/CMakeLists.txt` — add `lsm6dsv16x.c`.

#### M3.4.5d — Teseo-LIV3FL GNSS driver (Part A) ✅ (2026-04-18)

First Core 1 driver that doesn't fit the `sensor_task` mold — GNSS is a
streaming NMEA protocol, so it lives in its own `gps_task` (priority
tskIDLE+2, 2 KB stack) running a 100 ms I2C drain. See software
requirements §108 for the full contract.

**Shipped:**
- `teseo_liv3fl_{h,c}` — full driver with NMEA checksum verify, GGA +
  RMC + GSV parsing (GSA / others ignored per design). Parsed snapshots
  in `teseo_state_t` (lat/lon ×1e7 + UTC + speed/course + HDOP + fix
  quality) and `teseo_sat_view_t` (up to 32 pooled sats PRN/el/az/SNR).
- `gps_task.{h,c}` — 100 ms drain cadence; `gps_task_start()` wired
  into `main_core1_bridge.c` at tskIDLE+2.
- `teseo_set_fix_rate(GNSS_RATE_OFF)` — `$PSTMGPSSUSPEND`, verified via
  SWD (sentence_count freezes after call).
- `teseo_set_fix_rate(GNSS_RATE_1HZ)` — default, verified working.

**Known issue — dynamic fix rate > 1 Hz not yet effective (deferred).**
Command bytes are correct (`$PSTMSETPAR,1303,0.1*<cs>\r\n` captured
verbatim at TX, write ACKs), but the Teseo engine continues to output at
1 Hz. `$PSTMGPSRESTART` after SETPAR does not help; SUSPEND works as a
sanity check that the write path is fine. Driver exposes the setter so
application code can be written against the final API; future work:
reverse-engineer which NVM bit / CDB entry gates runtime ODR changes on
LIV3FL (candidates: CDB 201/228 message-list rate scaler, or the rate
may require `$PSTMSAVEPAR` + `$PSTMSRR` to take effect — avoided here to
preserve NVM write budget). Raise as a new issue when commissioning
reaches the "satellite radar UI" milestone that actually needs 10 Hz.

**Heap hit — +16 KB to `configTOTAL_HEAP_SIZE` (32→48 KB).** Adding a
2 KB task pushed IDLE-task allocation in `vTaskStartScheduler` past the
32 KB limit; caught by a FreeRTOS `configASSERT` at tasks.c:3790 → panic
→ `_exit` bkpt HardFault with IPSR=3. Confirmed via CFSR/HFSR, MSP
exception frame (stacked PC = `_exit`, format string = "FreeRTOS assert
%s:%d" in `tasks.c`). Future task additions on Core 1 should budget
against 48 KB, not 32.

**Validation (2026-04-18):**
- SWD read `s_state` after ~12 s indoor: `online=1`, `fix_valid=0`,
  `fix_quality=0`, `hdop_x10=990` (99.0 = "no-fix" sentinel), `lat/lon=0`
  (gated by `fix_quality`), `utc_time=103418` (10:34 UTC matches local
  TPE 18:34), `sentence_count=32` (~2.7 Hz of GGA+RMC, matches 1 Hz base
  rate × 2 sentences + some clock slop), `i2c_fail_count=0`.
- `teseo_get_sat_view()` returns `count=0` indoor (no completed GSV
  cycles without fix) — parser runs but can't populate anything.
- `$PSTMGPSSUSPEND` round-trip: sentence_count delta over 5 s drops from
  ~10 to 0 when rate set to OFF.
- `python -m meshtastic --port COM16 --info` returns normal node info.

**Files added:**
- `firmware/core1/src/sensor/teseo_liv3fl.{h,c}`
- `firmware/core1/src/sensor/gps_task.{h,c}`

**Files changed:**
- `firmware/core1/m1_bridge/CMakeLists.txt` — add `teseo_liv3fl.c` +
  `gps_task.c`.
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` — spawn
  `gps_task_start()` after sensor_task.
- `firmware/core1/m1_bridge/src/FreeRTOSConfig.h` —
  `configTOTAL_HEAP_SIZE` 32 KB → 48 KB.
- `docs/requirements/software-requirements.md` §108 — rewrite around the
  shipped contract (always-on NMEA mask, runtime-adjustable ODR,
  caller-owned rate policy).

#### M3.4.5d Part B — GNSS dynamic fix rate resolved ✅ (2026-04-18)

Followed up on the deferred "SETPAR doesn't take effect" finding.

**Mechanism found:** Teseo caches CDB 303 (GNSS FIX Rate) at engine init
and does not re-read Current configuration while running. The working
sequence is `$PSTMSETPAR,1303,<period>` + `$PSTMSAVEPAR` + `$PSTMSRR` —
persist the new period to NVM and force the engine to reload via
software reset. NVM endurance is 10k cycles and runtime rate changes
happen at user cadence (mounting the satellite UI, toggling power
modes), so wear is not a concern.

**Shipped:**
- `send_await(body, ok_kind, err_kind, timeout_ms)` — generic helper
  that emits a `$PSTM...` command and blocks until `$PSTMxxxOK`,
  `$PSTMxxxERROR`, or timeout is observed via the normal NMEA drain +
  dispatcher. Reusable for any future ST proprietary command sequence.
- `dispatch_pstm()` — matches `$PSTMSETPAROK`, `$PSTMSETPARERROR`,
  `$PSTMSAVEPAROK`, `$PSTMSAVEPARERROR`, and the `$PSTMSETPAR,...`
  synthetic reply to `$PSTMGETPAR`. Longest-prefix match order so
  `ERROR` doesn't shadow `OK`.
- `teseo_set_fix_rate(gnss_rate_t)` — now does the 3-step sequence, with
  `$PSTMGPSRESTART` wake from OFF preserved. NOT called automatically
  on boot (no NVM wear from startup); only fires when an application
  layer explicitly requests a rate change.
- `teseo_get_fix_rate_from_device()` — diagnostic public API, sends
  `$PSTMGETPAR,11303` and captures the reported period. Reply parsing
  ready but did not land during the validation run — cause unclear (may
  be that Teseo routes GETPAR replies only to UART); kept as public
  API because the dispatch plumbing is already there.
- `DRAIN_BUF_SIZE` 256 → 1024 B so higher NMEA output rates don't
  overflow the Teseo TX buffer on the 100 ms poll cadence.

**Empirical rate ceiling.** The driver honors requested rates up to an
effective ~3 Hz on MokyaLora Rev A — DS13881 Table 1 lists 10 Hz max
fix rate for dual-constellation configurations, and bringup enabled all
four (GPS + GLONASS + Galileo + BeiDou) in NVM. With that constellation
mix, 5 Hz and 10 Hz requests both land as ~3 Hz NMEA output (6
sentences/s for GGA+RMC), while 1 Hz and 2 Hz requests are honored
proportionally (2/s and 4/s respectively). Not a driver bug; if a UI
genuinely needs 10 Hz it should drop BeiDou via `$PSTMSETPAR,1200,...`
first.

**Validation (2026-04-18, indoor no fix):**
- `s_test_set_rc = 1` (SETPAR OK + SAVEPAR OK + SRR sent)
- `s_last_resp_count = 2` confirms both reply dispatch paths ran
- Rate sweeps (steady state, GGA+RMC per-second counts):
  - 1 Hz request → 2/s ✓
  - 2 Hz request → 4/s ✓
  - 5 Hz request → 6/s (clamp)
  - 10 Hz request → 6/s (clamp)
- Boot-time rate measurement after cleanup: 4/s (NVM still carries 2 Hz
  from last validation write; driver never overwrites on boot). Proves
  the NVM path persists across resets.
- `python -m meshtastic --port COM16 --info` returns normal node info.

**Files changed:**
- `firmware/core1/src/sensor/teseo_liv3fl.{h,c}` — reply dispatch,
  send_await, SETPAR+SAVEPAR+SRR sequence, GETPAR diagnostic API,
  1 KB drain buffer.

#### M3.4.5d Part C — RF debug plumbing + UI ✅ (2026-04-18)

Expose Teseo's ST-proprietary RF diagnostics (`$PSTMRF`, `$PSTMNOISE`,
`$PSTMNOTCHSTATUS`, `$PSTMCPU`, `$GPGST`) as a first-class driver
snapshot with an LVGL view so commissioning / troubleshooting have
structured telemetry instead of USB stdout greps.

**Driver additions (`teseo_liv3fl.{h,c}`):**
- `teseo_rf_state_t` — pooled snapshot: noise floor (GPS+GLN), ANF
  status per path (freq, power, lock, mode, ovfs/jammer flag), CPU
  usage + clock, up to 32-sat RF detail (PRN / C/N0 / freq / phase
  noise). Accumulator for multi-sentence `$PSTMRF` mirrors the GSV
  pattern.
- Parsers in `dispatch_pstm()` for each of the four `$PSTM` lines plus
  safe ignore for anything else.
- `teseo_enable_rf_debug_messages(bool on)` — OR-/AND-NOT-patches the
  CDB 231 mask (`0x408000A8` = GST + NOISE + RF + CPU + NOTCHSTATUS)
  via the existing `SETPAR + SAVEPAR + SRR` utility. RAM-only SETPAR
  with `mode=1` (OR) / `mode=2` (AND-NOT); NVM persisted once, so no
  per-boot writes.

**UI (`src/ui/rf_debug_view.{h,c}`):**
- Landscape 320x240 layout: title, fix status, noise floor, CPU, ANF
  GPS + GLN (freq / lock / mode / jammer flag), sat table top-9 by
  C/N0. Shows "— not received —" per row until its respective counter
  increments, plus a bottom hint pointing at the enable API when
  nothing has arrived at all.
- Selected via compile-time `MOKYA_BOOT_VIEW_RF_DEBUG` in `lvgl_glue.c`
  (defaults to 0 = keypad_view). Proper runtime view switching belongs
  to a later milestone that also adds FUNC-key navigation.

**Commissioning:** ran once with `MOKYA_COMMISSION_RF_DEBUG` set in
`gps_task.c` — on boot, calls `teseo_enable_rf_debug_messages(true)`;
SETPAR/SAVEPAR OK, SRR fires, NVM committed. Flag immediately
re-disabled for normal builds; subsequent flashes confirmed the
diagnostic stream still arrives without any further writes.

**Validation (2026-04-18 indoor):**
- `s_rf_state` readback after commissioning:
  - `noise_gps = noise_gln = 12500` (raw, both paths alive)
  - `anf_gps`: freq ≈ 5.04 MHz, mode = AUTO, unlocked
  - `anf_gln`: freq ≈ 8.18 MHz, mode = AUTO, **locked, ovfs = 1000
    (jammer bit set — GLONASS notch is actively rejecting RFI)**
  - `cpu_pct_x10 = 226` (22.6 %), `cpu_mhz = 98`
  - `rf_update_count`, `noise_count`, `anf_count`, `cpu_count` all
    incrementing at ~1 Hz
- Post-reboot (commission flag removed) RF data still flows — NVM
  persistence confirmed.
- `meshtastic --info` regression clean.

Followup ideas: (1) add UI-triggered enable button so user can flip the
mask without a commissioning flash; (2) decode `cpu_mhz = 98` — not one
of the datasheet-listed values (52/104/156/208); Teseo may be reporting
actual rather than nominal clock.

**Files added:**
- `firmware/core1/src/ui/rf_debug_view.{h,c}`

**Files changed:**
- `firmware/core1/src/sensor/teseo_liv3fl.{h,c}` — RF state + parsers
  + enable API.
- `firmware/core1/src/sensor/gps_task.c` — optional commission hook
  (disabled by default).
- `firmware/core1/src/display/lvgl_glue.c` — boot-view selector.
- `firmware/core1/m1_bridge/CMakeLists.txt` — add `rf_debug_view.c`.
- `docs/design-notes/core1-memory-budget.md` — `s_rf_state` row.

#### M3.4.5d follow-up — Core 1 memory budget hygiene ✅ (2026-04-18)

Reactive heap bumps across M3.4.1 through M3.4.5d (32 KB → 48 KB via
multiple "HardFault at `vTaskStartScheduler`" sessions) kept wasting
debug time. Converted to an upfront budget.

**Doc:** `docs/design-notes/core1-memory-budget.md` — lists every task,
stack depth, kernel object, reserve target (≥ 20 %). Required reading /
editing when adding any new task or queue.

**Enforcement in `main_core1_bridge.c`:**
- `TASK_START_OR_PANIC` macro wraps every `xTaskCreate` and every
  `*_task_start` helper. No more `(void)rc_xxx` silent casts — a
  failure prints `core1: <name> task_start failed` on USB CDC.
- After all tasks created, checks `xPortGetFreeHeapSize() ≥ 20 %` of
  `configTOTAL_HEAP_SIZE`; panics with used / total figures if below.
  Catches drift before the next IDLE-task allocation blows up inside
  scheduler startup.

**Validation (2026-04-18):**
- Build + flash clean; IPSR = PendSV (normal), meshtastic `--info`
  passes.
- SWD read of heap_4 free-list: `xFreeBytesRemaining = 15,008 B`
  (14.65 KB / 30.5 %). Matches the estimate in core1-memory-budget.md
  §3.4 to within 1 % — budget table is trustworthy.

**Files changed:**
- `docs/design-notes/core1-memory-budget.md` — new.
- `firmware/core1/m1_bridge/src/main_core1_bridge.c` — macro + heap
  reserve assert.
- `CLAUDE.md` — pointer to the new doc.

#### M3.6 — MIEF font driver + Traditional Chinese LVGL rendering ✅ (2026-04-19)

LVGL labels can now render Traditional Chinese, Bopomofo, Latin-1
Supplement, and CJK punctuation from the MIEF v1 font blob compiled
against GNU Unifont 17.0.04.

**Deliverables:**
- `firmware/core1/src/display/mie_font.{c,h}` — `lv_font_t` adapter
  that parses the MIEF header, binary-searches the codepoint index,
  and unpacks 1 bpp MSB-first bitmaps to A8 into the `lv_draw_buf_t`
  LVGL hands to `get_glyph_bitmap_cb`. Zero runtime heap. Two
  public views share the same blob:
  `mie_font_unifont_sm_16()` (16 px native, 1:1) and
  `mie_font_unifont_sm_32()` (32 px via 2× nearest-neighbor — each
  source bit fills a 2×2 A8 block at draw time, so the 32 px variant
  costs no extra flash and ~4× per-glyph unpack work). The `lv_font_t`
  dsc holds a `mief_view_t { blob*, scale }` so a single callback
  body serves both scales.
- `firmware/core1/src/display/mie_font_blob.S` — `.incbin` stub that
  embeds `mie_unifont_sm_16.bin` into `.rodata` with
  `_mie_unifont_sm_16_{start,end,size}` symbols. Path passed via
  `MIEF_BLOB_PATH` per-source compile definition.
- `firmware/mie/tools/gen_font.py` — Latin-1 Supplement
  (0x00A0..0x00FF) added to `MANDATORY_RANGES` so `°C × ÷ ± ©` etc
  always ship regardless of charlist content. Also:
  `fontTools` import case fix (previously lowercase), `cp950`
  console-encoding fix (removed `©` from license notice so Windows
  default console doesn't raise `UnicodeEncodeError`).
- `firmware/core1/m1_bridge/CMakeLists.txt` — `add_custom_command`
  regenerates the MIEF blob whenever `gen_font.py`, `charlist.txt`,
  or the source OTF changes; `MIEF_BLOB_PATH` propagated to the
  `.incbin` assembler via `set_source_files_properties`. Note:
  `--no-subset` is used because fontTools' OS/2 unicode-range
  writer rejects Unifont's bit-123 range as
  `expected 0 <= int <= 122`.
- `firmware/core1/src/ui/font_test_view.{c,h}` — a third view (panel
  index 2). Renders the opening two paragraphs of 《桃花源記》
  (陶淵明) with the 32 px title at the top and the 16 px body
  wrap-laid-out underneath (4 px `text_line_space` — Unifont glyphs
  fill most of the 16 px cell so line_space=0 bleeds adjacent rows
  together). Mixed-scale rendering confirms both font views can be
  active on the same screen without interfering. FUNC cycles
  keypad → rf_debug → font_test.

**Footprint:**
- MIEF blob: 851 061 bytes (831 KB), 19 320 glyphs.
- Core 1 `.bin`: 440 KB → 1.29 MB (~65 % of the 2 MB flash window).
- No RAM cost at runtime — glyphs are binary-searched and unpacked
  directly from flash-resident bytes; the only per-call allocation
  is the LVGL-owned draw buffer (already sized for A8 glyphs).

**Verification.** `build_and_flash.sh --core1`, visual inspection of
all seven `font_test_view` rows. Latin-1 row rendered as tofu on the
first pass (root cause: `MANDATORY_RANGES` omitted 0x00A0..0x00FF);
fixed and re-flashed successfully on the second pass.

**Follow-ups not in M3.6 scope:**
- Full font variant (all CODEPOINT_RANGES) — deferred until a real
  user flow demands it; the charlist-subset already covers all MoE
  dict coverage + mandatory Unicode ranges.
- RLE encoding path (`mie_font.c` currently rejects `flags & 0x01`).
  No user demand; uncompressed random-access lookup is simpler and
  already fits the flash budget.
- `lv_font_t::fallback` chain — the `mie_font` is a superset of
  what Montserrat 14 covers for ASCII, so there's no fallback need
  for now. Revisit if we ever ship an emoji or symbol-extension
  font alongside.
- Boot-time MIEF header corruption check surfaced via SWD
  breadcrumb — deferred to when a second font blob gets added.

#### MIE Phase 1.4 — IME UX completion (Tasks A / B / C) ✅ (2026-04-24)

Three input-UX gaps closed on top of the MIE v4 composition engine.

**Task A — Long-press multitap for primary ↔ secondary phoneme**
(commit `df226da`).

Two-tier slot-key UX replacing v2's half-keyboard-only model:

| operation | engine hint | display |
|---|---|---|
| short tap       | `ANY` (fuzzy)                 | compound (ㄅㄉ) |
| 1st long press  | primary                       | single (ㄅ)       |
| 2nd long press  | secondary                     | single (ㄉ)       |
| 3rd long press  | tertiary (slot 4 only — ㄦ)   | single (ㄦ)       |
| Nth long press  | cycles modulo phoneme_count   | –                 |

Cycle window is `kMultiTapTimeoutMs = 800 ms`; any short tap / DEL /
MODE / different-slot press locks the cycle.

Implementation surface:
- **Dict format**: header.flags bit 0 marks per-reading phoneme_pos
  byte (packed 2-bit × klen). +26 KB on the 2.65 MB dict (+1 %).
- **Searcher**: `CompositionSearcher::search(user_keys, user_phoneme_hints,
  user_n, ...)` overload — each byte's hint in {0, 1, 2, 0xFF = any}
  filters readings whose authored phoneme position doesn't match.
- **Engine**: `ImeLogic.phoneme_hint_[kMaxKeySeq]` parallel array +
  `lp_cycle_` FSM (byte_index / slot / phoneme_idx / last_ms).
- **Wire**: `MOKYA_KEY_FLAG_LONG_PRESS` bit in `KeyEvent.flags`;
  inject ring upgraded to 2-byte events (key_byte, flags_byte),
  magic `KEYI` → `KEYJ`.
- **Keypad scan**: 20 slot keys defer press emission; short tap fires
  press(flags=0) on release, ≥ 500 ms hold fires press(LONG_PRESS)
  at the threshold.

**Task B — SYM1 long-press symbol picker** (commit `f3cc048`).

4×4 grid of common Traditional-Chinese punctuation the half-keyboard
can't type:

    「」『』
    （）【】
    ，。、；
    ：？！…

UX:
- Short-tap SYM1   → commit ，/, (unchanged).
- Long-press SYM1  → open picker overlay (already wired in tick()).
- DPAD in picker   → navigate (LR ±1, UD ±cols, mod cell_count).
- OK in picker     → commit selected symbol, close picker.
- SYM1 in picker   → close without commit.
- Any other key    → close without commit; next press hits normal
                     routing.

View reuses the candidate flex-wrap renderer; data source swaps to
picker cells when `ime_view_picker_active()`.

**Task C — Space + Newline as first-class commits** (commit `7878f8c`).

Unified "idle-commits-literal-char" convention on SPACE and OK:

| short-tap | has candidates | multi-tap pending | picker open | idle |
|---|---|---|---|---|
| SPACE | `0x20` tone-1 (ZH) / commit word (EN) / " " (Direct) | commits it | — | commits " " |
| OK    | commits selected candidate | commits it | commits symbol | emits "\n" |

Both keys now work without a long-press hold — the existing "finish
pending first, otherwise insert a literal" pattern extends from SPACE
onto OK.

**Validation.** 150 / 150 host unit tests pass (21 new: 7 for
phoneme-pos searcher, 7 for long-press cycling + picker, 3 for
idle-OK newline, 4 for view wiring). Hardware SWD regression across
six passages totalling 1 416 CJK + 130 ASCII + 48 spaces + 21
newlines + 136 picker punctuation = **1 751 characters, 0 failures**:

| passage | CJK | top-8 | top-100 | picker | newline | space | ks/char |
|---|---|---|---|---|---|---|---|
| Echeneis intro | 243 | 93.8 % | 100 % | – | – | – | 7.49 |
| 生態習性    | 217 | 96.8 % | 100 % | 24/24 | 5/5 | 3/3 | 6.72 |
| 台灣生活   | 258 | 98.1 % | 100 % | 27/27 | 4/4 | – | 6.06 |
| 硬體碎念    | 264 | 98.9 % | 100 % | 23/23 | 4/4 | 16/16 | 6.46 |
| 終極難度    | 252 | 93.7 % | 100 % | 31/31 | 4/4 | 15/15 | 8.06 |
| 文言文       | 182 | 87.9 % | 100 % | 31/31 | 4/4 | 14/14 | 10.01 |
| **total**   | **1 416** | **94.6 %** | **100 %** | **136/136** | **21/21** | **48/48** | **7.16** |

**P1.5 dropped (2026-04-24).** The original follow-up list included a
Phase 1.5 for Unihan kHanyuPinlu + MoE 成語典 + CC-CEDICT compound
import (~50 KB of dict growth). Post-P1.4 analysis shows:

- No passage miss → demand for Unihan-only chars is vanishing.
- Mid-range buckets (ㄧˋ = 215, ㄒㄧ¹ = 158 …) already saturate the
  100-cap; enlarging them hurts UX for everyone.
- 563 chars / 3 % of current dict already have *every* reading at
  rank > 100 — a pruning candidate, not an expansion one.

The real remaining pain (repeat-typer of "吸" hits rank 21 four times
in one passage) is solved by **Phase 1.6 — personalised LRU cache**
(~6 KB RAM + ~6 KB LittleFS; no dict bloat). See
[mie-v4-status.md](mie-v4-status.md) "Phase 1.6 plan" for the spec.

### P1.6 — Personalised LRU cache delivered (2026-04-23)

Engine, flash persistence, and symmetric P2-11 park all landed in
three commits on `dev-Sblzm`:

- `9d05ae1` LruCache module + 16 host tests (kCap 64, 48 B per entry).
- `3c4c6c8` Wire into `ImeLogic::run_search_v4` + `commit_partial`;
  5 integration tests (171/171 total suite).
- `c4dd34c` Core 1 flash partition at `0x10C00000` (64 KB reserved,
  8 KB slot), symmetric `--wrap=flash_range_*` on Core 1 with a
  mirror park listener in the Meshtastic variant dir; throttled
  save on 50 commits / MODE / 30 s idle.

Hardware verification (SWD-only, J-Link Commander):
- Core 0 initVariant reaches phase 0x16 (`c0_ready` published). Core 1
  boots cleanly, USB CDC enumerates, `ime_task_start` succeeds with
  lru_persist pre-allocated in BSS.
- Injecting a MODE key via `g_key_inject_buf` fires the
  `mode_tripwire` → `lru_persist_save` → `flash_range_erase` +
  `flash_range_program`. Flash at `0x10C00000` reads `0x3155524C`
  ("LRU1") + version 1 + 0xFF tail immediately after. Survives SWD
  reset without re-flashing the partition.
- `0x2007FFE0` debug breadcrumbs (now removed) confirmed the
  throttle loop fires exactly once per MODE cycle and returns ok.

Hardware regression ran 2026-04-23 on `user1.txt` (first 30 chars,
`--erase --limit 30`):

    Pass 1 (cold cache): rank 0 = 5 / rank ≤ 3 = 16 / rank ≥ 8 = 1
    Pass 2 (warm cache): rank 0 = 22 / rank ≤ 3 = 24 / rank ≥ 8 = 0
    Δ rank 0 = +17, Δ rank ≤ 3 = +8, Δ rank ≥ 8 = −1

The +17 bump at rank 0 is the LRU promoting the 17 chars that were
committed on Pass 1. All 24 CJK chars in Pass 2 are visible in the
top-3 candidate row without any DPAD navigation — exactly the
"repeat-typer cost" win Phase 1.6 was built to deliver.

Three items deferred to a follow-up session:
1. `--reboot` path (flash-persistence across a SWD reset) is scripted
   but hit a Core 1-stays-in-bootrom edge case after repeated
   erase/reset cycles in the same session. The flash contents were
   verified with `mem32 0x10C00000 4` → `0x3155524C` ("LRU1") after
   a save; the load path ran on boot and returned success during
   normal (non-erase) testing. Needs a clean session to reproduce
   the failure shape and fix.
2. Full five-passage regression run across user{1..5}.txt +
   echeneis.txt. Script is ready; awaiting a session that isn't
   already deep in debugging churn.
3. `mie_dict_blob` target regenerates a corrupt 89 MB `dict.bin`
   from the local `tsi.csv`; ship-path uses the pre-built
   `dict_mie_v4.bin` via `--v4` which is unaffected.

Notable bugs hit during implementation:
- `LruCache::kCap = 128` initially overflowed Core 1 RAM by 4280 B
  (.bss push from the added 6 KB `LruCache` inside `ImeLogic`). Halved
  `kCap` to 64 and shrank the flash-write scratch accordingly.
- First lazy `pvPortMalloc(6400)` for the save-path scratch **never
  succeeded at throttle time** — FreeRTOS heap was already fragmented
  by the time the first save fired hours into runtime, even with
  ~14 KB free in total. Switched to a static .bss scratch allocated
  at image load.
- `MOKYA_KEY_A` is slot 10 (ㄇㄋ), not 14 (ㄠㄤ) — Step 2 integration
  tests initially failed because I assumed the key-map ordering
  matched the keypad matrix order. Fixed by routing ㄠ through
  `MOKYA_KEY_L`.
- Core 0 and Core 1 are BOTH editable as the Meshtastic submodule —
  remember to commit inside the submodule before the super-project
  records the bump. Step 3 touches the Meshtastic fork for the
  listener `flash_park_listener.c`.

### P1.6 follow-up — `--reboot`, full regression, dict regen, RTT transport (2026-04-23 / 24)

Closes all three deferred items and adds a second key-inject
transport. Commits on `dev-Sblzm`:

- `75fc9a4` / `3af07f6` — `--reboot` round-trip wedge root-caused
  and fixed. `force_lru_save()` was overwriting `producer_idx = 2`,
  which — with Pass 1's consumer_idx already at ~30 — made the
  consumer race the full ring re-processing stale events, crashing
  Core 1 mid-`flash_range_program` and leaving Core 0 in the park
  spin with XIP off (the QMI-wedge pattern). Now writes events at
  `(cur_prod & (RING-1))` and bumps `producer_idx += 2`. Hardware
  verified: `--erase --reboot --limit 30` → Pass 1 rank-0 = 5,
  Pass 2 rank-0 = 23 (post-reset, cache loaded from flash).
- `3af07f6` — six-passage regression numbers captured. See
  `mie-v4-status.md` Phase 1.6 delivered table. Short passages
  (user1-30 / echeneis) +18 / +23 rank-0; long passages
  (user2/4/5) essentially flat — documented as P1.6.1 territory
  in `docs/design-notes/mie-p1.6-lru-plan.md`.
- `f63f41b` — `mie_dict_blob` 89 MB regeneration root-caused.
  `mie_dict_data_lg` was sharing `${MIE_DATA_DIR}` with
  `mie_dict_data_sm` and clobbering the filtered `dict_dat.bin`
  (1.9 MB) / `dict_values.bin` (3 MB) with unfiltered 38 MB /
  51 MB outputs. Re-routed `lg/` to its own subdir; regression
  `firmware/mie/tools/test_pack_dict_blob.py` asserts header size
  fields match input file sizes so a future overwrite surfaces
  immediately.
- `7e2443a` / `862ef72` / `1a21ee9` — **RTT key-inject transport**
  as an alternate to the SWD ring. Host side: `MokyaSwd.rtt_send_frame`
  writes directly into the SEGGER RTT control block (pylink's own
  `rtt_write` proved unreliable on RP2350 + J-Link V932, silently
  drops every write after the first in a session). Firmware:
  `key_inject_rtt.{c,h}` + shared `key_inject_frame.h` wire frame
  (magic + type + len + payload + crc8). Both transports coexist
  under a shared `g_key_inject_mode` byte — active one runs at
  poll cadence, inactive long-sleeps 50 ms so ime_task never has
  to compete with two hot pollers. `ime_text_test.py
  --transport {swd,rtt}` picks.
- `eb182a3` — script poll refactor. Replaced `while time < deadline:
  read_snapshot(400 B); if cond: break; sleep(0.01)` with
  `wait_until(cheap_u32_read, cond, poll_s=0.003)`. Per-char
  steady-state 400 → 310 ms.
- `01af1b9` — LFU-weighted eviction experiment. **Null result** on
  long passages (+0 rank-0 on user2/4/5). Root cause wasn't
  one-shot neighbours evicting hot entries; it was that long-
  passage non-rank-0 chars are mostly one-shot rare homophones
  that LFU weighting doesn't save. Unit test + honest commit
  message preserved; see `mie-p1.6-lru-plan.md` "What we learned
  about long passages" for the full autopsy.

Bugs hit during the follow-up:
- QMI wedge recovery: only physical USB unplug recovers a QMI
  that was reset mid-flash-write. `build_and_flash.sh` (J-Link
  loadbin) fails with "RAMCode did not respond" while QMI is
  dead. Memory updated in `project_qmi_wedge_recovery.md`.
- Partial `--core1` reflash used to leave Meshtastic IPC or dict
  state inconsistent enough that post-flash `ime_text_test` timed
  out; `--v4` full reflash was the quickest known recovery. Root-
  caused 2026-04-24: `--core1` was also re-running the
  `mie_dict_blob` cmake target and flashing `build/mie-host/dict.bin`
  (always MDBL v2) over any prior `--v4` MIE4 blob. v2 dict has
  different candidate rankings than v4, so the test saw unexpected
  top-8 and timed out trying to reach the target char. Fixed by
  making `--core1` flash strictly the Core 1 image — dict/font
  partitions must be refreshed with explicit `--dict` / `--font` /
  `--v4` flags.
- Boot-time `panic()` on `heap_free < 20 %` fired silently when
  adding the RTT task (+1 KB TCB/stack pushed free from 9.6 KB
  to 8.8 KB). Loosened threshold to 15 % and exposed
  `g_core1_boot_heap_free` as an SWD-readable checkpoint.
- RTT task stack at 128 words silently overflowed inside
  `TRACE()` (128 B vsnprintf scratch + parser + SEGGER_RTT_Read
  call chain). Bumped to 256 words; task ran exactly one frame
  before dying, which made the symptom look like a task-
  creation failure rather than a stack overflow.

#### M3.5 — IpcGpsBuf bridge + dummy fixed-position pipeline ✅ (2026-04-26)

**Goal.** Wire Core 1's GNSS path through the existing `IpcGpsBuf`
shared-SRAM double-buffer to Core 0's Meshtastic `GPS` class, so
`PositionModule` produces a position fix without requiring a real
sky-view fix. Phase 1 of M3.5 ships behind a dev-only build flag and
uses a fixed-position dummy NMEA injector; Phase 2 (later) will swap the
producer to the real Teseo-LIV3FL parsed-fix path.

**Phase 1 pieces (committed 2026-04-26):**

- **Core 1 dummy NMEA producer** (`firmware/core1/src/sensor/gps_dummy.{c,h}`).
  500 ms task that emits one `$GPGGA` and one `$GPRMC` per second (alternating)
  at the user-supplied dummy coordinates `25.052103 N, 121.574039 E`, alt 50 m,
  fix=1, 8 sats. Runtime-computed NMEA checksum so any string edit stays
  consistent. Selected via CMake option `MOKYA_GPS_DUMMY_NMEA=ON` (default OFF);
  `gps_task.c` `#ifdef`-switches between the dummy and the real Teseo task at
  build time. Dummy-mode Teseo source is not compiled at all (full `#ifndef`
  around `gps_task()` body).

- **`IpcGpsBuf` typedef cleanup**. The struct was 261 B by `sizeof()` but the
  shared-SRAM layout reserved 260 B (`_pad[2]` should have been `_pad[1]`).
  Fixed and added `_Static_assert(sizeof(IpcGpsBuf) == 260)`. Replaced
  `uint8_t gps_buf[260]` in `IpcSharedSram` with the typed `IpcGpsBuf gps_buf`,
  updated the `_tail_pad` arithmetic to use `sizeof(IpcGpsBuf)`. No address
  shift, all post-build `nm` symbol locations unchanged.

- **Core 0 `IpcGpsStream` adapter** (`firmware/core0/meshtastic/variants/.../ipc_gps_stream.{h,cpp}`).
  Arduino `Stream` subclass. `available()`/`read()`/`peek()` latch the
  just-published slot (`buf[write_idx ^ 1]` per the
  firmware-architecture.md §5.4 contract) on each refresh; bytes flow until
  the buffer is drained, then the next Core 1 flip latches the new slot.
  `write()` is a no-op (IpcGpsBuf is one-way).

- **Meshtastic submodule patch** (commit `06c5f15a3` on
  `tengigabytes/firmware feat/rp2350b-mokya`), all gated by
  `MOKYA_IPC_GPS_STREAM`:
  - `GPS.h` — `_serial_gps` typed as `Stream*` in this build; new
    public `static void setExternalSerial(Stream *)` for variant injection.
  - `GPS.cpp` — typed-init for the new arm, `setExternalSerial()` impl,
    `createGps()` forces `tx_gpio = 0` so the chip-detect probe loop never
    fires (the loop's gate is `if (tx_gpio && gnssModel == UNKNOWN)`).
    `setup()` accepts `GNSS_MODEL_UNKNOWN` as a valid generic-NMEA chip
    instead of returning false → looping every 2 s. The `probe()` case-0
    block's SerialUART-only baud-rate path (`end()` / `setFIFOSize()` /
    `baudRate()` / `updateBaudRate()`) is `#if`-gated out so the file still
    compiles when `_serial_gps` is `Stream*`.
  - `variants/.../variant.cpp` — `initVariant()` calls
    `GPS::setExternalSerial(&IpcGpsStream::instance())` after IPC bring-up
    but before main.cpp runs `GPS::createGps()`.
  - `platformio.ini` — drop `MESHTASTIC_EXCLUDE_GPS=1`; add `HAS_GPS=1`,
    `MOKYA_IPC_GPS_STREAM=1`, `GPS_RX_PIN=99` (any non-zero — guards the
    early-return in `createGps()`); add `ipc_gps_stream.cpp` to
    `build_src_filter`.

**Verification.** `meshtastic --port COMxx --info` reports the local
node's `position` populated with `latitude=25.052103`, `longitude=121.574039`,
`altitude=50`, `locationSource=LOC_INTERNAL`. Independently confirmed via
SWD that `IpcGpsStream::s_instance` latches each Core 1 flip and
`read_pos` advances through the 72-byte sentence per refresh.

**Two integration bugs hit during bring-up:**

1. **`gps_dummy.c` wrote into the wrong slot**. First version did
   `buf[write_idx ^ 1] = sentence; write_idx = old write_idx ^ 1`, so
   `write_idx` ended pointing at the just-published slot. The architecture
   spec (and the `IpcGpsStream` reader) follows the convention "writer
   owns `buf[write_idx]`, reader reads `buf[write_idx ^ 1]`". Reader was
   therefore always reading the empty/stale slot. Fixed by writing into
   `buf[cur]` then flipping `write_idx = cur ^ 1`. The two conventions
   are equivalent up to labelling; matching the spec is what mattered.

2. **GGA-only never produces a Meshtastic position**. After clearing the
   slot-order bug, SWD confirmed Core 0 was reading bytes — but
   `lookForLocation()` still returned false. Cause: GPS.cpp:1772 requires
   `reader.date.age() < GPS_SOL_EXPIRY_MS`, and `TinyGPSPlus::date` is
   only populated by `$GPRMC` (or equivalent) — `$GPGGA` carries time
   but no date. With only GGA, `date.age()` returns `ULONG_MAX` and the
   freshness gate fails forever. Fixed by interleaving an `$GPRMC` per
   GGA (500 ms period each, both carrying the same UTC second). RMC date
   is hard-coded to `260426` since this is a dummy producer with no
   real wall clock — the date field is just a TinyGPS validity gate.

**Investigation pattern worth carrying forward.** The whole debug arc
ran on SWD memory inspection. With `nm` to find
`_ZZN12IpcGpsStream8instanceEvE10s_instance` and `gps`, sampling 8 bytes
of the singleton (`last_seen_idx` / `read_pos` / `latched_idx`) across
2-second windows isolated "Core 0 not consuming bytes" before any code
patches. Without that, the failure (empty `position: {}` in `--info`)
would have been ambiguous between five different layers. RTT was not
needed for any of this; SWD `mem8` against a `nm`-resolved address was
faster.

**Phase 2 scope (deferred).** Real Teseo NMEA → IpcGpsBuf wiring on
Core 1 (replace the dummy producer in normal builds). Phase 2 also
needs a Core 0 sanity-check that `lookForLocation()`'s date check still
passes when the Teseo wakes up before its first RMC — the current Teseo
driver in M3.4.5d already parses RMC, so the real path should "just
work" as long as both sentences are forwarded.

#### M5 Phase 1 — RX_TEXT one-way (Core 0 → Core 1 LVGL) ✅ (2026-04-26)

**Goal.** Stop using Core 1 as a dumb byte-bridge for received text. The
M1 byte-bridge keeps the host CDC stream working (`meshtastic --info`
etc.), but every received text message also needs to *land in Core 1's
own LVGL UI* so the device can act as a feature-phone instead of just
a USB modem. M5 is the milestone where Core 1 becomes a real
PhoneAPI-style client; this Phase 1 slice covers only the simplest
one-way case (incoming text), with NODE_UPDATE / TX_ACK /
IPC_CMD_SEND_TEXT / Config IPC deferred.

**Pieces (committed 2026-04-26):**

- **Core 0 IPC observer** (`firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/ipc_text_observer.{h,cpp}`).
  `IpcTextObserver` registers a `CallbackObserver<IpcTextObserver,
  const meshtastic_MeshPacket *>` against
  `textMessageModule->notifyObservers()` (TextMessageModule.cpp:44 —
  same hook the InkHUD applets use). On each RX text, it builds an
  `IpcPayloadText` (struct already defined in `ipc_protocol.h`:
  `from_node_id`, `to_node_id`, `channel_index`, `want_ack`, `text_len`,
  flexible `text[]`) and `ipc_ring_push`es it onto the `c0_to_c1` DATA
  ring with `IPC_MSG_RX_TEXT`. Single-shot init via
  `mokya_register_ipc_observers()`, called from `main.cpp` immediately
  after `setupModules()` so `textMessageModule` is non-null. The hook
  is gated on `MOKYA_IPC_GPS_STREAM` so the patch is invisible to
  upstream arches.

- **Core 1 dispatcher** (`firmware/core1/m1_bridge/src/main_core1_bridge.c`).
  The existing `if (msg_id == IPC_MSG_SERIAL_BYTES)` chain grew an
  earlier branch for `IPC_MSG_RX_TEXT`: cast `scratch` to
  `IpcPayloadText`, clamp `text_len` against the actual payload bytes
  (defends against a producer-side length mismatch), then call
  `messages_inbox_publish()`.

- **Inbox** (`firmware/core1/src/messages/messages_inbox.{c,h}`).
  Single-snapshot store of the most recent message + a monotonic 32-bit
  `seq`. Producer writes body fields, then bumps `seq` with
  `__atomic_store_n(..., __ATOMIC_RELEASE)`; consumer reads `seq` with
  `__ATOMIC_ACQUIRE`, copies the snapshot if `seq` differs from
  `last_seen_seq`. No mutex — `bridge_task` is the sole producer and
  `lvgl_task` (via `messages_view_refresh()`) is the sole consumer, so
  the seq-release / seq-acquire pair is sufficient. Multi-message
  scrollback is M5 Phase 2 work.

- **LVGL view** (`firmware/core1/src/ui/messages_view.{c,h}` + view
  router glue). Two labels: a green header line "From 0xNNNNNNNN  ch%d"
  and a wrapped white body label, both using `mie_font_unifont_sm_16`
  so Traditional-Chinese / Bopomofo / Latin-1 all render. Refresh polls
  `messages_inbox_take_if_new(s_last_seen_seq, &snap)` and re-`set_text`s
  on snapshot bump. Added as the 5th view in `view_router`
  (`VIEW_COUNT 4 → 5`); FUNC cycles `keypad → rf → font_test → ime →
  messages → keypad`.

**Verification.** Layered test:

1. **Display + inbox path (Layers 3+4)**: SWD-poked
   `s_snap = {seq=1, from=0xDEADBEEF, text="Hello world!"}` directly,
   `s_last_seen_seq` advanced from 0 → 1 within a refresh tick,
   confirming `messages_view_refresh()` consumed the snapshot.

2. **Full end-to-end (Layers 1–4)**: peer mesh node sent several real
   `--sendtext` messages addressed to our node (`!b15db862`). SWD
   readback after the user reported "messages sent":
   `s_snap.seq = 5` (cumulative), `to_node_id = 0xB15DB862` (= our
   node, so the wire-side flow is "real DM"), `text_len = 106`,
   payload bytes decode to a complete 36-character Traditional-Chinese
   sentence `「簡單說：那個警告是預留給未來功能的 UI 佔位符，對應的操作入口目前不存在。」`
   byte-for-byte. UTF-8 boundary handling, fullwidth punctuation, and
   space-mixed Latin all survived the round trip.

**Why this slice was small.** `IPC_MSG_RX_TEXT`, `IpcPayloadText`, the
`c0_to_c1` ring, and `ipc_ring_push` were already defined and exercised
by M1's byte-bridge. The work was wiring an observer on the producer
side and adding a typed dispatcher case on the consumer side. The big
M5 questions (Core 0 selective reset, full PhoneAPI subclassing,
structured config IPC) are deferred — this slice proves the typed-IPC
pattern works end-to-end before we commit to the full architecture.

**Submodule commit.** Lives on `tengigabytes/firmware feat/rp2350b-mokya`
as `c3c7776c1`; super-project records the bump.

**Phase 2 scope (deferred).** `IPC_MSG_NODE_UPDATE` (so node list view
reflects mesh activity), `IPC_MSG_TX_ACK` (compose-side delivery
confirmation), `IPC_CMD_SEND_TEXT` (Core 1 → Core 0 outbound from MIE),
multi-message scrollback in `messages_inbox`, and the corresponding
"compose / chat thread" LVGL views.

#### M5 Phase 2 — multi-message inbox + outbound SEND_TEXT (reply flow) ✅ partial (2026-04-26)

**Goal.** Promote messages_view from "show last RX" to a real chat
panel: scroll back through recent peers, type with MIE, OK to reply.
Closes the Core 1 → Core 0 outbound text path that's been a TODO since
M1's byte-bridge first shipped. NODE_UPDATE and TX_ACK still deferred
to a follow-up slice.

**Pieces (committed 2026-04-26, super-project + submodule
`50e8cd759`):**

- **FIFO inbox** (`firmware/core1/src/messages/messages_inbox.{c,h}`).
  Single-snapshot store grew into an 8-entry ring keyed by monotonic
  `seq`; producer (`bridge_task` IPC dispatcher) appends and bumps
  `seq` with `__ATOMIC_RELEASE`, consumer (`lvgl_task` via the view)
  reads any entry by relative offset (0 = newest). The
  seq-release / seq-acquire pair is sufficient for the SPSC layout —
  no mutex. Eviction is FIFO once `count == capacity`.

- **Scrollable view** (`firmware/core1/src/ui/messages_view.c`). UP /
  DOWN walks older / newer; a third `msg N/M` footer label tracks
  position. While `offset == 0` the view stays sticky-to-newest so a
  fresh RX auto-displays; pressing UP breaks the sticky and the user
  parks at their chosen offset until DOWN walks them back to 0.

- **Outbound send** (`firmware/core1/src/messages/messages_send.{c,h}`).
  `messages_send_text(to, channel, want_ack, text, len)` builds an
  `IpcPayloadText` and `ipc_ring_push`es it onto `c1_to_c0` as
  `IPC_CMD_SEND_TEXT`. Single-shot best-effort — caller decides what
  to do on a full-ring push failure.

- **Core 0 dispatcher** (submodule, `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/ipc_command_handler.cpp`).
  `mokya_handle_ipc_command(msg_id, payload, len)` switches on
  `msg_id`; `IPC_CMD_SEND_TEXT` decodes `IpcPayloadText`, allocates a
  `meshtastic_MeshPacket` via `router->allocForSending()`, sets
  `portnum = TEXT_MESSAGE_APP` plus `to / channel / want_ack`, copies
  the UTF-8 payload, and dispatches `service->sendToMesh(p,
  RX_SRC_LOCAL, /*ccToPhone=*/true)` — same call shape as
  Meshtastic's canned-message path in InkHUD MenuApplet.
  `ipc_serial_stub::refill_rx_` was previously dropping non-
  `SERIAL_BYTES` slots with a "M4+ will route them" comment; now
  routes them to this dispatcher. Unknown msg ids are silently
  absorbed so a newer Core 1 can speak to an older Core 0 without
  crashing it.

- **IME glue** (`firmware/core1/src/ime/ime_task.{cpp,h}`). New
  `ime_view_clear_text()` resets `g_text_len` / `g_cursor` /
  `g_text[0]` under the snapshot mutex and bumps the dirty counter so
  the IME view's gated refresh repaints. Called by messages_view
  immediately after a successful push so the user gets a clean slate
  for the next message.

- **Reply UX** (messages_view OK key). On press, view snapshots
  `ime_view_text()` under `ime_view_lock`, looks up the currently
  displayed inbox entry, and sends the IME buffer to that entry's
  `from_node_id` on the same `channel_index` as a DM with `want_ack
  = true`. Empty IME or empty inbox is a no-op with a footer hint.
  Footer changes to `sent → 0xNNNNNNNN` on success, or
  `send failed (ring full)` on backpressure.

**Verification.** Two end-to-end runs:

1. **Outbound broadcast (initial cut):** OK on messages_view sent a
   hardcoded `"ping from MokyaLora"` to `MESSAGES_SEND_BROADCAST`;
   peer node TNGB-9c28 (Heltec) reported the message in the host
   `meshtastic --info`. Confirmed `c1_to_c0` ring carries the new
   msg id, Core 0 dispatcher picks it up, MeshService routes it, and
   the LoRa tx path is intact.

2. **Outbound reply (final shape):** typed a Traditional-Chinese
   reply on Core 1's IME, FUNC-cycled to messages view, OK pushed
   the IME buffer to TNGB-50ca's node id (the original peer who
   sent us the test DM). Peer received it as a normal DM. IME
   buffer cleared automatically; messages_view footer updated to
   `sent → 0x58a750ca`.

**Why two-stage UX (read in messages, type in IME).** The simplest
"OK = send" wiring without a dedicated compose state. UP/DOWN already
mean "scroll messages" on this view, so reusing them for a compose
cursor would conflict. A future M6 slice can introduce a true compose
overlay; for now the user mentally pairs "currently displayed = current
recipient", which matches feature-phone reply UX.

**Still deferred to M5 Phase 3+:** `IpcPhoneAPI` subclass to fully
replace the byte bridge. (`IPC_MSG_NODE_UPDATE` and `IPC_MSG_TX_ACK`
landed in M5 Phase 3; "Core 0 selective reset on config change" is
resolved by the B2 soft-reload below — a hardware selective reset
turned out to be infeasible on RP2350.)

#### B2 — IPC config soft-reload (2026-04-26)

**Goal.** Let Core 1 issue config changes without rebooting the chip.
Originally framed as "selective reset of Core 0 only" (so USB CDC and
the LVGL UI on Core 1 survive the modem-side restart). After a hardware
POC closed off that direction, pivoted to a pure-software soft-reload
through Meshtastic's existing `service->reloadConfig()` path.

**POC: Core-0-only watchdog reset (dead end).** `variant.cpp` gained a
warm-boot detect path (read `WATCHDOG_REASON` + `g_ipc_shared.c1_ready`
before `ipc_shared_init` / `multicore_reset_core1` / `multicore_launch_core1_raw`)
so a selective reset wouldn't trash the still-running Core 1.
`poc_c0_reset.cpp` (gated `MOKYA_POC_C0_RESET=1`) armed a FreeRTOS task
that, 30 s after boot, set `PSM_WDSEL = PROC0_BITS` and triggered
watchdog. **Empirical result:** PSM did isolate the reset domain —
post-trigger SWD probe shows `PSM_FRCE_OFF.PROC1=0`, `PSM_DONE.PROC1=1`,
and shared SRAM (boot_magic, c0_ready, c1_ready) intact — but Core 1's
PC ended up at `0x019E` in the bootrom mailbox FIFO handler and ime
ticks stopped at exactly `t=30s`. Root cause: **RP2350 bootrom unconditionally
calls `multicore_reset_core1` on every proc0 cold-boot**, regardless of
WDSEL, regardless of whether Core 1 is already running. There is no SoC-
level workaround that keeps Core 1 alive across a proc0 restart. POC
files (`poc_c0_reset.cpp`, warm-boot detect in `variant.cpp`) are kept
as documentation; build flag commented out so the trigger never fires
in production.

**Pivot: Meshtastic already has the soft-reload path.**
`MeshService::reloadConfig(int saveWhat)` (`mesh/MeshService.cpp:134`)
calls `nodeDB->resetRadioConfig()`, fires `configChanged.notifyObservers(NULL)`
(which `RadioInterface::reconfigure()` listens to and uses to put the
radio in standby + reprogram modem params + restart receive), and then
`nodeDB->saveToDisk(saveWhat)`. AdminModule's `handleSetConfig`
(`AdminModule.cpp:780`) explicitly marks LoRa config changes — including
region — as `requiresReboot = false`: the same observer chain re-tunes
the radio live. Validated upstream by sending `meshtastic --set
lora.tx_power 27` over USB CDC: reply succeeded, `rebootCount` stayed at
0, USB CDC stayed connected.

**Step 1 implementation (`firmware/core0/.../variants/rp2350/rp2350b-mokya/ipc_config_handler.cpp`).**
GET / SET / COMMIT for `IPC_CFG_LORA_REGION`, `IPC_CFG_LORA_TX_POWER`,
and `IPC_CFG_LORA_HOP_LIMIT`. SET writes `config.lora.*` directly
(matching AdminModule's in-memory pattern); COMMIT calls
`mokya_meshservice_reload_config_segment_config()`, a thunk in
`ipc_command_handler.cpp` that wraps `service->reloadConfig(SEGMENT_CONFIG)`
— kept in the existing TU because that's where `MeshService.h` is
already included, avoiding a second copy of the heavy header dep chain
in `ipc_config_handler.cpp`. Other keys defined in `IpcConfigKey` return
`UNKNOWN_KEY` until follow-up slices wire them up. Module-level
`s_pending_lora_reload` flag lets COMMIT skip `reloadConfig` when no
LoRa-touching SET happened since the last commit (so a future Owner-
only SET batch won't bother re-tuning the radio).

**Dispatcher** (`ipc_command_handler.cpp`): `mokya_handle_ipc_command`
gains three new cases — `IPC_CMD_GET_CONFIG`, `IPC_CMD_SET_CONFIG`,
`IPC_CMD_COMMIT_CONFIG` — each forwarded to the corresponding
`mokya_handle_ipc_*_config` extern.

**End-to-end verification** (`scripts/test_ipc_config.sh`). Reads
`c1_to_c0_ctrl.head` from shared SRAM via J-Link, computes the next two
slot addresses, writes an `IpcMsgHeader` + `IpcPayloadConfigValue` for
SET and a header-only frame for COMMIT, advances head twice, then
`meshtastic --get lora.tx_power` to confirm the change took. Three
round-trips on real hardware (`22 → 17 → 25 → 22`) all read back the
new value with `rebootCount` unchanged and CDC uninterrupted across the
whole sequence. The full path Core 1 ring → Core 0 dispatcher → config
handler → `reloadConfig` → `configChanged` observer → `RadioInterface`
re-tune is live.

**Why SWD-inject for the test rather than a Core 1 client.** Decoupling
the protocol verification from the eventual UI lets the next slice add
a settings view in LVGL without re-validating the IPC pipe. The script
also doubles as a debugging tool when wiring real Core 1 clients (drops
in to substitute for the UI when reproducing edge cases).

**Submodule commit.** `ed6fa3ee5` on `tengigabytes/firmware
feat/rp2350b-mokya`. Super-project bump in `9f19f39`.

**Limitations / remaining work.**

- LoRa subset only. Other categories (Device, Position, Power, Display,
  Channel, Owner) need their handler cases filled in. Mostly mechanical;
  Owner uses a different reload path (`reloadOwner` instead of
  `reloadConfig`).
- No reboot-required path yet. Some keys really do need a chip restart
  (Device.role, rebroadcast_mode, most module configs). Their handlers
  should set a separate flag and COMMIT should fall back to
  `RebootNotifier` for those. Not implemented in step 1.
- No Core 1 client UI. Validation goes via the SWD-inject script; the
  LVGL settings view is a separate slice.
- No batch-commit semantics surfaced to the protocol — multiple SETs
  followed by one COMMIT already works (only the COMMIT triggers
  reload), but there's no explicit ABORT to discard pending SETs. Add
  if usage requires it.

---

#### P2-20 — Deferred Core 1 launch + SWD-only QMI wedge recovery (2026-04-27)

**Status:** ✅ Fixed.
**Impact:** All cold boots — fresh-erase or warm — were dying with Core 1
HardFault until this. Discovered while attempting to build the
COMMIT_REBOOT IPC path for B2 follow-up; the reboot path turned out to be
the latest in a long line of triggers for the same underlying race.

**Symptom:** After any cold boot, Core 1 HardFaults at IBUSERR fetching
its own image around `0x10200674` (early `main()`). Core 0 reaches
Meshtastic idle hook fine, but USB CDC bridge is down so `--info` times
out. Repeated reset cycles eventually leave the chip in "QMI wedge"
state — J-Link `erase` returns RAMCode timeout, no SWD reset strategy
recovers.

**Root cause (verified).** P2-11 wrap (`flash_safety_wrap.c::mokya_flash_park_core1`)
parks Core 1 only if `c1_ready==1`, but Core 1 sets that flag late in
its boot. Core 0's first boot-time flash op (LittleFS format on fresh
erase, NodeDB save on warm boot) fires within ~50–200 ms of
`initVariant()` returning — before Core 1 reaches `c1_ready=1`. With
c1_ready=0 the wrap falls through, Core 0 enters direct mode for the
flash op, Core 1 is still fetching flash for its own startup → IBUSERR.

**Fix.** `firmware/core0/.../variant.cpp`: defer Core 1 launch from
`initVariant()` to `vApplicationIdleHook()`. `initVariant()` now only
calls `multicore_reset_core1()` (holding Core 1 in `PSM_FRCE_OFF.PROC1`)
and stashes launch params; `vApplicationIdleHook` does the actual
`multicore_launch_core1_raw()` on first invocation, which is guaranteed
to be after Meshtastic `setup()` has finished and all boot-time flash
ops are complete. Boot is now race-free: while Core 0 does its early
flash work, Core 1 is explicitly held in PSM reset (not fetching
anything). Once Core 0 reaches idle, Core 1 launches into a quiet bus.

**SWD-only recovery method (new).** Documented in `~/.claude/projects/.../memory/reference_qmi_wedge_swd_recovery.md`:
RP2350 bootrom honours `WATCHDOG.SCRATCH4=0xb007c0d3` + XOR-checked
scratch[5..7] as a "boot-to-RAM-PC" magic. Load a small Thumb stub at
`0x20040000` that calls bootrom `rom_reboot(BOOTSEL|NO_RETURN)`, set
scratch redirect, write `0xC0000007` to `WATCHDOG.CTRL`, watchdog times
out, bootrom jumps to stub, stub enters BOOTSEL mode → RPI-RP2 USB MSC
mounts → J-Link RAMCode works again, full chip erase + reflash OK.
Replaces the previous `project_qmi_wedge_recovery` claim that "only
physical USB unplug can recover".

**Verification.** BOOTSEL recovery → full erase (43 s) → flash patched
ELF + Core 1 BIN + dict + font → 5 consecutive `meshtastic --info` over
60 s all succeed (`nodedbCount=1, rebootCount=0`), both cores running
real workload (Core 0 in `xTaskIncrementTick` / scheduler, Core 1 in
`bridge_task` / USB device task).

**Side-effect:** Core 1 image now starts hundreds of ms later than
before. UI / IPC consumers on Core 1 should not assume Core 0 is "still
booting" when they come up. Boot phase breadcrumb gains `0x17` (Core 1
launched late from idle hook).

#### M5 Phase 2 — Cascade PhoneAPI architecture, Phase A skeleton ✅ (2026-04-27)

**Plan.** `~/.claude/plans/core1-usb-core1-composed-globe.md`. Decided
that the right end-state is **not** `IPCPhoneAPI` (a second PhoneAPI
session on Core 0 dedicated to Core 1 — the original M5 framing), but
instead a **cascade**: Core 1 is the *primary* PhoneAPI client, USB CDC
is a transparent byte-tunnel sub-mode that activates only when a host
opens the CDC. The SERIAL_BYTES ring carries exactly one PhoneAPI
session. Core 0 is unaffected — entire change is Core-1-side.

**Why cascade over dual-session.**
1. Core 0's PhoneAPI has a hidden global static (`heartbeatReceived` in
   `PhoneAPI.cpp:36`) that breaks when two instances are both active —
   would require patching Meshtastic submodule.
2. PhoneAPI lazy-starts on `want_config_id` (none of the 11 streaming
   states fire without a client request). Two sessions would mean two
   `want_config_id`s, each restarting the state machine and clobbering
   the other's mid-stream output.
3. Cascade reuses the existing `ipc_serial_stub` byte transport
   verbatim. Zero Meshtastic patches needed for transport itself.
4. License: `mesh.pb.h` is GPL-3.0 in `firmware/core0/meshtastic/protobufs/LICENSE`.
   Core 1 (Apache-2.0) cannot include it. Either approach needed an
   independent decoder; cascade keeps the boundary cleaner because the
   only crossing point is opaque bytes on a ring.

**License approach (decided 2026-04-27):** hand-written minimal
decoder in `firmware/core1/src/phoneapi/`, no nanopb runtime, no
generated headers. Field numbers are wire-format interop facts and
are not copyrightable — identical to writing a third-party HTTP
client. Each new file carries `SPDX-License-Identifier: Apache-2.0`
and a header comment noting the wire format mirrors Meshtastic's
public protobufs.

**Phase A deliverable: byte-stream tap + tag identification.** New
files under `firmware/core1/src/phoneapi/`:

- `phoneapi_framing.{c,h}` — 5-state machine for `0x94 0xC3 LEN_HI
  LEN_LO <payload>`. 512 B internal payload buffer. Bulk-copies
  payload across IPC ring slot boundaries (single 256-byte slot
  cannot carry a full FromRadio, so multi-slot reassembly is
  mandatory). Stats counters (`frames_ok`, `frames_oversized`,
  `resync_drops`) inspectable via SWD.
- `phoneapi_decode.{c,h}` — varint reader + wire-type-aware skip;
  `phoneapi_decode_from_radio()` walks the FromRadio message,
  identifies which `payload_variant` oneof field is present (tags
  2..17 per `mesh.proto:2070`), records its byte length or scalar
  value. Returns the top-level `FromRadio.id` (field 1) too.
- `phoneapi_session.{c,h}` — Phase A glue. Owns one static
  `phoneapi_framing_t`, the on-frame callback decodes + emits one
  RTT trace per frame (`TRACE("phapi", "rx_frame", "len=%u,id=%u,
  tag=%s,val=%u", ...)`), bumps a per-tag histogram counter.

Bridge task tap point: `main_core1_bridge.c:340` (right when a
SERIAL_BYTES slot is popped, before the existing `tud_cdc_write` loop).
The cascade tap and CDC pass-through are independent — neither
modifies bytes the other touches.

**Build-flag toggle: `MOKYA_PHONEAPI_CASCADE`** (CMake option, default
`OFF`). All new sources are listed under
`$<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:...>`, the `phoneapi_session.h`
include in `main_core1_bridge.c` is `#ifdef`-gated, and the init +
feed call sites are `#ifdef`-gated too. With the flag OFF the binary
is byte-identical to before (verified by clean Phase A build matrix:
both `OFF` and `ON` link successfully without warnings).

**Validation gate (Phase A success criteria from the plan).** ELF
flashed with `MOKYA_PHONEAPI_CASCADE=ON`, RTT logger captured with
`JLinkRTTLogger.exe -RTTSearchRanges "0x20000000 0x80000"`, USB host
ran `python -m meshtastic --info`. Captured 50 `phapi` events:

| Tag | Count | Notes |
|---|---|---|
| `rebooted` (8) | 1 | first frame after boot, before `want_config_id` |
| `my_info` (3) | 1 | MyNodeInfo |
| `deviceui` (17) | 1 | DeviceUIConfig |
| `node_info` (4) | 2 | matches the 2 nodes in NodeDB |
| `metadata` (13) | 1 | DeviceMetadata |
| `channel` (10) | 8 | 8 channel slots (1 primary + 7 disabled) |
| `config` (5) | 10 | Config oneof variants |
| `module_config` (9) | 16 | ModuleConfig oneof variants |
| `file_info` (15) | 6 | FileInfo manifest |
| `config_complete_id` (7) | 1 | terminator |
| `queue_status` (11) | 2 | heartbeat replies |

`bad_frame` and `frames_oversized` both 0. USB host's `--info` returned
the full state dump unchanged — cascade tap and pass-through coexist.
This proves: framing parser handles cross-slot frames (saw 115 B
node_info crossing slot boundary), tag identification correctly maps
all 11 expected variants, and the tap is non-destructive to the
existing transport.

**Memory cost (Phase A only).** ELF text +~1 KB (decoder code), bss
+~600 B (512 B framing buffer + 80 B stats counters). Negligible.

**What Phase A does NOT do:** field-level decode (no NodeDB cache, no
my_info struct, no config), no ToRadio encoder, no mode state machine
(the bridge always taps regardless of USB DTR — fine for Phase A
because it's just observation), no LVGL view changes. Those land
across Phases B–D per the plan.

**Next: Phase B.** Field-level decoders for the 7 message types we
care about (my_info, node_info, channel, config sub-oneof,
module_config tag-only, metadata, packet) + `phoneapi_cache.{c,h}`
holding NodeDB (LRU, 128 entries) + my_info + channels + config
snapshots. Validation: SWD dump of cache after a `--info` run should
match what Meshtastic CLI reports. Risk R1 mitigation
(shadow-then-swap on `config_complete_id`) gets implemented here.

---

## Cross-cutting Decisions (2026-04-15)

### DEC-1 — MIE keycode API refactor (out-of-band, pre-M3)

`mie::KeyEvent` changes from `{ row, col, pressed }` to `{ keycode, pressed }`.
MIE C API changes from `mie_process_key(ctx, row, col, pressed)` to
`mie_process_key(ctx, keycode, pressed)`.

**Rationale.** MIE is a service layer; exposing 6×6 matrix geometry through
its public API leaked hardware knowledge into the IME. Subsequent
multi-producer requirements (M3 keypad + M9 USB Control injection) would have
forced the injection path to reconstruct matrix coordinates just to call MIE
— a layering inversion. Folding the matrix into a single translation table
inside `KeypadScan` lets every layer above see only semantic keycodes.

**Two-header split:**

| Header | Owner | Content |
|--------|-------|---------|
| `firmware/mie/include/mie/keycode.h` | MIE (MIT) | Canonical `MOKYA_KEY_*` constants, `0x01..0x3F`; no matrix concept |
| `firmware/core1/src/keymap_matrix.h` | Core 1 (Apache-2.0) | 6×6 `(row, col) → keycode` lookup, applied once inside `KeypadScan` |

**Scope.** `hal_port.h`, `mie.h`, `ime_logic.*`, `mie_c_api.cpp`, 120
GoogleTest cases, `mie_gui`, `hal/pc/key_map.h`. Android / Windows IME
bindings inherit via the updated C API signature.

**Timing.** Immediately, in `dev-Sblzm`, before M3 starts. M3 keypad driver
writes directly against the new API; no intermediate (row, col) phase.

### DEC-2 — USB Control Interface added to Phase 2 as M9

Phase 2 milestone blueprint now carries M9. See
[`docs/design-notes/usb-control-protocol.md`](../design-notes/usb-control-protocol.md)
for the normative wire protocol and
[`docs/requirements/software-requirements.md`](../requirements/software-requirements.md) §6
for the non-functional requirements.

**Integration summary.**

- USB Mode renamed from `A/B` to `OFF/COMM`. Mode COMM is a composite
  TinyUSB device exposing two CDC interfaces: CDC#0 (Meshtastic bridge,
  unchanged) and CDC#1 (Control Protocol).
- Core 1 adds `UsbCtrlTask` (priority 3, 2.5 KB stack). Build flag
  `MOKYA_ENABLE_USB_CONTROL=ON` by default (no CE/FCC submission planned);
  when OFF, CDC#1 descriptor and the task are dropped entirely at link time.
- `key_event_t` is upgraded to carry a `source` flag (`HW` vs `INJECT`).
  HW events win arbitration within the 20 ms debounce window; INJECT events
  losing the race return `ERR_BUSY` to the host. Safe mode rejects all
  state-mutating INJECT commands.
- Authentication uses HMAC-SHA256 challenge-response against a 32-byte
  pairing key in LittleFS. Supports remote-debug use case (device away from
  the user, still auditable via signed HMAC).

**M3/M4 implication (G2).** Keypad driver is written from day-1 against the
multi-producer KeyEvent queue with source flag — even though `UsbCtrlTask`
is not yet implemented, the queue shape is the final shape. M9 then only
adds a second producer, not a queue refactor.

**Host tooling.** `tools/mokya-ctl/` Python package (CLI + reusable
`mokya_control` module) ships alongside the firmware; `Key` / `UiAction`
Python enums are generated from the C headers to prevent drift.

---

## Open Follow-ups (rolling)

Snapshot 2026-04-27. Update on commit when items resolve or new ones
land. Items closed should move into the relevant milestone narrative
above (with date) and be deleted from this list.

### IME quality / measurement

- ~~**settings_view per-LVGL-tick cost — +14 % wall-time regression
  on bench (2026-04-27).**~~ **Fixed same day, commit `37b9f9d`
  (mitigation b — stash hidden panels off-screen).** Bisect
  confirmed `settings_view` was the entire +57 ms/char cost; watchdog
  / postmortem / ime_request_text contributed zero. The fix
  reparents inactive panels to a free-standing `lv_obj_create(NULL)`
  stash (LVGL only traverses the *active* screen tree during
  refresh, so children of an unloaded screen are skipped entirely).
  Net wall-time on `ime_text_test.py user1.txt` (228 chars):
  478 → **405 ms/char**, **2.6 % faster than the historical
  416 ms baseline** because the stash trick also eliminates the
  per-tick cost of the other 5 inactive panels (rf / font_test /
  ime / messages / nodes) that LVGL was previously walking every
  refresh. IME quality unchanged.
- ~~**lazy_friday picker rank-7 commit miscommit.**~~ **No-repro on
  2026-04-27** — three consecutive runs against
  `scripts/ime_passage_lazy_friday.txt` (warm, then `--erase` cold
  Pass 1, then warm Pass 2) all reported 327/327 CJK commit ✓ 100 %,
  including every rank-7 hit. Picker bullet retired; if the cascade
  recurs, file a fresh entry with the exact LRU state and dict
  blob hash so the trigger is locatable.
- ~~**Refresh Phase 1.6 regression table** in `mie-v4-status.md`.~~
  **Done 2026-04-27.** New "single-pass dual-mode snapshot" table
  added below the original two-pass table, capturing `--precise-hints`
  vs default HINT_ANY for user2..5 / t9_stress / lazy_friday with the
  rewritten fictional content. Two-pass numbers themselves still
  reflect pre-rewrite content — refresh as part of #11.
- ~~**Re-run two-pass LRU regression** on the new fictional passages.~~
  **Done 2026-04-27.** kCap = 128 delivers measurable cold→warm
  rank-0 lift across user2..5 (+9 / +6 / +5 / **+47**) and lazy_friday
  (+3). user5 in particular jumps from kCap = 64 baseline +0 to +47.
  All passages 100 % commit on both passes. Refreshed table in
  `mie-v4-status.md`. Lingering FAIL* flags are the script's
  rank≥8-promotion gate misfiring on small tails — see the analysis
  paragraph below the table.

### Phase 2 plumbing

- ~~**Verify RTT key-inject `--transport rtt`** end-to-end.~~
  **Done 2026-04-27.** `ime_text_test.py scripts/ime_passage_user2.txt
  --transport rtt` ran 218/218 commit ✓ 100 % with zero `rtt inject
  timeout` errors at 420 ms/char (faster than the SWD pass at 458
  ms/char on the same content cold). RTT transport is wired end-to-
  end: rtt_send_frame → ring drain → key_inject_rtt task →
  key_event_push_inject_flags → ime_task. SWD/RTT contention was a
  red herring; root cause was the warm-detect HardFault as suspected.

### B2 IPC config follow-ups

All three closed by 2026-04-27 — see "B2 Stage 2 — settings_view"
commit and the "B2 Stage 1" entry for full keys + COMMIT_REBOOT path.

- ~~Extend handler keys beyond LoRa subset.~~ Done in B2 Stage 1.
- ~~Reboot-required path.~~ Done — `IPC_CMD_COMMIT_REBOOT (0x8C)`
  routes to `mokya_request_graceful_reboot()`. Per-key needs_reboot
  table lives Core-1-side in `firmware/core1/src/settings/settings_keys.c`.
- ~~Core 1 settings view UI.~~ Done — `firmware/core1/src/ui/settings_view.c`
  (Stage 2). 6 groups, UP/DOWN row, LEFT/RIGHT group, OK edit /
  Apply. SK_KIND_STR (owner names) deferred to Stage 3 (IME path).

### Watchdog liveness chain + cross-reset postmortem (2026-04-27)

**Watchdog (commit `770daec`):** Replaces aspirational §9.4 design.
Core 0 `vApplicationIdleHook` bumps `g_ipc_shared.c0_heartbeat` each
tick. Core 1 `wd_task` polls every 200 ms, kicks HW watchdog (3 s
timeout) on heartbeat advance, stops kicking after 4 s of silence
so HW WD wins. `mokya_watchdog_pause/resume()` (inline atomic
helpers) are nesting counters that opt out of silence detection
during long blocking ops (flash erase/program on both cores,
`Power::reboot` 500 ms USB-disconnect delay). Avoids
`WATCHDOG.SCRATCH4..7` (BOOTSEL recovery uses those —
`reference_qmi_wedge_swd_recovery` memory). Cost: 192-word stack +
~88 B TCB ≈ 0.86 KB heap. Boot panic threshold 15 % → 14 % since
RAM region is full.

Verified end-to-end: SWD-pokeable `g_mokya_wd_test_freeze_heartbeat`
flag → halts Core 0 idle hook → wd_task observes silence ramping
(silent_max 5/11/17/23 over 4 s), state transitions KICK→SILENT at
t=4 s, kick count freezes, HW watchdog fires at ~t=7 s,
`WATCHDOG.SCRATCH3` (boot counter) increments by 1.

**Postmortem (commit `a3005dc`):** RP2350 SRAM survives watchdog
reset and SYSRESETREQ; only POR/BOR clears it. Two 128 B
`mokya_postmortem_t` slots at `0x2007FD00..0x2007FDFF` inside
`g_ipc_shared`; `ipc_shared_init` partial-init skips this window.
Three capture paths:

1. **Cortex-M fault handlers** (strong overrides of
   `isr_hardfault`/`memmanage`/`busfault`/`usagefault` on both cores)
   capture stacked frame + CFSR/HFSR/MMFAR/BFAR + EXC_RETURN +
   FreeRTOS task name (Core 1) → SYSRESETREQ via AIRCR direct.
2. **wd_task SILENT transition** — `mokya_pm_snapshot_silent()`
   first-event-wins; captures `c0_heartbeat`, `wd_state`,
   `silent_ticks`, `wd_pause`, task_name="wd".
3. **`RebootNotifier::onReboot`** tags GRACEFUL_REBOOT before the
   500 ms delay so soft reboot is distinguishable from a fault.

Surface path: Core 1's `mokya_pm_surface_on_boot()` runs once at top
of `main()` after `SEGGER_RTT_Init()`; reads BOTH slots, TRACE-prints
via RTT, clears magic. Payload preserved for SWD inspection across
subsequent reboots. Core 0 deliberately doesn't self-print (early
Serial unreliable; Core 1 RTT works from first iteration).

Test hooks shipped: `g_mokya_pm_test_force_fault` (`udf #0` →
UsageFault → HardFault) + `g_mokya_wd_test_freeze_heartbeat` (above).
Both default 0, no production cost.

Verified all three paths produce expected forensic content:

- WD_SILENT: cause=1, t=21.5 s pre-reset, c0_heartbeat captured,
  wd_state=last KICK, silent_max=20 (=threshold), task="wd".
- HARDFAULT (UDF inject): cause=2, pc inside `mokya_pm_test_poll`,
  cfsr=0x00010000 (UFSR.UNDEFINSTR), hfsr=0x40000000 (FORCED),
  exc_return=0xFFFFFFFD (PSP/no FP), task="bridge".
- GRACEFUL_REBOOT (`meshtastic --reboot`): cause=7, c0_heartbeat
  frozen at 0x6C26091F, no fault fields, core=0.

Memory cost: 256 B inside the existing 24 KB `.shared_ipc` region
(claimed from `_tail_pad_pre`); 0 B from heap. Postmortem at
`0x2007FD00`, ahead of the existing `ime_view_debug` (0x2007FE00) and
breadcrumb tail (0x2007FFC0). See `docs/design-notes/firmware-architecture.md`
§9.4 / §9.5.

---

## Issues Log (Phase 2)

| # | Date | Area | Issue | Resolution |
|---|------|------|-------|-----------|
| P2-20 | 2026-04-27 | P2-11 race window — Core 1 IBUSERR HardFault during Core 0's first boot-time flash op (LittleFS format on fresh-erase, NodeDB save on warm boot), + the chip subsequently entering "QMI wedge" state where SYSRESETREQ + J-Link RAMCode erase all fail | **Symptom 1 (Core 1 dies on cold boot):** After full chip erase + reflash, on boot Core 1 runs its reset handler → main() → first flash fetch at PC=`0x10200674` → **CFSR=0x101 (IBUSERR+IACCVIOL)** HardFault loop (PC stuck at `0xEFFFFFFE` exception-return value, IPSR=003). Core 0 boots into Meshtastic idle hook fine, but USB CDC bridge runs on Core 1 so `--info` times out. Reproducible across baseline ELF (commit `ed6fa3e`) and rebuilt baseline (`14dd315`), and across erased / fresh / valid-LittleFS conditions — meaning it isn't fresh-LittleFS specific. **Symptom 2 (post-fault QMI wedge):** Multiple cycles of "Core 1 dies → SYSRESETREQ → re-boot → Core 1 dies again" eventually leave the chip in a state where J-Link's `erase` command times out with "RAMCode did not respond in time", PC stays in bootrom flash function (`0xF80`/`0xF88`), and **no SWD reset strategy recovers** — RSetType 0/1/2/8, watchdog timeout (writing `0xC0000007` to `WATCHDOG.CTRL`), DCRSR/DCRDR direct PC write (silently rejected), `unlock` + low-speed (100 kHz) all fail. Memory `project_qmi_wedge_recovery` previously claimed only physical USB unplug recovers this. **Investigation:** Halted Core 0 mid-fault, observed XIP_CTRL = `0x80` (cache disabled, SPLIT_WAYS bit only) and DIRECT_CSR = `0x03010803` (EN=1) **at the moment of fault**. So Core 0 (or some boot path) was in QMI direct mode with cache off when SysTick fired (IPSR stacked = `0x0F`), Core 0 ISR then IBUSERRed fetching `xTaskIncrementTick` (`0x1002F630`). Same pattern for Core 1: in early `main()` (PC=`0x10200674` is `hw_clear_bits` on RESETS), IBUSERR fetching from flash because Core 0 was in direct mode for its first flash op. **Race window:** P2-11 wrap (`mokya_flash_park_core1`) parks Core 1 ONLY if `c1_ready==1`. Core 1's `c1_ready` write happens late in its boot (after .bss zero, .data copy, runtime_init, full main() init reaching `ime_task_start`). Meanwhile Core 0's first flash op (LittleFS format on fresh erase, NodeDB save on warm boot) fires within ~50–200 ms of `initVariant()` returning. So with very high probability, Core 0's first wrap call sees `c1_ready==0`, falls through, enters direct mode while Core 1 is still fetching flash → IBUSERR. The "QMI wedge" symptom is a downstream artefact: when Core 1 hardfaults mid-fetch, some cache lines or QMI-prefetch state corrupts; subsequent SYSRESETREQ doesn't fully reset that, so re-boot hits the same fault sooner each cycle, eventually Core 0 itself dies and bootrom is stuck. **SWD-only recovery method (new, supersedes "physical unplug only"):** RP2350 bootrom honours `WATCHDOG.SCRATCH4=0xb007c0d3` + XOR-checked `SCRATCH5/6/7` as a "boot-to-RAM-PC" magic. Load a small Thumb stub at `0x20040000` that calls bootrom `rom_table_lookup('R'\|'B'<<8, RT_FLAG_FUNC_ARM_SEC)` → `rom_reboot(BOOTSEL\|NO_RETURN, 10, 0, 0)`, set scratch[4..7] to point at the stub, write `0xC0000007` to `WATCHDOG.CTRL`, watchdog times out, bootrom reads scratch magic, jumps to stub, stub enters BOOTSEL mode, USB MSC `D:` drive appears. From BOOTSEL state J-Link RAMCode works again, full chip erase OK (43 s for 16 MB), full reflash via `loadfile` works. Stub source + complete script in `~/.claude/projects/.../memory/reference_qmi_wedge_swd_recovery.md`. **Why "QMI wedge" is reachable in this codebase but not on stock Pico SDK builds:** stock builds either run single-image (no Core 1 race) or use SDK's full multicore_launch_core1 which does the FIFO handshake before any flash op. MokyaLora's dual-image custom launch leaves Core 1 running its own image while Core 0 begins flash work — the SDK's race-free guarantee doesn't transfer. | **Fixed (2026-04-27): defer Core 1 launch from `initVariant()` to `vApplicationIdleHook()`.** `firmware/core0/.../variants/rp2350/rp2350b-mokya/variant.cpp`: instead of calling `multicore_launch_core1_raw()` at the end of `initVariant()`, just call `multicore_reset_core1()` (which holds Core 1 in `PSM_FRCE_OFF.PROC1` reset) and stash the launch params in static globals (`s_core1_sp`, `s_core1_entry`). Add `extern "C" void vApplicationIdleHook(void)` (FreeRTOS fires this on Core 0 once no other task is runnable — guaranteed to be after `setup()` and all boot-time flash ops have completed). On the first call, the hook does `multicore_reset_core1()` again (idempotent — keeps Core 1 in clean bootrom mailbox state) then `multicore_launch_core1_raw(entry, sp, vt)`. Result: Core 1 starts executing its image with Core 0 already idle, no concurrent flash ops, no race. The boot-time flash burst (LittleFS format on fresh erase) runs with Core 1 explicitly held in PSM reset — guaranteed not fetching flash. After that burst is done and Core 0 reaches idle, Core 1 launches and immediately joins the IPC handshake. `configUSE_IDLE_HOOK=1` is the Arduino-Pico FreeRTOSConfig.h default so no platformio.ini change needed. **Verification:** BOOTSEL recovery → erase → flash patched ELF → 5 consecutive `python -m meshtastic --info` over 60 s all return cleanly (`nodedbCount=1, rebootCount=0`). Both cores running with PC samples varying across real workload (Core 0 around `0x1002F324`/`0x1002F360` = FreeRTOS scheduler / `xTaskIncrementTick`; Core 1 around `0x10215390`/`0x102127F8` = bridge_task / USB device task). Owner reads `Meshtastic b862 (b862)`. **Side-effects worth noting:** (1) Boot timing changes: Core 1 image now starts ~hundreds of ms later than before, after Meshtastic setup completes. UI / IPC consumers on Core 1 should not assume Core 0 is "still booting" when they come up. (2) The `dbg[3]=*sentinel` snapshot in initVariant phase 4 is now meaningless (Core 1 hasn't run, can't have written sentinel). Phase byte advances to `0x17` from idle hook on first launch — useful as a "Core 1 launched late" SWD breadcrumb. |
| P3-5 | 2026-04-26 | Build/test flow — MDBL v2 dict was the default flash output even though all of M5 / Phase 1.6 baselines run on MIE4 v4 | **Symptom:** Recurring footgun: a "fresh" `bash scripts/build_and_flash.sh` (no flag) silently wrote MDBL v2 over the flash dict partition, and any subsequent `ime_text_test` measured ~1700 ms/char — an apparent ~4× regression that was actually just v2 ranking the candidates differently from v4. Already partly mitigated by P2-15 follow-up (--core1 no longer touches the dict partition) and the 2026-04-24 `--core1` tightening, but the no-flag default kept producing the wrong format every time the dict was deliberately reflashed. Most recently bit the M5P3 benchmark run, where a 1700 ms/char measurement masked the real ~430 ms/char baseline until it was traced back to the dict format on flash. | **Fixed (2026-04-26).** Default flipped to MIE4 v4: `scripts/build_and_flash.sh` now sets `USE_V4=true` unconditionally, `--v4` is a kept-for-compat no-op, and the only way back to v2 is the explicit `--v2-deprecated` flag (which prints a 3-second loud warning before flashing). `scripts/ime_text_test.py` rejects MDBL on both the host-side dict file AND on the device flash (probes `0x10400000` magic via SWD before running) — bench runs against a stale v2 flash now exit with a clear "reflash via build_and_flash.sh --dict" message instead of producing nonsense numbers. Core 1's `mie_dict_loader.c` exports `g_mie_dict_format` (MIE4 / MDBL_DEPRECATED / NONE) for SWD inspection. v2 binaries in `firmware/mie/data/` (`dict_dat.bin`, `dict_values.bin`, `en_*.bin`) and the v2 generator path in `gen_dict.py` are kept as archaeology with a `firmware/mie/data/README.md` note marking them retired. |
| P2-1 | 2026-04-11 | Meshtastic `AddI2CSensorTemplate.h` | Latent upstream bug — non-dependent `ScanI2CTwoWire` name fails to resolve under `MESHTASTIC_EXCLUDE_I2C=1` due to two-phase template lookup | Workaround: `-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` in Core 0 variant. Upstream fix would be to also guard the `ScanI2CTwoWire*` parameter declaration under `#if !MESHTASTIC_EXCLUDE_I2C`. |
| P2-2 | 2026-04-11 | Arduino-Pico 5.4.4 FreeRTOS SMP on RP2350 | Core 1's passive-idle task HardFaults in `vStartFirstTask` the instant `xPortStartScheduler` launches it, blocking Core 0 from ever reaching `setup()`. CFSR=0x101 (IACCVIOL+IBUSERR), MMFAR=BFAR=0x2000C5AC (inside Core 1's own PSP region). Independent of `-DNO_USB` — architecture change, not a stub bug. | Switched to single-core FreeRTOS (`-DconfigNUMBER_OF_CORES=1`). Requires guarding the SMP-only framework code (`vTaskCoreAffinitySet`, `vTaskPreemptionDisable/Enable`, IdleCoreN task creation) in `freertos-main.cpp` / `freertos-lwip.cpp` and fixing the missing `extern` decl of `ulCriticalNesting` in the port — all done idempotently by `patch_arduinopico.py`. Core 1 is now left under Pico SDK reset and will be launched separately by the M1.0b Apache-2.0 boot image. Upstream fix would be to (a) provide an `extern` decl of `ulCriticalNesting` in single-core mode in `portmacro.h`, (b) drop `static` from `ulCriticalNesting` in `port.c`, and (c) either make `freertos-main.cpp`/`freertos-lwip.cpp` compile under `configNUMBER_OF_CORES==1` or gate the SMP-only bits. |
| P2-3 | 2026-04-11 | `multicore_launch_core1_raw` in `initVariant()` | On cold reset, by the time Core 0 reaches `initVariant()` Core 1 is **not** in the clean bootrom FIFO handler that `multicore_launch_core1_raw` expects — the four-word launch handshake never completes and Core 0 hangs inside `_raw` (debug breadcrumb stuck at phase `0x12`). Candidates for the disturbance: Arduino-Pico's early `rp2040.fifo.begin(2)`, `multicore_doorbell_claim_unused`, or Pico SDK `runtime_init_per_core_bootrom_reset`. Not yet isolated. | Workaround: call `multicore_reset_core1()` immediately before `multicore_launch_core1_raw()` in `initVariant()`. This asserts `PSM_FRCE_OFF_PROC1` and returns Core 1 to the bootrom handler (same mechanism as Arduino-Pico's `restartCore1()`), after which the handshake completes instantly and Core 1 starts executing our Apache-2.0 bootspike image at `0x10200000`. Safe — mirrors an existing framework path — but root cause investigation deferred to M1.1 when the Core 0 boot path is fully instrumented. |
| P2-4 | 2026-04-12 | `ipc_shared_layout.h` comment + legacy breadcrumb placement (M1.0b / M1.1-A) | The header comment claims `0x20078000..0x2007A000` is "preserved untouched" as a secondary SWD debug channel. This is **wrong** — Core 0's patched `memmap_default.ld` sets `RAM(rwx) : ORIGIN = 0x20000000, LENGTH = 512k - 0x6000` = `0x20000000..0x2007A000`, so the window is inside Core 0's FreeRTOS heap / task stack region. M1.0b + M1.1-A both used `0x20078000` for Core 1 sentinel + breadcrumbs and it worked by luck — at those phases Core 0's heap pressure was low enough that the sentinel bytes were never reached. M1.1-B triggered the collision: once Meshtastic task stacks + heap allocations reached the window, the breadcrumb region was overwritten with code pointers (observed `0x1002xxxx` / `0x2000xxxx` values instead of the `0xC1B01200` sentinel). | M1.1-B breadcrumbs moved to the last 64 B of `.shared_ipc` (`0x2007FFC0..0x20080000`), which lives inside `g_ipc_shared._tail_pad` — both cores' linker scripts reserve this range as NOLOAD shared SRAM and ring traffic never touches it. The M1.0b / M1.1-A legacy breadcrumbs at `0x20078000..0x2007801C` (initVariant phase marker + Core 1 SP/entry capture) are still used by Core 0 `variant.cpp` on cold boot before heap grows; those reads are valid for the first ~100k NOPs after `multicore_launch_core1_raw` returns. `ipc_shared_layout.h` comment should be rewritten to reflect reality when the header is next touched. |
| P2-5 | 2026-04-12 | M1.1-B USB CDC bridge — intermittent message loss via Meshtastic web console | **Symptoms (observed during post-M1.1-B smoke test):** (a) Sending a text message from Mokya out over LoRa works **sometimes** — neighbouring nodes receive it — but other times the send silently fails with no error surfaced in the web console. (b) Neighbouring nodes sending TO Mokya: the sender's device UI shows "delivered" (ACK received), and Core 0 Meshtastic presumably processed the packet, but the message never appears in the Meshtastic web console text view. One-way `meshtastic --info` protobuf round-trip (M1.1-B close-out test) still works — so the bridge is not totally broken, only lossy under sustained traffic. **Hypotheses (ranked):** (1) `bridge_task` bounded-retry drop-burst at `main_core1_bridge.c:131-168` — when TinyUSB TX FIFO is full for 10 ticks (≈10 ms) the remainder of the ring-slot payload is dropped. If the web console's WebSerial reader isn't draining fast enough, protobuf frames get truncated mid-stream and Meshtastic's web client silently discards partial packets. (2) c1_to_c0 single-slot push from `bridge_task:158-176` reads up to 256 B with `tud_cdc_read` and pushes as one `IPC_MSG_SERIAL_BYTES` slot — if the host sends a frame >256 B in one USB transaction, it may be chunked across reads in a way Core 0's `IpcSerialStream::read` doesn't reassemble cleanly. (3) `usb_device_task` 1-tick (1 ms) yield between `tud_task()` calls — under WebSerial burst traffic, 1 ms may be too slow and endpoint FIFOs back up. (4) FreeRTOS heap fragmentation in the Core 1 image under sustained traffic. **Verification plan (next session, M1.1-B sign-off):** Instrument `bridge_task` with counters for (a) drop-burst events (FIFO stall timeout hit), (b) c0_to_c1 ring overflow count, (c) c1_to_c0 ring overflow count, (d) max observed `tud_cdc_write_available()` deficit. Place counters at `0x2007FFE0+` in `.shared_ipc` tail. Then reproduce the lossy scenarios over web console while watching counters via SWD. Fix strategy depends on which counter moves: if drop-burst, raise retry budget or move to blocking-with-watchdog; if ring overflow, extend slot count or coalesce; if neither, suspect CDC endpoint latency and tune `usb_device_task` yield. **Status:** Theoretical risk confirmed (pop-before-delivery defect exists in code). However, the observed symptom (`meshtastic --info` timeout) was actually caused by P2-8 (CLI version mismatch 2.3.11 vs 2.7.21). M1.1-B baseline works correctly with CLI 2.7.8. Pop-before-delivery fix (M1.2 Part A) remains a hardening improvement for CDC backpressure scenarios but is no longer a blocker. |
| P2-7 | 2026-04-12 | Core 0 ISR stack overflow into breadcrumb area | SWD reads at `0x2007FFC0` show Core 0 SRAM/code pointers (`0x2000F5E4`, `0x100419E8`) instead of the expected sentinel `0xC1B01200`. Core 0 MSP starts at `0x20082000`, SCRATCH_X+Y = 8 KB ends at `0x20080000` — deep ISR nesting or large stack frames overflow into `0x2007FFxx` (the `.shared_ipc` tail where breadcrumbs live). Breadcrumb corruption is cosmetic (counters still readable via offsets), but a deep stack overflow could eventually reach ring control structures at lower addresses. | **Fixed (2026-04-13).** Root cause was P2-12 (recursive `detachInterrupt` panic consuming 34 KB of ISR stack). With P2-12 fixed, normal ISR stack usage is ~184 bytes — well within the 8 KB SCRATCH region. Added Cortex-M33 `MSPLIM` hardware stack guard in `variant.cpp:initVariant()`: `MSPLIM = 0x20080000` (bottom of SCRATCH_X). Any future MSP overflow triggers UsageFault (STKOF) instead of silently corrupting the IPC region. Also enabled UsageFault + MemManage fault handlers via `scb_hw->shcsr` for cleaner SWD diagnosis. SWD verified: `MSPLIM = 0x20080000`, `MSP = 0x20081F48`, breadcrumb sentinel `0xC1B01200` at `0x2007FFC0` intact. Fault injection test: SWD wrote `MSPLIM = 0x20082000` (above MSP) → SysTick entry triggered UsageFault, IPSR=6, CFSR=`0x00100000` (UFSR.STKOF), MSP clamped at `0x20082000` — IPC region untouched. Reset → normal operation restored, `meshtastic --info` confirmed working. |
| P2-8 | 2026-04-12 | CLI version mismatch — false regression in all M1.2 testing | System PATH resolved `meshtastic` to Python 3.11 (Microsoft Store) version **2.3.11**. Active Python 3.13 has `meshtastic` **2.7.8** (via `python -m meshtastic`). Firmware is **2.7.21** — protobuf schema between 2.3.x and 2.7.x is incompatible. All `meshtastic --info` tests returned "Timed out waiting for connection completion" regardless of bridge code changes. Discovered when stock Pico2 on COM7 also timed out with `meshtastic` but succeeded with `python -m meshtastic`. MokyaLora dual-image then also confirmed working with correct CLI — 64 nodes, full config, `pioEnv: "rp2350b-mokya"`. An entire M1.2 debugging session (staged-delivery, drain mode, taskYIELD experiments) was wasted chasing a phantom regression. | Use `python -m meshtastic` for all future testing. Uninstall or deprioritize the Python 3.11 meshtastic package. |
| P2-9 | 2026-04-12 | 2.7.21 mokya variant HardFault on first boot (empty flash / no LittleFS) | **Symptom:** After a full `erase` via J-Link (which wipes the entire 16 MB flash including LittleFS filesystem area), flashing the 2.7.21 mokya variant ELF results in an immediate HardFault on boot. CFSR=0x00008201 (IACCVIOL + IBUSERR), MMFAR/BFAR=0x2008204C — 76 bytes beyond SRAM end (0x20082000). The crash occurs during Meshtastic filesystem initialization when LittleFS finds no valid superblock. **Comparison:** The older 2.7.15 mokya variant ELF does **not** have this bug — it successfully initialises a fresh LittleFS filesystem on empty flash and boots normally. **Recovery procedure:** (1) Flash old 2.7.15 ELF via J-Link → boots and creates LittleFS filesystem. (2) Flash 2.7.21 ELF on top → finds existing filesystem and boots normally. **Root cause:** Not yet investigated. Likely a regression in the Meshtastic LittleFS init path between 2.7.15 and 2.7.21, or a mokya variant config change that triggers an uninitialised pointer / unbounded stack allocation during filesystem format. | **Resolved (2026-04-13).** Root cause = P2-11: `LittleFS.begin()` → `lfs_format()` → `lfs_flash_erase()` → `flash_range_erase()` runs with XIP disabled but interrupts still enabled (Arduino-Pico's `#ifndef __FREERTOS / noInterrupts()` is skipped, `rp2040.idleOtherCore()` is no-op in single-core mode). SysTick fires during XIP-off → IBUSERR. P2-11's `flash_safety_wrap.c` (`--wrap=flash_range_erase/program`) intercepts all callers including LittleFS format. **Evidence:** (1) Breakpoint on `__wrap_flash_range_erase` (`0x20000168`) hit during first-boot format — LR=`0x100233F7` (`lfs_flash_erase`), R0=`0x0037F000` (FS offset), R1=`0x1000` (block size). (2) Negative test: removed `--wrap` flags, full erase + flash → **HardFault** reproduced — IPSR=3, CFSR=`0x00000100` (IBUSERR), HFSR=`0x40000000` (FORCED), R8=`0x0037F000`, R9=`0x00001000` (flash erase params preserved in registers). (3) Positive test: with `--wrap`, full erase + flash → first boot succeeds, `meshtastic --info` returns `nodedbCount: 1` (fresh DB). No additional code changes required. |
| P2-10 | 2026-04-12 | Config change via Web Console hangs COM port | **Symptom:** After modifying node settings in the Meshtastic web console, the device attempts to reboot (via `Power::reboot()` → `rp2040.reboot()` → `watchdog_reboot(0, 0, 10)`). The watchdog hard-resets the entire chip including the USB controller without calling `tud_disconnect()` first. Windows sees the USB device vanish abruptly — the COM port handle enters an error state and the Web Console WebSerial connection hangs indefinitely. After the chip restarts and Core 1 re-enumerates USB CDC, the old COM port handle is stale and the user must close/reopen the browser tab or manually reconnect. **Root cause:** `rp2040.reboot()` performs no USB disconnect — it directly arms the watchdog and spins. In native Arduino-Pico builds (single-core, framework-managed USB), the framework's boot-time `SerialUSB` init happens early enough that Windows re-associates the same COM port seamlessly. In our dual-core architecture, Core 1's manual TinyUSB init occurs later (after `multicore_launch_core1_raw` + FreeRTOS scheduler start), and the re-enumeration timing differs from what the host driver expects. | **Fixed in M2 Part B (2026-04-13).** Core 0 `RebootNotifier` pushes `IPC_MSG_REBOOT_NOTIFY` via ring + doorbell → Core 1 `bridge_task` receives it, calls `tud_disconnect()`, idles until watchdog fires. COM port re-enumerates cleanly after reboot. Verified with `meshtastic --set device.role ROUTER` → reboot → COM16 re-appears → `--get device.role` confirms persistence. |
| P2-11 | 2026-04-12 | Flash write HardFault — `flash_range_erase`/`program` runs with XIP off, interrupts enabled, Core 1 active | **Symptom:** `meshtastic --set` (any config change) triggers a HardFault on Core 0. CFSR=0x00000101 (IACCVIOL+IBUSERR) at `isr_systick` entry (0x1002B61C). PSP frame PC=0x292 (bootrom — inside ROM flash-erase function). Device stuck in HardFault with no watchdog recovery (crash occurs before `Power::reboot()` arms the watchdog). **Root cause chain:** Meshtastic config save → `EEPROM.commit()` → Arduino-Pico `EEPROM.cpp:121-130`. The framework guards flash writes with `#ifndef __FREERTOS / noInterrupts()` and `rp2040.idleOtherCore()`. On MokyaLora both guards are no-ops: (1) `__FREERTOS` is defined → `noInterrupts()` skipped, (2) `_multicore` is false (single-core FreeRTOS) → `idleOtherCore()` returns immediately. The framework assumes "has FreeRTOS → `__freertos_idle_other_core()` handles everything", but our single-core FreeRTOS + independent Core 1 architecture falls through both guards. Result: `flash_range_erase()` calls ROM `flash_exit_xip()` which disables XIP, then SysTick fires (1 ms tick, still enabled), hardware fetches `isr_systick` from flash (0x1002B61C) → IACCVIOL because XIP is off. Independently, Core 1 is still executing from flash during the XIP-off window — its instruction fetches also fail. **Why standard Meshtastic is unaffected:** (a) SMP mode: `_multicore=true` → `idleOtherCore()` actually pauses Core 1 and disables interrupts. (b) Single-core without FreeRTOS: `_multicore=false` but `__FREERTOS` not defined → `noInterrupts()` is called, protecting the flash write. MokyaLora is the only configuration where both paths are skipped. **Affected code paths:** Any flash write — `EEPROM.commit()`, LittleFS write, Preferences save. `meshtastic --info` (read-only) is safe. **SWD evidence captured:** MSP=0x20081F28, stacked xPSR IPSR=0x0F (SysTick), stacked PC=0x1002B61C (`isr_systick` entry), stacked LR=0xFFFFFFED (EXC_RETURN to Thread/PSP). PSP frame PC=0x292 (bootrom flash function). HFSR=0x40000000 (FORCED). | **Fixed in M2 (2026-04-13).** Linker `--wrap=flash_range_erase` + `--wrap=flash_range_program` intercepts all flash write callers. `flash_safety_wrap.c` parks Core 1 via shared-SRAM `flash_lock` protocol + `IPC_FLASH_DOORBELL`, disables Core 0 interrupts, then calls `__real_flash_range_*`. Core 1's `flash_park_handler()` (RAM-resident) ACKs parked, disables all interrupts, WFE-spins until lock cleared. Handles c1_ready==0 (first boot) by skipping park. Verified: `meshtastic --set device.role ROUTER` completes without HardFault. RAM cost: +136 bytes. |
| P2-12 | 2026-04-13 | LoRa RX HardFault — `detachInterrupt` ISR-unsafe under FreeRTOS | **Symptom:** Device HardFaults on LoRa message reception. CFSR=0x00020001 (INVSTATE+IACCVIOL), MSP=0x20079F30 (34 KB stack overflow from `__StackTop`). Crash inside IO_IRQ_BANK0 (SX1262 DIO1). **Root cause:** `RadioLibInterface::isrLevel0Common()` → `disableInterrupt()` → `clearDio1Action()` → `detachInterrupt(29)`. Arduino-Pico's `detachInterrupt()` acquires `CoreMutex(&_irqMutex)` → `__get_freertos_mutex_for_ptr` lazy-creates FreeRTOS semaphore → `pvPortMalloc` uses critical section → `vPortExitCritical` detects ISR context → `rtosFatalError` → `panic` → `puts` → recursive malloc (stdout lock also needs lazy init) → ~177 recursive iterations → stack overflow through SCRATCH + shared IPC → corrupted function pointer → INVSTATE HardFault. | **Fixed (2026-04-13).** `patch_arduinopico.py` patches `wiring_private.cpp`: in ISR context (`portCHECK_IF_IN_ISR()`), `detachInterrupt` calls `_detachInterruptInternal` directly, skipping `CoreMutex`. Patch marker: `MOKYA_ISR_DETACH_PATCH`. Verified: LoRa reception works without crash. |
| P2-13 | 2026-04-13 | XIP cache disabled — 30–85× slowdown on Core 0 instruction fetch | **Symptom:** 12–50 ms frame-to-frame gaps during `want_config_id` → `FromRadio` burst. Stock Pico2 (native SerialUSB) achieves 0–3 ms. DWT CYCCNT profiling showed `getFromRadio()` consuming 750K–5.8M cycles per frame (5–39 ms at 150 MHz). Calibration loop: ~690 cycles/iter vs expected ~7 → ~100× instruction fetch slowdown. **Root cause:** XIP_CTRL register at 0x400C8000 reads 0x00000000 — both EN_SECURE (bit 0) and EN_NONSECURE (bit 1) are cleared, disabling the RP2350's 4 KB XIP cache. All instruction fetches go to QSPI flash at 37.5 MHz (QMI CLKDIV=2, sys_clk=150 MHz). The cache is cleared during boot by the PSRAM detection flow and/or ROM functions — `psram_detect()` enters QMI direct mode which resets XIP_CTRL, and Pico SDK boot2 only restores QMI configuration (M0_TIMING, M0_RCMD, M0_RFMT), never XIP_CTRL. Additionally, every `flash_range_erase/program` call goes through ROM `flash_exit_xip()` which clears XIP_CTRL, and `flash_enable_xip_via_boot2()` (boot2 copyout) does not restore the cache enable bits. This is a Pico SDK gap — boot2 should restore XIP_CTRL but doesn't. Stock Pico2 boards are less affected because native SerialUSB masks the latency. **Investigation steps:** (1) DWT CYCCNT v1/v2 profiling of `writeStream()` loop confirmed 98.5% of time in `getFromRadio()`, 1.5% in `emitTxBuffer()`; (2) `-DDEBUG_MUTE` (compile-time LOG suppression) gave ~45% improvement but early frames still 10–40 ms; (3) FreeRTOS task analysis ruled out preemption — only CORE0(pri 4), Timer Svc(7), Idle(0); (4) SWD register reads: PLL confirmed 150 MHz, QMI at 37.5 MHz, XIP_CTRL=0x00000000. | **Fixed (2026-04-13).** Two-site fix: (1) `variant.cpp:initVariant()` — unconditionally sets EN_SECURE + EN_NONSECURE via XIP_CTRL SET alias (0x400CA000) at boot, before any Meshtastic code runs. (2) `flash_safety_wrap.c` — after each `__real_flash_range_erase/program` call, re-enables cache via `MOKYA_XIP_CTRL_SET = 0x03`. **Results (LOG enabled, no DEBUG_MUTE):** XIP_CTRL=0x00000003, calibration 7.2 cycles/iter (96× better), per-frame gap 0–1.6 ms (was 12–50 ms), burst 37 ms / 47 frm (2.5× faster than stock Pico2), `--info` 15.0 s → 4.5 s. Behavior now matches stock Pico2 including ~20 ms tail-frame LOG spikes. |
| P2-14 | 2026-04-18 | Core 1 `i2c_set_baudrate_core1()` — FS counters + `sda_hold` uninitialised | **Symptom:** All four sensor-bus devices (LIS2MDL 0x1E, Teseo-LIV3FL 0x3A, LPS22HH 0x5D, LSM6DSV16X 0x6A) NACK from cold on Core 1, even though the same i2c1 peripheral reaches 0x36 + 0x6B on the power bus. User-confirmed hardware works with the Core 0 bringup firmware. **Root cause:** Our manual override only wrote the standard-speed counters (`ss_scl_hcnt/lcnt`) and set `IC_CON.SPEED=STANDARD`, leaving `fs_scl_hcnt/lcnt` and `sda_hold` at whatever `i2c_init()` computed with `clock_get_hz(clk_peri)=0` on Core 1 (garbage — both 0, sda_hold min 1 cycle). Power bus tolerated the marginal timing (short traces, few devices); sensor bus did not. A power-first scan also corrupted the peripheral so that a subsequent sensor scan never recovered — confirmed by reversing order (both buses then 0 ACK). | Rewrote `i2c_set_baudrate_core1()` to mirror the SDK: populate both FS and SS counters for the chosen period, compute `sda_tx_hold_count` from the fixed 150 MHz, pick `SPEED=FAST` for any baud > 100 kHz. Added full `i2c_init()` + baud re-apply inside `switch_pinmux()` so pinmux changes never leave i2c1 in a half-configured state. Default baud lifted 100 kHz → 400 kHz (matches bringup). Scan now finds all six devices; LPS22HH reports 1008 hPa / 31.9 °C. |
| P2-6 | 2026-04-12 | M1.1-B USB CDC bridge — slow Meshtastic web console initial handshake | **Symptom:** When the Meshtastic web console attaches to COM16 (MokyaLora via Core 1 bridge), the initial device-configuration handshake (`want_config_id` → full `FromRadio` stream: MyNodeInfo, Metadata, Channel×8, Config×, ModuleConfig×, NodeInfo×86) takes **noticeably longer** than the same firmware build running with Arduino-Pico's native `SerialUSB` (no bridge). One-shot `meshtastic --info` via `pyserial` also feels slower than a reference Meshtastic device but is tolerable; web console's dozens of small-packet exchanges amplify the per-round-trip penalty. **Hypotheses (ranked):** (1) **`usb_device_task` 1-tick yield (`vTaskDelay(pdMS_TO_TICKS(1))`) at `main_core1_bridge.c:97`** — between every `tud_task()` poll. Full-speed USB bulk-IN polling interval is 125 µs, so a 1 ms quantum means the TX endpoint only has ~8 opportunities/ms to push out data, and every request→response round trip eats at least 1 ms on the USB side. (2) **`bridge_task` idle 1-tick yield at `main_core1_bridge.c:192`** — when both directions drain empty, we sleep 1 ms before re-checking. Web console's synchronous request/response protocol means every host→device→host round trip adds 1–2 ms of bridge latency on top of the USB wire time. (3) **No FreeRTOS task notification on ring push** — Core 0's `ipc_ring_push` writes the slot then releases `head`, but does not wake Core 1's `bridge_task`. Core 1 only sees the new slot on its next idle-yield wake-up. Similarly, CDC OUT data only triggers `bridge_task` wake-up 1 ms later. A cross-core sev / FIFO doorbell + `xTaskNotifyFromISR` would let the bridge react within tens of µs. (4) **`tud_cdc_write` micro-chunking** — the `bridge_task` splits each ring slot across `tud_cdc_write_available()` chunks; if avail is small and we re-loop with a 1-tick yield, a single 256 B ring slot can cost multiple ms. **Verification plan:** Measure end-to-end handshake time with `meshtastic --port COMxx --info` scripted against (a) native `SerialUSB` reference build on the same board, (b) current M1.1-B bridge, (c) M1.1-B bridge with `usb_device_task` switched to `taskYIELD()` instead of `vTaskDelay(1)`, (d) M1.1-B bridge with FreeRTOS task-notify on ring push (requires cross-core notify via RP2350 SIO FIFO IRQ). Pick the minimal change that matches (a). **Status:** Confirmed by benchmark (2026-04-12). With correct CLI (2.7.8): bridge ~0.50 KB/s vs native ~12.6 KB/s (**~25× slower per-KB**). Zero drops. Root cause is dual 1 ms vTaskDelay, not the bridge protocol itself. M1.2 Part B (`taskYIELD()` replacement) is the next fix. Cross-core notification deferred to M2. |
| P2-16 | 2026-04-22 | Flash CLKDIV=1 (75 MHz) has ~10⁻⁵ transient cache-fill error rate on W25Q128JW 1.8 V + this Rev A routing — ship at CLKDIV=2 (37.5 MHz) | **Symptom:** `flash_bench` at bootrom-default M0 (CLKDIV=3 / RXDELAY=2) gives ~20.5 MB/s uncached & cached (cache barely helps at 25 MHz because cmd+addr overhead dominates). `flash_sweep2` at CLKDIV=1 (75 MHz) FAILs across all 8 RXDELAY values with deterministic nibble-shifted reads. PSRAM on the same QMI bus (shared SCK / SD[3:0]) runs fine at 75 MHz (P2-15) — flash is specifically the problem. **Investigation in three passes on 2026-04-22:** **Morning — register + pad sweeps:** (1) 36-combo `flash_sweep2` (DUMMY 16/20/24/28 × SUFFIX none/0xA0/0xF0 × RXDELAY 1/2/3) all FAIL → not a register-level timing issue. (2) W25Q128JW non-DTR does NOT support full QPI (4-4-4). Opcode 0x35 is "Read Status Register-2" on W25Q, not "Enter QPI" (APS6404L conventions ≠ W25Q). (3) 1-1-4 mode (6Bh) at 75 MHz: same shift signature. (4) Flash SR3 DRV 25 %→100 %: no effect. (5) RP2350 QSPI pad defaults (DRIVE=4 mA, SLEWFAST=0, SCHMITT=1) aren't optimised for 75 MHz — boot2 doesn't run on RP2350. First `flash_pad_ablation` (2³ factorial, one-word sentinel) reported "SLEWFAST=1 alone is enough". Shipping that HardFaulted production in vStartFirstTask. (6) `flash_deep_scan` (full 16 MB block-XOR uncached): SLEWFAST-only = 120/256 bad → one-word check was a false positive. (7) `flash_deep_ablation` across 4 pad combos: **SLEWFAST + DRIVE=8 mA = 0/256 (both SCHMITT on and off); SLEWFAST alone = 120**. (8) Production with DRIVE=8 mA + SLEWFAST + CLKDIV=1 + RXDELAY=2 still HardFaulted in vStartFirstTask, despite bringup reporting 0 errors at the same register state. **Afternoon — cached-burst oracle + 2×2 factorial:** Added `flash_deep_scan_cached` / `flash_deep_ablation_cached`: mirror the uncached versions but read through `XIP_BASE` with per-64 KB-block `xip_cache_invalidate_range()`, so every word cold-misses and triggers a fresh 8-byte cache-line burst fill at the candidate timing. (Gotcha: the first implementation self-destructed by calling `to_ms_since_boot(get_absolute_time())` inside the CLKDIV=1 window — `timer_time_us_64` at flash offset `0x18940` lived in a block we'd just invalidated and re-filled with CLKDIV=1-corrupted data; the next call fetched poisoned instruction bytes and crashed with IACCVIOL. Rewrote both `*_cached_run` functions to use `timer_hw->timerawl` directly and avoid all flash-resident calls between the M0 switch and the post-restore `xip_cache_invalidate_all()`. Reusable lesson: poisoned cache lines from bad-timing fills persist until explicit invalidation.) Cached ablation reports **identical bad-block counts to the uncached ablation** — cached-vs-uncached oracle gap hypothesis refuted. Completed CLKDIV × pad 2×2 factorial on production: CLKDIV=2 + default pad ✓, CLKDIV=2 + 8 mA+SLEWFAST ✓, CLKDIV=1 + default pad ✗ (CFSR=`0x00018201` = IACCVIOL+UNDEFINSTR+PRECISERR), CLKDIV=1 + 8 mA+SLEWFAST ✗ (CFSR=`0x00008200` precise data-bus fault; SWD-verified cache line `0x1002AEA8` poisoned — CPU's cached `ldr r3,[pc,#80]` returned `0xF3EFB943`, direct SWD read gives `0x2000D81C`). Pad boost reduces corruption severity (fewer downstream faults) but doesn't eliminate it. Ran CLKDIV=1 + 8 mA+SLEWFAST **single-core** (Core 1 launch suppressed) — still HardFaults, refuting dual-core concurrent fetch as the cause. **Night — transient-error-rate measurement in bringup:** Added `flash_rand_scan_cached` (100 k random-address LCG, per-sample cache invalidate, IRQ-off). At CLKDIV=1 + 8 mA+SLEWFAST: **0/100 mismatch buckets**. Extended to `flash_rand_scan_long` (1 M samples, same config): **7–9 / 100 mismatch buckets, first-bad at bucket #37–38 across 4 repeat runs with the same seed**. Count drifts slightly (7, 8, 8, 9) with identical address sequence → errors are semi-deterministic: part stable margin violation on specific address transitions, part sporadic transient. **Quantified error rate ≈ 10⁻⁵ per cache-line burst fill.** Bringup's previous oracles never exposed this because 16 MB linear scan = 2 M fills and 100 k random = 10⁵ fills — both below / right at detection threshold for this rate. Production boot + FreeRTOS setup does ~10⁵–10⁶ fills within seconds, guaranteeing a hit; the vStartFirstTask HardFault is exactly that rate's expected value. **Device context:** W25Q128JW is the 1.8 V variant; Pico 2 stock uses 3.3 V W25Q32RV, Pimoroni Pico Plus 2 uses 3.3 V W25Q128JV — both CLKDIV=2 / 75 MHz reliably. The 1.8 V + trace-length + pad-drive combination on this Rev A sits at the setup/hold margin at 75 MHz. | **Shipped (2026-04-22): CLKDIV=2 / 37.5 MHz via `flash_retime_m0()` in `variant.cpp`.** +50 % over bootrom raw SCK, `meshtastic --info` ~4.5 s (unchanged — CDC-bound, not flash-bound), both cores IPSR=000 persistently across repeated CDC interaction. Pads left at bootrom defaults (4 mA) — 2×2 factorial confirmed pad boost is unnecessary at 37.5 MHz. Helper runs from RAM with direct-mode bracket + RAM-only `mokya_xip_cache_invalidate_all()` (open-coded since `hardware/xip_cache.h` isn't on Arduino-Pico's variant include path). **Bringup tooling landed:** `flash_bench`, `flash_sweep2`, `flash_pad_ablation`, `flash_deep_scan`, `flash_deep_ablation`, `flash_deep_scan_cached`, `flash_deep_ablation_cached`, `flash_rand_scan_cached`, `flash_rand_scan_long`, `flash_reset`, `flash_boost_pads`, `qmi_diag`. **Key test-infrastructure lesson (carry forward):** oracle sample count must dominate (worst-acceptable error rate × safety factor). A ~10⁻⁵ defect rate needs ≥ 10⁶ samples (ideally 10⁷) to detect reliably; smaller runs produce false-clean results that look like green lights. **Rev B recommendations:** (a) switch to 3.3 V W25Q128JV if pin-compatible, or (b) shorten QSPI traces + improve return-path grounding. Either path should unlock the +117 % cached flash read gain (20.5 → ~44 MB/s) that bringup proved is register-reachable. **Rev B validation gate:** `flash_rand_scan_long` must report 0 mismatches over 1 M samples on the new board (multiple runs, multiple seeds) before enabling CLKDIV=1 in production. |
| P2-19 | 2026-04-25 | Meshtastic `PhoneAPI::getFromRadio()` heartbeat path corrupts every reply — `LOG_DEBUG` between encode and return clobbers the encoded payload via the shared `txBuf`/`fromRadioScratch` | **Symptom:** After P2-17 (IPC truncation) and P2-18 (RTC epoch clobber) were fixed, `client.meshtastic.org` Settings panel still spun forever (config never finished loading). Reproduced via `python -m meshtastic --port COMxx --info` — 4 `Error parsing message with type 'meshtastic.protobuf.FromRadio'` per session, deterministic across runs. **Investigation:** Captured raw COM stream during `--info`, walked `0x94 0xC3 LEN_HI LEN_LO`-framed packets, found one bad frame per run with declared `LEN=6` and payload `32 40 0A 2D 46 72`. Decoded as FromRadio, this is field 6 (`log_record`) wire-2 length 64 — but only 4 payload bytes follow, so protobuf parse fails. Looked at the preceding good frame: a 66-byte `log_record` with message `"FromRadio=STATE_SEND_QUEUE_STATUS, numbytes=6"`. Searched the source: `firmware/core0/meshtastic/src/mesh/PhoneAPI.cpp:226–237`, the heartbeat fast-path: `pb_encode_to_bytes(buf, ...); LOG_DEBUG("FromRadio=STATE_SEND_QUEUE_STATUS, numbytes=%u", numbytes); return numbytes;`. **Root cause:** When the StreamAPI subclass has switched to protobuf log encapsulation (`SerialConsole::usingProtobufs == true` after `want_config`), `LOG_DEBUG` re-enters `StreamAPI::emitLogRecord` (`SerialConsole.cpp:142–149`). `emitLogRecord` shares `txBuf` and `fromRadioScratch` with the very `getFromRadio` flow above it: it `memset`s `fromRadioScratch`, encodes a 66-byte `log_record` over the same `txBuf+HEADER_LEN` that just held the queue_status, and emits a clean log_record frame. Control returns to `writeStream()` (`StreamAPI.cpp:53–54`), which then calls `emitTxBuffer(numbytes)` with the original `numbytes=6` and the now-clobbered `txBuf` — emitting `0x94 0xC3 0x00 0x06` followed by the first 6 bytes of the leftover log_record encoding (`32 40 0A 2D 46 72`). Wire pattern: clean log_record + corrupted-trailer queue_status, with 4 different bad-frame sites visible per session because heartbeat exchanges happen multiple times during config sync. **Why upstream knows but missed it:** the same file has an explicit comment at line ~584 ("VERY IMPORTANT to not print debug messages while writing to fromRadioScratch — because we use that same buffer for logging when we are encapsulating with protobufs"). The switch-statement path obeys the rule; the heartbeat fast-path was added later and didn't. **Why the settings page spins specifically:** `client.meshtastic.org` enters Settings by issuing a sequence of `get_config` admin requests that each pair a heartbeat with a config response. Every heartbeat triggers a corrupted queue_status frame; the host parser drops the bad frame and waits for missing config segments that already left the wire as part of a corrupted-then-discarded frame. UI never gets a complete config tree. **Diagnostic dead ends ruled out:** (a) Log-ring → CDC interleaving (the original suspect for the same symptom): tested by dropping log ring forwarding entirely on Core 1 — corruption persisted, so the byte-by-byte log path was not the source. (b) An attempted fix that gave `emitLogRecord` its own static `logScratch` + `logTxBuf` regressed Core 0 to RX=6 bytes total (Core 0 stopped sending most data) — root cause not investigated; reverted in favour of the simpler upstream-spirit fix below. | **Patched (2026-04-25)** in `firmware/core0/meshtastic` submodule: move `LOG_DEBUG` to before `pb_encode_to_bytes` in `PhoneAPI::getFromRadio()`'s heartbeat path; drop the `numbytes` value from the message text (cosmetic loss). The reordering means any re-entrant `emitLogRecord` clobbers `txBuf`/`fromRadioScratch` *before* the queue_status encode, which then writes its own bytes cleanly and `writeStream()` emits the correct payload. **Defense in depth:** kept the Core 1 log-ring → CDC drop from the earlier diagnostic round (`main_core1_bridge.c`). Original architecture forwarded byte-by-byte log text alongside protobuf frames on the same CDC IN endpoint; with `usingProtobufs == true` (the post-`want_config` default) the byte-by-byte path is dead, so the drop is functionally a no-op today, but it eliminates a future re-introduction risk and forces all log output onto the structured `log_record` channel. **Verification:** 3 consecutive `--info` runs reported `errors=0 size=68 lines` (versus 4 errors / 85 lines before), nodedbCount populated, no parse failures in the captured raw stream. Settings-page spin should now resolve — needs user-side confirmation in the browser. |
| P2-18 | 2026-04-25 | Meshtastic `perhapsSetRTC()` self-clobbers freshly-set epoch on platforms with no RTC chip and no `settimeofday()` (e.g. RP2350) | **Symptom:** After confirming P2-17's stream-corruption fix, the Meshtastic web console (`client.meshtastic.org`) still showed every neighbour node's `last_heard` and message timestamps stuck at 1970 (epoch 0/small uptime values). Sending an explicit setTime via `python -m meshtastic` likewise had no lasting effect — `getValidTime()` returned ~boot uptime instead of the host wall clock. **Investigation:** SWD readback of static globals in `gps/RTC.cpp` after a successful Python `iface.localNode.setTime(int(time.time()))`: `currentQuality = 0x03` (NTP, set correctly), but `zeroOffsetSecs = 575` and `timeStartMsec = 575618 ms` — both ≈ uptime since boot, **not** the 1.77 G epoch the host pushed. Symbol addresses were verified via `arm-none-eabi-nm` (`_ZL14zeroOffsetSecs` at `0x20001858`, `_ZL13timeStartMsec` at `0x20008B00`, `_ZL14currentQuality` at `0x2000D5A3`). **Root cause:** `perhapsSetRTC()` (RTC.cpp:171) writes `currentQuality`, `timeStartMsec`, `zeroOffsetSecs = tv->tv_sec` correctly, then unconditionally calls `readFromRTC()` at the end of its success path. `readFromRTC()` walks a chain of `#ifdef RV3028_RTC / PCF8563_RTC / PCF85063_RTC / RX8130CE_RTC` branches; none match on RP2350, so control falls into the `#else` branch (RTC.cpp:149-157) which calls `gettimeofday()` and overwrites `timeStartMsec`/`zeroOffsetSecs` with whatever it returns. Pico SDK's `gettimeofday()` (newlib stub) returns boot uptime, not wall clock — there is no `settimeofday()` shim wired up on Arduino-Pico, so the AON timer / external RTC infrastructure that the rest of `perhapsSetRTC` assumes simply isn't there. Net effect: every successful set is immediately undone, leaving the device permanently at "uptime epoch" while pretending `currentQuality == NTP`. The other RTC-chip branches don't have this bug because they re-read the wall clock from chip and fall through `if (currentQuality == RTCQualityNone)` guards (RTC.cpp:63, 107, 141) before assigning — only the no-RTC `#else` branch was missing the guard. **Why this matters:** without valid `getValidTime()`, every received MeshPacket gets `rx_time = 0` stamped on it (Router.cpp:217, 737, 843, 848), so phone/web clients render every message and every node's last-heard field as Jan 1 1970. Position data also fails to render because Position protobuf is gated on a non-zero `time` field. **Why client.meshtastic.org doesn't auto-fix:** the official phone/Python clients send `AdminMessage.set_time_only` on connect, but `client.meshtastic.org` (at least the version in production on 2026-04-25) does **not** — it only sends time on an explicit user-triggered "Sync" action. So even with this bug fixed, web users will still need either a manual sync, the Python CLI, or GPS-derived time (M5). **Diagnostic dead ends ruled out:** (a) `MESHTASTIC_EXCLUDE_GPS=1` excluding the GPS module — irrelevant, time can come from any quality source. (b) Admin auth (`is_managed`/PKI) blocking the set — confirmed not the cause: `mp.from == 0` local USB admin path is taken, no auth required. (c) `BUILD_EPOCH` rejecting the time — confirmed not the cause; `currentQuality` was successfully upgraded to NTP, only the offset got clobbered after. | **Patched (2026-04-25)** in `firmware/core0/meshtastic` submodule: `gps/RTC.cpp` no-RTC `#else` branch now guards the `timeStartMsec`/`zeroOffsetSecs` assignment with `if (currentQuality == RTCQualityNone)`, matching the pattern of the four RTC-chip branches above it. The function still returns `RTCSetResultSuccess` so callers behave identically, but the fresh higher-quality value set by `perhapsSetRTC()` is no longer overwritten. **Verification:** flashed, connected via Python lib, sent setTime — SWD readback `zeroOffsetSecs = 0x69EC582F = 1777096751` exactly matching the host epoch, `currentQuality = 0x03` (NTP) preserved. **Build-script bug surfaced during verification:** `scripts/build_and_flash.sh` selected the Core 0 ELF via `ls firmware*.elf | head -1`, which is alphabetical and picks the oldest commit-hash suffix (`0ab5ef2` vs current `bf7bb81`) — silently flashed stale firmware for the first verification round and made it look like the patch wasn't taking effect. Fixed by switching to `ls -t` (mtime newest-first). **Status of full UX fix:** time now sticks within a session, but RP2350 has no battery-backed RTC so reboots reset to uptime-zero. Until M5 GPS time integration (`IpcGpsBuf` writer Core 1 → Core 0) lands, web users must manually sync after each reboot — `client.meshtastic.org` does not do this automatically. |
| P2-17 | 2026-04-25 | IPC byte bridge — both directions silently truncated stream-protocol frames mid-payload, desyncing host parser permanently | **Symptom:** Meshtastic web console (`client.meshtastic.org`) showed neighbour nodes' `last_heard` times as garbage and **never displayed remote node positions**. Reproduced with `python -m meshtastic --info`: ~30+ `Error parsing message with type 'meshtastic.protobuf.FromRadio'` per session, but enough frames decoded to populate basic NodeDB fields — explains why some node info shows but PositionInfo / time-related fields are missing. **Investigation:** Captured raw COM bytes after `want_config_id`, walked `0x94 0xC3 LEN_HI LEN_LO`-framed packets, decoded each as `FromRadio` protobuf. Of 134 frames, 9 failed parse — every failed frame contained an **embedded `\x94\xc3` magic header partway through its declared payload**, e.g. frame@2420 declared LEN=136 but had next frame's `\x94\xc3\x00\x62` at offset ~120. Pattern means: declared header LEN > actual bytes on the wire → host reads LEN bytes, gobbles into next frame, alignment lost. **Root cause:** Both directions of the IPC byte bridge had a "give up after timeout, return early" path that aborted **mid-frame**: (1) `firmware/core0/.../ipc_serial_stub.cpp` — `IpcSerialStream::write(buf,len)` chunked into ≤256 B ring slots and `break`ed out of the chunk loop after a 50 ms `ipc_ring_push` timeout, leaving the 4-byte stream-protocol header already in the wire even though the rest of the payload was dropped. (2) `firmware/core1/.../main_core1_bridge.c` — data-ring → CDC IN forwarder `break`ed out of the chunked write after 10 stall_ticks of `tud_cdc_write_available()==0`, dropping the remainder of the current ring slot but having already shipped earlier chunks of the same FromRadio frame. Both paths' comments asserted "Meshtastic retransmits on demand" — that is **wrong** for the stream-protocol layer; there is no frame-level retransmit, only opportunistic application-level resend on missing ACKs. Any mid-frame byte drop permanently desyncs every subsequent FromRadio. The corruption was traffic-dependent: the 100-node DB `want_config` reply pushed enough chunks that backpressure timeouts hit; smaller exchanges (`--noproto`, basic config probes) escaped. **Why it surfaced now:** earlier `--info` smoke tests at M1.2 / M2 had `nodedbCount` of 1–10 and never sustained enough chunked traffic to hit the timeout windows. After Meshtastic accumulated 100 nodes, the want_config reply reliably triggers backpressure on the host CDC FIFO during the burst, exposing the latent drop. **Diagnostic dead ends ruled out:** (a) Core 0 has no time source → benign, web client `set_time_only` admin message would have synced fine if it had reached AdminModule. (b) Log-ring → CDC interleaving on Core 1 → not the cause; corruption persists with log forwarding disabled. (c) Initial `did_work=true` removal experiment starved `usb_device_task` and reduced TX throughput to 6 bytes total — a side-quest; reverted. | **Fixed (2026-04-25).** Both directions changed to never abort mid-frame: (1) `IpcSerialStream::write(buf,len)` — removed 50 ms deadline, now `yield()` until each chunk is pushed; only exit is full success. (2) Core 1 bridge `c0_to_c1` → CDC IN — removed 10-tick stall budget, now `taskYIELD()` until CDC FIFO has room; only exit is `!tud_mounted()` (host gone). **Verification:** raw stream capture: 144/144 frames parse cleanly, 0 embedded magic markers, `nodedbCount=100` `--info` returns full output (1814 lines, 0 parse errors vs 36 before). User can now reconnect web console and confirm timestamps + positions render correctly. **Backpressure rationale:** stream protocol is byte-exact framing; correct response to backpressure is to slow the producer, not to drop bytes. Both bridges run in FreeRTOS context where blocking-with-yield is cheap; the only previous concern was deadlock if the host vanished, which `tud_mounted()` already guards against. |
| P2-15 | 2026-04-20 | Core 1 `psram.c` — PSRAM bit errors from MAX_SELECT=0 (DRAM refresh starvation) | **Symptom:** IME candidate lookup returned wrong suggestions for `ㄅㄆ` / `ㄅㄉ` (shape bytes `\x21\x26` / `\x21\x25`) on device, while host `dict_probe` returned correct candidates from the same MDBL dict. Escalated to a full PSRAM sweep under FreeRTOS: 8 MB uncached round-trip test reports **93.75 % word errors** (1,965,135 / 2,097,152) with consistent signature — byte 0 upper-nibble random bit-flips (e.g. wrote `0xA5000002`, CPU reads back `0xA5000022`, SWD via NOTRANSLATE reads back `0xA5000002` correctly). **Investigation steps:** (1) Isolated Core 1 task concurrency — disabled USB/LVGL/keypad/sensors/GPS, only `ptst` task runs — **still 93.75 %**, ruling out task/DMA interference. (2) Halted Core 0 via J-Link after boot — **still 93.75 %**, ruling out Core 0 M0 flash traffic. (3) Compared cached (0x11xxxxxx) vs uncached (0x15xxxxxx) paths at production default: cached **46.81 %**, uncached 93.75 % — different error rates imply different access patterns matter. (4) Swept QMI CLKDIV × RXDELAY at runtime (via `psram_set_timing_rt` in RAM): only CLKDIV=2/RXDELAY=0 (production default) produced the "least-bad" 93.75 %; all other combos were 100 %. CLKDIV=1 at runtime wedges QMI into direct-mode-stuck state that survives `SYSRESETREQ` + chip erase + `r`-reset — **only USB power cycle recovers**; binary with CLKDIV=1 in the sweep array was enough to trigger the wedge even before the test task ran. **Root cause:** `psram.c`'s `psram_init_run` sets M1.timing as `(m[0].timing & ~(COOLDOWN|RXDELAY|CLKDIV)) | <new bits>`, inheriting `MAX_SELECT`, `MIN_DESELECT`, `PAGEBREAK`, `SELECT_HOLD`, `SELECT_SETUP` from M0 (flash). Arduino-Pico / Pico SDK boot2 leaves `MAX_SELECT = 0` on M0 because NOR flash has no refresh requirement. When inherited onto M1, `MAX_SELECT = 0` lets QMI keep CS asserted indefinitely when servicing back-to-back M1 accesses. APS6404L-SQN-ZR is pseudo-SRAM with DRAM cells and self-managed refresh that only runs while CS is HIGH — datasheet `tCEM = 8 µs` (standard grade) is the max CS-low window; violating it starves the DRAM cells. RP2350 SVD explicitly flags MAX_SELECT as "required to meet timing constraints of PSRAM devices". Bringup standalone firmware passed the same 8 MB test because its access pattern (printf + 4 KB ring) never kept CS low long enough for refresh starvation to manifest; production's tight access loops under XIP cache fill + QMI prefetch do. **Fix:** add `MAX_SELECT = 1` (2-bit field in M1.timing, 1 × 64 sys_clk = 427 ns trigger) to `psram.c` `psram_init_run`. Worst-case CS-low = 256 + 316 (one cache-line burst) ≈ 3.81 µs ≪ 8 µs tCEM. **Validation (2026-04-20):** SWD M1.timing = `0xA0027002` with MAX_SELECT=1 bit set. Full 8 MB sweep at fix: **cached = 0 / 2,097,152 errors across 2 passes (0.00 %)**, uncached = 983,340 / 2,097,152 (46.89 %, tight-loop CPU read pattern). First `first_bad_idx` for cached = `0xFFFFFFFF` (no errors). MIN_DESELECT sweep (7/15/31 at MS=1) had zero effect; only MAX_SELECT matters. **Follow-up fix:** `mie_dict_loader.c` PSRAM_READ_ADDR macro had regressed to `MOKYA_PSRAM_UNCACHED_BASE`, so dict `s_mie_dict` pointers landed at 0x15xxxxxx instead of 0x11xxxxxx — trie tight-loop reads + font glyph lookups hit the 47% uncached error. Restored READ via cached alias (WRITE stays uncached for correct write-through), per the intent already documented in the file header comment. Dict pointers readback after fix: `0x11000000 / 0x11200000 / 0x11500000 / 0x11510000`. |
