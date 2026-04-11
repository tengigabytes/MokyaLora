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

---

## Issues Log (Phase 2)

| # | Date | Area | Issue | Resolution |
|---|------|------|-------|-----------|
| P2-1 | 2026-04-11 | Meshtastic `AddI2CSensorTemplate.h` | Latent upstream bug — non-dependent `ScanI2CTwoWire` name fails to resolve under `MESHTASTIC_EXCLUDE_I2C=1` due to two-phase template lookup | Workaround: `-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` in Core 0 variant. Upstream fix would be to also guard the `ScanI2CTwoWire*` parameter declaration under `#if !MESHTASTIC_EXCLUDE_I2C`. |
| P2-2 | 2026-04-11 | Arduino-Pico 5.4.4 FreeRTOS SMP on RP2350 | Core 1's passive-idle task HardFaults in `vStartFirstTask` the instant `xPortStartScheduler` launches it, blocking Core 0 from ever reaching `setup()`. CFSR=0x101 (IACCVIOL+IBUSERR), MMFAR=BFAR=0x2000C5AC (inside Core 1's own PSP region). Independent of `-DNO_USB` — architecture change, not a stub bug. | Switched to single-core FreeRTOS (`-DconfigNUMBER_OF_CORES=1`). Requires guarding the SMP-only framework code (`vTaskCoreAffinitySet`, `vTaskPreemptionDisable/Enable`, IdleCoreN task creation) in `freertos-main.cpp` / `freertos-lwip.cpp` and fixing the missing `extern` decl of `ulCriticalNesting` in the port — all done idempotently by `patch_arduinopico.py`. Core 1 is now left under Pico SDK reset and will be launched separately by the M1.0b Apache-2.0 boot image. Upstream fix would be to (a) provide an `extern` decl of `ulCriticalNesting` in single-core mode in `portmacro.h`, (b) drop `static` from `ulCriticalNesting` in `port.c`, and (c) either make `freertos-main.cpp`/`freertos-lwip.cpp` compile under `configNUMBER_OF_CORES==1` or gate the SMP-only bits. |
| P2-3 | 2026-04-11 | `multicore_launch_core1_raw` in `initVariant()` | On cold reset, by the time Core 0 reaches `initVariant()` Core 1 is **not** in the clean bootrom FIFO handler that `multicore_launch_core1_raw` expects — the four-word launch handshake never completes and Core 0 hangs inside `_raw` (debug breadcrumb stuck at phase `0x12`). Candidates for the disturbance: Arduino-Pico's early `rp2040.fifo.begin(2)`, `multicore_doorbell_claim_unused`, or Pico SDK `runtime_init_per_core_bootrom_reset`. Not yet isolated. | Workaround: call `multicore_reset_core1()` immediately before `multicore_launch_core1_raw()` in `initVariant()`. This asserts `PSM_FRCE_OFF_PROC1` and returns Core 1 to the bootrom handler (same mechanism as Arduino-Pico's `restartCore1()`), after which the handshake completes instantly and Core 1 starts executing our Apache-2.0 bootspike image at `0x10200000`. Safe — mirrors an existing framework path — but root cause investigation deferred to M1.1 when the Core 0 boot path is fully instrumented. |
