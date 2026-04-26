# CLAUDE.md

This file provides guidance to Claude Code when working in this repository.

## Project Overview

**MokyaLora (Project MS-RP2350)** is an open-hardware standalone Meshtastic feature phone.
The goal is a fully phone-free, long-range mesh communication device built around the Raspberry Pi RP2350B, with a 36-key physical keyboard, 2.4″ IPS display, LoRa radio, and GNSS.

The project is currently in the **hardware design phase**. Firmware does not yet exist.

## Repository Structure

```
MokyaLora/
├── docs/
│   ├── requirements/           # System requirements documents
│   │   ├── system-requirements.md   — system specs, operating modes, mandatory hw rules
│   │   ├── hardware-requirements.md — full BOM, power tree, GPIO map, keypad matrix
│   │   └── software-requirements.md — SRS (WHAT): driver needs, power states, UI/UX, IME requirements
│   ├── design-notes/           # Design decision records
│   │   ├── power-architecture.md    — power tree, rail definitions, charger config
│   │   ├── rf-matching.md           — LoRa / GNSS RF frontend, TCXO coupling, antenna rules
│   │   ├── mcu-gpio-allocation.md   — full GPIO pin map, I2C bus allocation
│   │   └── mie-architecture.md      — MokyaInput Engine design and roadmap
│   ├── bringup/                # Bring-up and debug logs
│   │   ├── rev-a-bringup-log.md     — Rev A checklist (board at manufacturer)
│   │   └── measurements/            — oscilloscope/spectrum captures (add files here)
│   └── manufacturing/          # Manufacturing-related documents
│       ├── fab-notes.md             — PCB spec, fabrication status, Gerber list, assembly notes
│       └── compliance.md            — regulatory notes (CE/FCC/NCC, deferred post Rev A)
├── hardware/
│   ├── kicad/                  # KiCad 8 source design files
│   │   ├── MokyaLora.kicad_pro      — project file
│   │   ├── MokyaLora.kicad_sch      — top-level schematic (13 sub-sheets)
│   │   ├── MokyaLora.kicad_pcb      — PCB layout
│   │   ├── MokyaLora.kicad_sym      — project symbol library
│   │   ├── *.kicad_dbl              — component database libraries (ODBC DSN: KiCad-Library)
│   │   ├── MokyaLora.pretty/        — project footprint library (61 footprints)
│   │   ├── packages3D/              — component 3D models (see NOTICE for licensing)
│   │   ├── FabricationFiles/        — KiCad fabrication output (Gerbers + BOM); source
│   │   └── plots/                   — KiCad plot output (schematic PDF + PCB renders)
│   ├── production/             # Released fabrication snapshots — copied from FabricationFiles
│   │   └── rev-a/
│   │       ├── gerber/              — Gerber + drill files
│   │       ├── pdf/                 — schematic PDF + assembly drawing PDF
│   │       ├── MokyaLora.csv        — Bill of Materials
│   │       └── MokyaLora.step       — full-board 3D export
│   └── mechanical/             # Enclosure and stack-up drawings (future)
│       └── enclosure/
├── firmware/                   # Phase 1 active (MIE on PC); Core 0/1 pending Rev A
│   ├── CMakeLists.txt
│   ├── core0/                  — Core 0 modem firmware [GPL-3.0]
│   │   ├── meshtastic/         — git submodule: tengigabytes/firmware@feat/rp2350b-mokya
│   │   └── src/                — RP2350B platform glue (variants/, radio init)
│   ├── core1/                  — Core 1 UI & application firmware [Apache-2.0]
│   │   ├── freertos-kernel/    — git submodule: FreeRTOS/FreeRTOS-Kernel V11.3.0 [MIT]
│   │   │   └── portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/
│   │   │                       — nested submodule: RP2350 Cortex-M33 port
│   │   └── src/                — LVGL, FreeRTOS tasks, MIE integration
│   ├── mie/                    — MokyaInput Engine sub-library [MIT]
│   │   ├── CMakeLists.txt      — builds as static library with no Pico SDK dependency
│   │   ├── include/mie/        — public headers (#include <mie/...>)
│   │   ├── src/                — Trie-Searcher, IME-Logic
│   │   ├── hal/                — IHalPort interface + rp2350/ and pc/ adapters
│   │   ├── tools/              — gen_font.py, gen_dict.py (data pipeline)
│   │   ├── data/               — generated .bin assets (gitignored)
│   │   └── tests/              — C++ unit tests, host-only build
│   ├── shared/
│   │   └── ipc/                — Inter-core IPC protocol definition [MIT]
│   │       └── ipc_protocol.h  — ONLY shared header between Core 0 and Core 1
│   └── tools/                  — flash scripts and test utilities
└── .gitignore
```

