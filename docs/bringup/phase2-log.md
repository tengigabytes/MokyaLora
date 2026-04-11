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
**Result:** ⚠️ PARTIAL — build ✅ PASS, flash + boot verification ⏳ PENDING

**Goal:** Prove Core 0 Meshtastic firmware can build with Arduino-Pico's built-in `SerialUSB` disabled, with the framework `Serial` global replaced by a stub `IpcSerialStream` that will later route bytes through the shared-SRAM SPSC ring to Core 1.

**Changes applied to `variants/rp2350/rp2350b-mokya/`:**

| File | Change |
|------|--------|
| `platformio.ini` | `-DNO_USB`; `extra_scripts` registers `patch_arduinopico.py`; `build_src_filter` adds `ipc_serial_stub.cpp`; `lib_deps` override drops `environmental_base`/`environmental_extra`; `lib_ignore += SdFat SD`; adds `MESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` |
| `variant.h` | Removed `#define DEBUG_RP2040_PORT Serial`; added `#include "ipc_serial_stub.h"` so the `extern Serial` declaration reaches every Meshtastic TU via `configuration.h` |
| `ipc_serial_stub.h` (new) | `IpcSerialStream : public Stream` (no-op read/write, `begin()`/`end()` stubs, `operator bool`); `extern IpcSerialStream Serial;` |
| `ipc_serial_stub.cpp` (new) | Single `IpcSerialStream Serial;` global |
| `patch_arduinopico.py` (new) | Idempotent pre-build script that wraps `extern SerialUSB Serial;` in `framework-arduinopico/cores/rp2040/SerialUSB.h` under `#if !defined(NO_USB)` with `// MOKYA_NO_USB_PATCH` marker |

**Upstream Meshtastic latent bug worked around:**
`src/modules/Telemetry/Sensor/AddI2CSensorTemplate.h:17` unconditionally references `ScanI2CTwoWire` under `#if WIRE_INTERFACES_COUNT > 1`, but `ScanI2CTwoWire.h` wraps its body in `#if !MESHTASTIC_EXCLUDE_I2C`. Two-phase name lookup resolves the non-dependent name at template definition time, so the header fails to parse whenever `MESHTASTIC_EXCLUDE_I2C=1` is combined with a telemetry sensor TU that hits `AirQualityTelemetry.cpp`. Workaround: `-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` (orthogonal to M1.0 — aligned with existing Core 0 no-sensor policy).

**Build result:**

| Metric | Value |
|--------|-------|
| RAM   | 10.9 % (57 116 / 524 288 bytes) |
| Flash |  7.8 % (286 032 / 3 665 920 bytes) |
| ELF   | 4 600 044 bytes |
| BIN   | 298 340 bytes |
| UF2   | 596 992 bytes |
| Build time | 62.6 s |

**Pending:**
- [ ] Flash via J-Link (`RP2350_M33_0`) — USB CDC will not enumerate after flash (by design). Verify via SWD memory read and/or LoRa TX liveness. Plan M1.0 step 4.

**Rationale notes:**
- `DEBUG_RP2040_PORT` removal (A1-lite): Arduino-Pico's `DEBUGV` macro expands to `Serial.printf(...)` inside framework library TUs (e.g. `LittleFS.h`), but those TUs are compiled independently and cannot see our variant-level `Serial` injection. When `DEBUG_RP2040_PORT` is undefined, `debug_internal.h:23-27` makes `DEBUGV` a no-op, which is exactly what Core 0 wants.
- `lib_deps` override dropping `environmental_base`/`environmental_extra`: those sets pull in Adafruit sensor libraries (`Adafruit_BusIO`, `Adafruit_Sensor`, `Adafruit_BMP280`, `Adafruit_DPS310`, `Adafruit_MCP9808`, `Adafruit_INA219/260`, `Adafruit_MPU6050`, `Adafruit_LSM6DS`, `Adafruit_AHTX0`, `Adafruit_LIS3DH`). Their .cpp files reference the framework `Serial` global unconditionally (e.g. `Adafruit_Sensor.cpp:11` has an unguarded `Serial.println(...)`) and compile as independent TUs that never see our stub, so `-DNO_USB` is fatal for them. Core 0 has no sensors anyway.
- `lib_ignore += SdFat SD`: `framework-arduinopico/libraries/SdFat/` has header-inline `ls(&Serial, ...)` methods in `FatFile.h`, `FsFile.h`, `ExFatFile.h`, `FsVolume.h`, `FatVolume.h`, `ExFatVolume.h`. Any TU including these headers fails under `-DNO_USB`. PIO's LDF drags SdFat in because `src/FSCommon.cpp:17` has `#include <SD.h>` gated by `HAS_SDCARD` — LDF doesn't evaluate preprocessor, so the include is always scanned. Core 0 has no SD card.

---

## Issues Log (Phase 2)

| # | Date | Area | Issue | Resolution |
|---|------|------|-------|-----------|
| P2-1 | 2026-04-11 | Meshtastic `AddI2CSensorTemplate.h` | Latent upstream bug — non-dependent `ScanI2CTwoWire` name fails to resolve under `MESHTASTIC_EXCLUDE_I2C=1` due to two-phase template lookup | Workaround: `-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` in Core 0 variant. Upstream fix would be to also guard the `ScanI2CTwoWire*` parameter declaration under `#if !MESHTASTIC_EXCLUDE_I2C`. |
