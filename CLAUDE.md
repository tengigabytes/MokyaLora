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

> **RP2350 SDK peripheral note:** Both buses map to `i2c1` in the Pico SDK — GPIO 6/7 and GPIO 34/35 are both I2C1 pin options. They cannot be active simultaneously; switch by reinitialising `i2c1` with different GPIO pairs.

- **Sensor bus** (`i2c1`, GPIO 34/35): IMU 0x6A, Mag 0x1E, Baro 0x5D, GPS 0x3A
- **Power bus** (`i2c1`, GPIO 6/7): Charger BQ25622 0x6B, Fuel Gauge BQ27441 0x55, LED Driver LM27965 0x36

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
- Current status: **M1 ✅ + M2 ✅ complete (2026-04-13).** M1 delivered IPC byte bridge (staged-delivery, taskYIELD + TX accumulation, Config IPC definition). M2 delivered doorbell-driven IPC (SIO doorbell + `xTaskNotifyFromISR`, Part A), graceful reboot via `RebootNotifier` + `tud_disconnect()` (Part B, fixes P2-10), and flash write safety via linker `--wrap` + Core 1 parking (fixes P2-11). **P2-13 fix (XIP cache was disabled since boot) eliminated the 16× throughput gap.** CLI `--info`: 15.0 s → 5.9 s → 4.5 s (parity with stock Pico2); burst rate 2.5× faster than stock. IPC handshake v2 deferred to M5. **IMPORTANT: always use `python -m meshtastic` (v2.7.8), never bare `meshtastic` command.** **M3.1/M3.2 ✅ display + LVGL, M3.3 ✅ keypad (Phase A PIO+DMA scan, Phase B 20 ms debounce + keymap→KeyEvent queue, Phase C LVGL consumer + landscape 320×240 view mirroring physical PCB). Next: M3.4 — sensor/power HAL on Core 1.**

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