## Architecture

Hardware spec, BOM, GPIO map, and mandatory HW rules live in
`docs/requirements/system-requirements.md` and
`docs/requirements/hardware-requirements.md`. Firmware implementation detail
(memory map, IPC byte layout, build system, boot sequence) lives in
`docs/design-notes/firmware-architecture.md`. This file is a working cheat
sheet, not authoritative.

### Dual-Core Software (high-level)
- **Core 0:** Meshtastic LoRa modem (GPL-3.0, PlatformIO).
- **Core 1:** FreeRTOS + LVGL + MIE UI (Apache-2.0, CMake/Ninja).
- IPC: three SPSC rings + GPS double-buffer in 24 KB shared SRAM. See
  `firmware/shared/ipc/ipc_protocol.h` and firmware-architecture.md §5.

### I2C Bus Layout (day-to-day reference; SYS §7 / hw-requirements is authoritative)

> **Shared peripheral (Rev A).** On RP2350 both pin pairs land on `i2c1` only —
> GPIO mod-4 = 2/3 (which covers 6/7 and 34/35) has no I2C0 pinmux alternative.
> Rev A firmware therefore time-multiplexes `i2c1` between the two pin pairs
> via a FreeRTOS mutex — see `firmware/core1/src/i2c/i2c_bus.c`. Drivers MUST
> go through `i2c_bus_acquire` / `i2c_bus_release`; never call `i2c_init` or
> pass a raw `i2c_inst_t*` directly. Rev B plans to reroute the sensor bus to
> a mod-4 = 0/1 pair (e.g. GPIO 32/33) to restore two independent peripherals.

- **Sensor + GNSS bus** (GPIO 34/35, `i2c1`): IMU 0x6A, Mag 0x1E, Baro 0x5D, GPS 0x3A
- **Power bus** (GPIO 6/7, `i2c1`): Charger BQ25622 0x6B, Fuel Gauge BQ27441 0x55, LED Driver LM27965 0x36

## License Boundary Rules (CRITICAL)

| Directory              | License     | May include                        | Must NOT include              |
|------------------------|-------------|------------------------------------|-------------------------------|
| `firmware/core0/`      | GPL-3.0     | Meshtastic, `shared/ipc/`          | core1/, mie/                  |
| `firmware/core1/`      | Apache-2.0  | `shared/ipc/`, mie/, freertos-kernel/ | Any Meshtastic or core0 header|
| `firmware/mie/`        | MIT         | Only its own headers + hal_port.h  | core0/, core1/, Meshtastic    |
| `firmware/shared/ipc/` | MIT         | stdint.h only                      | Everything else               |

The sole crossing point between Core 0 (GPL-3.0) and Core 1 (Apache-2.0) is
`firmware/shared/ipc/ipc_protocol.h` (MIT — compatible with both licenses).
Never add a Meshtastic #include to core1/ or mie/. Full rationale and binary
distribution rules in firmware-architecture.md §1 and §11.

## Working with This Repo

