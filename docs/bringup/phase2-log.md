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

---

## Issues Log (Phase 2)

| # | Date | Area | Issue | Resolution |
|---|------|------|-------|-----------|
| P2-1 | 2026-04-11 | Meshtastic `AddI2CSensorTemplate.h` | Latent upstream bug — non-dependent `ScanI2CTwoWire` name fails to resolve under `MESHTASTIC_EXCLUDE_I2C=1` due to two-phase template lookup | Workaround: `-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` in Core 0 variant. Upstream fix would be to also guard the `ScanI2CTwoWire*` parameter declaration under `#if !MESHTASTIC_EXCLUDE_I2C`. |
| P2-2 | 2026-04-11 | Arduino-Pico 5.4.4 FreeRTOS SMP on RP2350 | Core 1's passive-idle task HardFaults in `vStartFirstTask` the instant `xPortStartScheduler` launches it, blocking Core 0 from ever reaching `setup()`. CFSR=0x101 (IACCVIOL+IBUSERR), MMFAR=BFAR=0x2000C5AC (inside Core 1's own PSP region). Independent of `-DNO_USB` — architecture change, not a stub bug. | Switched to single-core FreeRTOS (`-DconfigNUMBER_OF_CORES=1`). Requires guarding the SMP-only framework code (`vTaskCoreAffinitySet`, `vTaskPreemptionDisable/Enable`, IdleCoreN task creation) in `freertos-main.cpp` / `freertos-lwip.cpp` and fixing the missing `extern` decl of `ulCriticalNesting` in the port — all done idempotently by `patch_arduinopico.py`. Core 1 is now left under Pico SDK reset and will be launched separately by the M1.0b Apache-2.0 boot image. Upstream fix would be to (a) provide an `extern` decl of `ulCriticalNesting` in single-core mode in `portmacro.h`, (b) drop `static` from `ulCriticalNesting` in `port.c`, and (c) either make `freertos-main.cpp`/`freertos-lwip.cpp` compile under `configNUMBER_OF_CORES==1` or gate the SMP-only bits. |