- `docs/requirements/system-requirements.md` — system-level overview, operating modes, mandatory HW rules.
- `docs/requirements/hardware-requirements.md` — full BOM, power tree, keypad matrix, all mandatory design rules.
- `docs/requirements/software-requirements.md` — SRS (WHAT): driver class needs, power states, UI/UX, IME requirements.
- `docs/design-notes/firmware-architecture.md` — implementation (HOW): memory map, IPC byte layout, build system, boot sequence.
- `docs/design-notes/core1-driver-development.md` — required reading before adding any Core 1 peripheral driver (board header, GPIO rules, FreeRTOS pitfalls, debug playbook).
- `docs/design-notes/core1-memory-budget.md` — heap / stack / static-buffer ledger. Any new Core 1 task or queue MUST update this file in the same commit, and `main_core1_bridge.c` panics on task-start failure (no `(void)rc_xxx`).
- `docs/design-notes/mcu-gpio-allocation.md` contains the full GPIO table — keep in sync with the KiCad schematic.
- Production files in `hardware/production/` are **generated by KiCad** — do not edit manually.
- Firmware is actively developed in `firmware/` — dual-core architecture with Core 0 (PlatformIO) and Core 1 (CMake/Ninja).
- `firmware/core0/meshtastic/` is a git submodule — run `git submodule update --init --recursive` after cloning.
- `firmware/core1/freertos-kernel/` is a git submodule (FreeRTOS-Kernel V11.3.0, MIT). It contains a nested submodule `portable/ThirdParty/Community-Supported-Ports` which provides the `RP2350_ARM_NTZ` Cortex-M33 port. Init with `git submodule update --init --recursive firmware/core1/freertos-kernel`.
- To make Meshtastic changes: work inside `firmware/core0/meshtastic/`, commit there, then push to `tengigabytes/firmware` on branch `feat/rp2350b-mokya`.
- New RP2350B board variant goes in `firmware/core0/meshtastic/variants/rp2350b-mokya/`.

## Current Development Phase

**Phase 1 — MIE on PC** ✅ complete — 120 GoogleTest cases pass, C API ready at `firmware/mie/`.

**Rev A hardware bring-up** ✅ complete — Steps 1–26, logged in `docs/bringup/rev-a-bringup-log.md`.

**Phase 2 — RP2350B firmware productization** (active, on Rev A board):
- Goal: turn bring-up architecture into dual-core production firmware — Core 0 Meshtastic LoRa modem, Core 1 FreeRTOS + LVGL + MIE UI, shared-SRAM SPSC ring IPC.
- Tracked by milestone (M1.0, M1.0b, M1.1, ...) in `docs/bringup/phase2-log.md`.
- Plan: `~/.claude/plans/groovy-petting-alpaca.md`.
- Current status: **M1 ✅ + M2 ✅ complete (2026-04-13).** M1 delivered IPC byte bridge (staged-delivery, taskYIELD + TX accumulation, Config IPC definition). M2 delivered doorbell-driven IPC (SIO doorbell + `xTaskNotifyFromISR`, Part A), graceful reboot via `RebootNotifier` + `tud_disconnect()` (Part B, fixes P2-10), and flash write safety via linker `--wrap` + Core 1 parking (fixes P2-11). **P2-13 fix (XIP cache was disabled since boot) eliminated the 16× throughput gap.** CLI `--info`: 15.0 s → 5.9 s → 4.5 s (parity with stock Pico2); burst rate 2.5× faster than stock. IPC handshake v2 deferred to M5. **IMPORTANT: always use `python -m meshtastic` (v2.7.8), never bare `meshtastic` command.** **M3.1/M3.2 ✅ display + LVGL, M3.3 ✅ keypad (Phase A PIO+DMA scan, Phase B 20 ms debounce + keymap→KeyEvent queue, Phase C LVGL consumer + landscape 320×240 view mirroring physical PCB). M3.4.1 ✅ shared I2C bus module (time-muxed `i2c1` between GPIO 6/7 and GPIO 34/35 — both pairs are I2C1-only on RP2350; Rev B will reroute sensor bus to free `i2c0`). M3.4.2 ✅ BQ25622 charger driver (datasheet-accurate, VREG/ICHG/IINDPM configurable, WATCHDOG 50 s + 1 Hz kick, auto re-init on WD expiry, TS/TDIE ADC, HIZ + BATFET ship/shutdown APIs). M3.4.3 ✅ LM27965 3-bank LED driver (TFT BL / keypad BL / red + green indicators / all-off; GP cache for partial updates). M3.4.4 ✅ BQ27441 fuel gauge stub. M3.4.5a ✅ LPS22HH barometer + sensor-bus I2C baudrate fix (P2-14). M3.4.5b ✅ LIS2MDL magnetometer driver (mag X/Y/Z in µT×10 + internal temp, with mag/temp split read due to auto-increment wrap quirk at 0x6D → 0x68). M3.4.5c ✅ LSM6DSV16X 6-axis IMU driver (accel ±2 g, gyro ±250 dps, internal temp; single 14-byte burst from OUT_TEMP_L covers T+G+A under BDU; 30 Hz HP ODR polled at 10 Hz). M3.4.5d ✅ Teseo-LIV3FL GNSS driver — streaming NMEA parser (GGA+RMC+GSV) on its own `gps_task` (100 ms drain, 1 KB burst, 48 KB heap), parsed fix state + 32-sat pooled view, runtime `teseo_set_fix_rate(OFF/1/2/5/10Hz)` API via `$PSTMSETPAR,1303` + `$PSTMSAVEPAR` + `$PSTMSRR` sequence (tested 1/2 Hz match request exactly, 5/10 Hz clamp to Teseo's ~3 Hz ceiling for 4-constellation config), `send_await` helper + reply dispatch (`$PSTMSETPAROK/ERROR`, `$PSTMSAVEPAROK/ERROR`) reusable for future ST commands. Not called automatically on boot so NVM wear is non-issue. Not in scope: `IpcGpsBuf` writer (Core 0 consumer M5). Part C ✅ RF diagnostics: `teseo_rf_state_t` snapshot (noise floor / ANF status / CPU / per-sat C/N0), parsers for `$PSTMRF/NOISE/NOTCHSTATUS/CPU`, `teseo_enable_rf_debug_messages()` commission API (CDB 231 mask 0x408000A8, SETPAR+SAVEPAR+SRR once, NVM persists), LVGL `rf_debug_view` selectable via `MOKYA_BOOT_VIEW_RF_DEBUG=1`. **M3.6 ✅ (2026-04-19) MIEF font driver** — `firmware/core1/src/display/mie_font.{c,h}` implements LVGL v9 `lv_font_t` callbacks against the MIEF v1 binary format; `.incbin` embeds `mie_unifont_sm_16.bin` (~831 KB, 19 320 glyphs incl. Traditional CJK + Bopomofo + Latin-1) into `.rodata`; CMake auto-regens the blob from `gen_font.py` + `charlist.txt`. `font_test_view` (FUNC cycles to it) validated ASCII / CJK / Latin-1. LVGL ASCII-only restriction lifted. **Phase 1.6 ✅ (2026-04-24) personalised LRU cache + follow-up** — 64-entry LRU promotes repeat-typed chars to rank 0, flash-persisted at `0x10C00000` with symmetric P2-11 park. Six-passage hardware regression: short/repetitive content sees +18 / +23 rank-0 lift; long passages flat due to 64-entry cap (tracked as P1.6.1). Follow-up delivered: `--reboot` ring-wrap fix, dict-blob regen fix (`sm`/`lg` collision), **RTT key-inject transport** alongside SWD with mode-based arbitration (`g_key_inject_mode`), SWD-poll refactor (per-char 400→310 ms). See `docs/bringup/mie-v4-status.md` + `docs/design-notes/mie-p1.6-lru-plan.md`. **M3.5 Phase 1 ✅ (2026-04-26) IpcGpsBuf bridge end-to-end with dummy fixed-position injector** — Core 1 dummy NMEA producer (`gps_dummy.c`, GGA+RMC at 25.052103 N / 121.574039 E, build flag `MOKYA_GPS_DUMMY_NMEA=ON`, default OFF), `IpcGpsBuf` typedef sizeof bug fixed (was 261 vs reserved 260), Core 0 `IpcGpsStream` Arduino Stream adapter (`firmware/core0/.../ipc_gps_stream.{h,cpp}`), Meshtastic submodule patched (commit `06c5f15a3`) so `GPS::_serial_gps` accepts `Stream*` under `MOKYA_IPC_GPS_STREAM`, probe is skipped (`tx_gpio=0`), `setup()` accepts `GNSS_MODEL_UNKNOWN`. Verified `meshtastic --info` reports the local node at the dummy coordinates with `locationSource=LOC_INTERNAL`. Phase 2 = wire the real Teseo NMEA into `IpcGpsBuf` (deferred). **M5 Phase 1 minimal slice ✅ (2026-04-26) RX_TEXT one-way Core 0 → Core 1 → LVGL** — `IpcTextObserver` on Core 0 listens to `textMessageModule->notifyObservers` and pushes `IpcPayloadText` onto `c0_to_c1` DATA ring with `IPC_MSG_RX_TEXT`; Core 1 dispatcher decodes into `messages_inbox` (single-snapshot + seq gate, no mutex needed for SPSC); new LVGL `messages_view` (5th in view router, FUNC cycles to it) renders sender ID + UTF-8 body. Verified end-to-end with peer-node `--sendtext`: 106-byte Traditional-Chinese DM round-trips byte-perfect through Router→TextMessageModule→IPC ring→inbox→LVGL. Submodule commit `c3c7776c1`. Next: M5 Phase 2 (NODE_UPDATE / TX_ACK / IPC_CMD_SEND_TEXT) or M6 (UI shell — message list view, compose flow with MIE).**

### Known Phase 2 constraints

- **Core 0 Meshtastic must build with single-core FreeRTOS** (`-DconfigNUMBER_OF_CORES=1`). Arduino-Pico 5.4.4's `rp2350_base` enables FreeRTOS **SMP** via `-D__FREERTOS=1`, and the RP2350 SMP port launches Core 1's passive-idle task from `xPortStartScheduler`, which HardFaults in `vStartFirstTask` before any user code runs. MokyaLora gives Core 1 to a separate Apache-2.0 image at `0x10200000`, so Core 0's FreeRTOS must stay single-core. Switching to `configNUMBER_OF_CORES=1` requires five idempotent framework patches applied by `firmware/core0/meshtastic/variants/rp2350/rp2350b-mokya/patch_arduinopico.py` (SerialUSB.h extern guard, freertos-main.cpp + freertos-lwip.cpp SMP call-site guards, portmacro.h missing extern decl, port.c `static` removal). Full rationale in `docs/bringup/phase2-log.md` Issue P2-2.

## Build & Flash Rules

- Always verify a build compiles successfully before moving on to additional changes.
- If a build fails, fix it immediately — do not layer more changes on top of a broken build.
- **Auto-flash:** After each firmware change, always build **and flash** (`bash scripts/build_and_flash.sh`) — do not stop at compile-only. The user expects the board to be running the latest code immediately. Use `--core1` flag when only Core 1 changed.
- After adding new features, check memory usage: flash size constraints are common on embedded targets.

## Build Commands

```sh
# MIE host build — Windows (VS Build Tools 2019, from project root)
# IMPORTANT: project must be on a local drive (not UNC/network path)
cmake -S firmware/mie -B build/mie-host -G "Visual Studio 16 2019" -A x64
cmake --build build/mie-host --config Debug --parallel
cmake --build build/mie-host --config Debug --target RUN_TESTS

# MIE host build — Linux / macOS
cmake -S firmware/mie -B build/mie-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/mie-host
ctest --test-dir build/mie-host

# Run interactive REPL (after build)
# Windows:  build\mie-host\Debug\mie_repl.exe
# Linux:    ./build/mie-host/mie_repl

# Dual-image firmware — build + flash via J-Link (from project root)
# Builds Core 0 (PlatformIO) + Core 1 (CMake/Ninja), flashes both via J-Link SWD.
bash scripts/build_and_flash.sh

# Core 1 only — build + flash (skip Core 0 PlatformIO rebuild)
bash scripts/build_and_flash.sh --core1

# Core 0 only — PlatformIO build (from project root)
python -m platformio run -e rp2350b-mokya -d firmware/core0/meshtastic

# Core 1 only — CMake build (from project root, requires prior cmake configure)
cmake --build build/core1_bridge

# Core 1 cmake configure (one-time, from project root)
# Requires: VS Build Tools 2019, ARM GCC, Ninja, Pico SDK at C:\pico-sdk
PICO_SDK_PATH=/c/pico-sdk cmake -S firmware/core1/m1_bridge \
    -B build/core1_bridge -G Ninja

# Legacy bringup firmware (Steps 1–26, no longer active)
# bash scripts/build_and_flash_bringup.sh

# Data generation tools
python firmware/mie/tools/gen_font.py   # produces font_glyphs.bin + font_index.bin
python firmware/mie/tools/gen_dict.py   # produces dict_dat.bin + dict_values.bin
```

## Hardware Debug Toolchain

A J-Link Ultra V6 is connected to the RP2350B target via SWD. Claude Code can
autonomously build, flash, and perform SWD debug — no manual intervention required.

### Toolchain Paths

| Tool | Path |
|------|------|
| J-Link Commander | `C:/Program Files/SEGGER/JLink_V932/JLink.exe` |
| J-Link GDB Server | `C:/Program Files/SEGGER/JLink_V932/JLinkGDBServerCL.exe` |
| ARM GDB | `C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-gdb.exe` |
| Pico SDK | `C:/pico-sdk` |
| PlatformIO | `python -m platformio` (v6.1.19) |
| Core 0 ELF | `firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware*.elf` |
| Core 1 BIN | `build/core1_bridge/core1_bridge.bin` (flash @ `0x10200000`) |
| Serial port | Auto-detected by VID `0x2E8A` (Raspberry Pi), 115200 baud, USB CDC. Helper: `scripts/_mokya-port.ps1` (`Resolve-MokyaPort`). Override via `-PortName COMxx`. |

### Automated Workflows

**Build + Flash dual-image (primary workflow):**

```sh
# Build Core 0 (PlatformIO) + Core 1 (CMake) and flash both via J-Link
bash scripts/build_and_flash.sh

# Core 1 only (faster — skips PlatformIO rebuild)
bash scripts/build_and_flash.sh --core1
```

**Regression test after flash (wait ~3 s for USB CDC re-enumeration):**

```sh
python -m meshtastic --port COMxx --info
```

**Manual J-Link dual flash (if script not available):**

```sh
JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
# Core 0 ELF at default 0x10000000, Core 1 BIN at 0x10200000
cat > /tmp/jlink_flash_dual.jlink <<EOF
connect
r
loadfile "$(cygpath -w firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware*.elf)"
loadbin "$(cygpath -w build/core1_bridge/core1_bridge.bin)" 0x10200000
r
g
qc
EOF
"$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_flash_dual.jlink)"
```

**SWD debug (halt, registers, memory read):**

```sh
JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
cat > /tmp/jlink_debug.jlink <<'EOF'
connect
h
regs
mem32 0xD0000000 4
mem32 0x10000000 4
g
qc
EOF
"$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_debug.jlink)"
```

### Key Memory Addresses

| Address | Content |
|---------|---------|
| `0xD0000000` | SIO CPUID — `0x000000C0` = Core 0, `0x000000C1` = Core 1 |
| `0x10000000` | Flash XIP base — first word is MSP initial value (`0x20082000`) |
| `0x20000000` | SRAM base (520 KB, ends at `0x20082000`) |

### Notes

- Device name must be `RP2350_M33_0` (not `RP2350`). Use `RP2350_M33_1` for Core 1.
- USB CDC disconnects while MCU is halted — expected; reconnects on resume.
- After J-Link flash, USB may need 2–3 s to re-enumerate before serial is available.
- **COM port stuck/busy:** If serial open fails (`UnauthorizedAccessException`), re-flash via J-Link — this resets the MCU and forces USB CDC re-enumeration, releasing the port. Always flash before retrying serial.
- `scripts/bringup_run.ps1` handles build+flash+serial in one step with `-Flash` flag.

## Default Debug Methodology — SWD + RTT

**SWD + SEGGER RTT is the FIRST tool to reach for** when debugging Core 1
behavioural problems (latency, hangs, wrong output, crashes). Don't start
with breadcrumbs, `printf`-over-CDC, or LED toggles — RTT is faster to set
up, faster to read, and far less invasive.

### Why RTT first

| Mechanism | Setup | Live read | Per-event cost | Cross-task picture |
|---|---|---|---|---|
| SWD breadcrumb | low | requires halt | ~5 cycles | one point per slot |
| `printf` over USB CDC | medium | yes (CDC stack) | ~50 µs (USB) | text only |
| **SWD + RTT** | **medium (one-time)** | **yes (no halt)** | **~50 ns** | **CSV with µs timestamps** |
| GPIO toggle + scope | trivial | yes (scope) | ns | needs hardware |

RTT writes to a SRAM ring buffer; J-Link drains it via background DAP
transactions without halting the CPU. **Multi-task latency profiling is
essentially impossible without it** — breadcrumbs only capture point-in-time
state, USB CDC is too slow and noisy.

### Trace infrastructure (already wired into Core 1)

- `firmware/core1/src/debug/mokya_trace.h` — `TRACE(src, ev, fmt, ...)` and
  `TRACE_BARE(src, ev)` macros emit one CSV line per event:
  `<us_timestamp>,<source>,<event>[,<key=val>...]`
- Backed by SEGGER `SEGGER_RTT.c` bundled in `pico-sdk/src/rp2_common/pico_stdio_rtt/SEGGER/`
- Initialised once via `SEGGER_RTT_Init()` near the top of
  `main_core1_bridge.c`. Available immediately, even before USB / FreeRTOS.
- `MOKYA_MIE_PERF_TRACE=1` compile flag (set in Core 1 CMakeLists) enables
  optional `MIE_TRACE` macros inside `firmware/mie/src/` so MIE stays
  decoupled from `mokya_trace.h` on PC builds.

### Adding a trace point

```c
#include "mokya_trace.h"

// With payload fields:
TRACE("ime", "key_pop", "kc=0x%02x,p=%u",
      (unsigned)ev.keycode, (unsigned)ev.pressed);

// Without payload:
TRACE_BARE("ime", "done");
```

The first column is always a microsecond timestamp from `timer_hw->timerawl`
(no syscall, single MMIO load). `src` and `ev` together form the event tag.

### Capturing a trace

```sh
# Run logger in background
"C:/Program Files/SEGGER/JLink_V932/JLinkRTTLogger.exe" \
    -Device RP2350_M33_1 -If SWD -Speed 4000 \
    -RTTSearchRanges "0x20000000 0x80000" \
    -RTTChannel 0 /tmp/rtt.log

# Reproduce the issue on hardware (press keys, type, etc.)

# Stop logger and analyze
taskkill //IM JLinkRTTLogger.exe //F
python scripts/analyze_rtt_latency.py /tmp/rtt.log
```

Notes:
- Output may end up at the Windows-native path `C:/Users/<u>/AppData/Local/Temp/rtt.log`
  even when `/tmp/rtt.log` was given on the command line. Read both if one is empty.
- The 1 KB up-buffer can drop events under heavy bursts. If important events
  are missing, raise `BUFFER_SIZE_UP` in
  `pico-sdk/src/rp2_common/pico_stdio_rtt/SEGGER/Config/SEGGER_RTT_Conf.h`
  (or override locally) before rebuilding.
- The same RTT control block can be read by OpenOCD (raspberrypi fork) and
  pyOCD — use J-Link for everyday work, fall back to those for CI / vendor
  neutrality.

### Analysing a trace

`scripts/analyze_rtt_latency.py` parses CSV events and matches them up into
per-keystroke pipeline rows (`ime,key_pop` → `ime,done` → `lvgl,render_*` →
`lvgl,flush_done`) with p50 / p90 / max stats per segment. Use it to compare
"before" vs "after" of any optimisation that touches the input pipeline.

For ad-hoc analysis use Python directly:

```python
events = [(int(p[0]), p[1], p[2], p[3:])
          for line in open('/tmp/rtt.log')
          for p in [line.strip().split(',')] if len(p) >= 3]
# pair proc_start/proc_end, group by keycode, etc.
```

### When NOT to use RTT

- Hard fault / pre-`SEGGER_RTT_Init` boot crashes — RTT control block isn't
  set up yet, so use SWD memory inspection (`mem32` of stack/PC/sentinel
  addresses) instead.
- Non-Core-1 contexts (Core 0 Meshtastic) — RTT is wired into Core 1 only.
- Production firmware (RTT consumes ~6 KB code + 1 KB SRAM); compile out
  via `#ifdef DEBUG_RTT` if shipping size is tight.

## IPC Protocol

`firmware/shared/ipc/ipc_protocol.h` is the **only** cross-core shared header.
Transport is three SPSC rings + GPS double-buffer in 24 KB shared SRAM, with
HW FIFO doorbell. **NEVER** add a Meshtastic `#include` to `core1/`, `mie/`,
or `shared/ipc/`.

Full byte map, message catalogue, and end-to-end flows: see
`docs/design-notes/firmware-architecture.md` §5.

## Driver Development Rules

When writing drivers or bringup firmware, **never assume** any of the following — ask the user instead:
- Register addresses or field definitions (REG MAP)
- Initialization or operation sequences
- Hardware configuration (pin states, timing, protocol modes, default values)
- Any device-specific detail not already confirmed in the codebase or bringup log

If uncertain, stop and ask. The user will provide the correct information or point to the relevant datasheet section.

**Core 1 drivers:** Before writing a new peripheral driver under `firmware/core1/`, read
[`docs/design-notes/core1-driver-development.md`](docs/design-notes/core1-driver-development.md).
It captures the RP2350B board-header requirement, GPIO rules, FreeRTOS heap / priority
pitfalls, the I2C / SysTick workarounds Core 1 needs, and a top-down debug playbook
derived from M1–M3.3 incidents. Skipping it tends to reproduce bugs we have already
solved once.

## J-Link Device Names (RP2350)

When writing J-Link Commander scripts or GDB Server commands, always specify the core explicitly:

| Device name | Core | When to use |
|---|---|---|
| `RP2350_M33_0` | Core 0 (Cortex-M33) | All current bringup work |
| `RP2350_M33_1` | Core 1 (Cortex-M33) | Step 16 Core 1 validation onwards |

Do NOT use the alias `RP2350` — it is unknown to the J-Link software and silently falls back to `RP2350_M33_0`.

## Hardware Debug Protocol

When debugging a hardware peripheral that is not responding correctly, follow this sequence **without skipping steps**:

1. **Read first.** Before writing any code, read all relevant datasheet excerpts in `docs/datasheets/` for the component. Do NOT write code that references register fields or configuration parameters that have not been verified to exist in the datasheet — invented fields are silent bugs on real hardware.
2. **List root causes.** Enumerate all plausible root causes ranked by likelihood. For each, cite the specific datasheet section that supports the hypothesis.
3. **Propose diagnostics.** For each hypothesis, propose a minimal diagnostic test (e.g., read a register, toggle a pin, measure a signal) that would confirm or eliminate it.
4. **Wait for approval.** Present the full diagnostic plan and wait for the user to approve before writing any code.
5. **Implement one at a time.** After approval, implement and run one diagnostic test at a time. Never assume a fix — prove each hypothesis before moving on.

## Language

- All discussions with the user must be in **Traditional Chinese (Taiwan)** (正體中文).
- Code comments, documentation, and project descriptions must be in **English**.
- Exception: content that demonstrates Chinese IME output (e.g., MIE test cases) should use Chinese as appropriate.

## Session Hygiene

At the following natural breakpoints, remind the user to commit any open changes, then start a fresh session:
- A discrete feature or driver is complete and tested.
- The conversation has grown long (many tool calls, large context).

Suggested reminder phrasing: "這個功能已完成，建議 commit 後重開 session 保持 context 乾淨。"
