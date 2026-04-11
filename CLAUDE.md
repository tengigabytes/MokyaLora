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
│   │   ├── system-requirements.md   — system specs, BOM highlights, mandatory hw rules
│   │   ├── hardware-requirements.md — full BOM, power tree, GPIO map, keypad matrix
│   │   └── software-requirements.md — SW architecture, memory map, drivers, power states, UI/UX, IME
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

### Hardware (Rev 5.2)
- **MCU:** RP2350B (QFN-80, dual-core Cortex-M33, 1.8 V logic)
- **Memory:** 16 MB Flash (W25Q128JW) + 8 MB PSRAM (APS6404L) via QSPI
- **Display:** 2.4″ IPS LCD 240×320, 8-bit parallel 8080 (Newhaven NHD-2.4-240320AF)
- **Input:** 36-key 6×6 matrix with SDM03U40 Schottky diodes (NKRO), PIO+DMA scan
- **LoRa:** SX1262 (SPI1) + ECS-TXO-20CSMV4 TCXO 32 MHz
- **GNSS:** ST Teseo-LIV3FL (I2C0, 0x3A) + BGA725L6 LNA + B39162B4327P810 SAW
- **Audio:** IM69D130 PDM mic (PIO) + NAU8315 3.2 W Class-D amp (PIO) + CMS-131304 speaker
- **Sensors:** LSM6DSV16X IMU (0x6A), LIS2MDL mag (0x1E), LPS22HH baro (0x5D) — all on sensor bus (GPIO 34/35, `i2c1`)
- **Power:** BQ25622RYKR charger, BQ27441DRZR fuel gauge, TPS62840 1.8 V buck, TPS7A2033 3.3 V LDO
- **Battery:** Nokia BL-4C (~890 mAh)

### Dual-Core Software Architecture (Planned)
- **Core 0:** Meshtastic protocol stack, LoRa radio
- **Core 1:** FreeRTOS + LVGL, Input Method Engine (IME), UI rendering, power management
- **Framework:** Arduino-Pico + FreeRTOS + LVGL

### I2C Bus Layout

> **RP2350 SDK peripheral note:** Both buses map to `i2c1` in the Pico SDK — GPIO 6/7 and GPIO 34/35 are both I2C1 pin options. They cannot be active simultaneously; switch by reinitialising `i2c1` with different GPIO pairs.

- **Sensor bus** (`i2c1`, GPIO 34/35): IMU 0x6A, Mag 0x1E, Baro 0x5D, GPS 0x3A
- **Power bus** (`i2c1`, GPIO 6/7): Charger BQ25622RYKR 0x6B, Fuel Gauge BQ27441DRZR 0x55, LED Driver LM27965 0x36

## License Boundary Rules (CRITICAL)

| Directory              | License     | May include                        | Must NOT include              |
|------------------------|-------------|------------------------------------|-------------------------------|
| `firmware/core0/`      | GPL-3.0     | Meshtastic, `shared/ipc/`          | core1/, mie/                  |
| `firmware/core1/`      | Apache-2.0  | `shared/ipc/`, mie/, freertos-kernel/ | Any Meshtastic or core0 header|
| `firmware/mie/`        | MIT         | Only its own headers + hal_port.h  | core0/, core1/, Meshtastic    |
| `firmware/shared/ipc/` | MIT         | stdint.h only                      | Everything else               |

The sole crossing point between Core 0 (GPL-3.0) and Core 1 (Apache-2.0) is
`firmware/shared/ipc/ipc_protocol.h` (MIT — compatible with both licenses).
Never add a Meshtastic #include to core1/ or mie/.

## Key Design Constraints

- All GPIO are **1.8 V** — use SSM3K56ACT (low-Vth) to switch LEDs and motor.
- LSM6DSV16X SA0 must be tied to GND → address 0x6A (default 0x6B conflicts with BQ25620).
- TCXO to SX1262 XTA: **220 Ω series + 10 pF shunt** required.
- Speaker (CMS-131304, 0.7 W) must be software-limited; NAU8315 is 3.2 W — cap at −3 dB.
- Anti-brick: QSPI Flash CS test point is mandatory.
- SWD (SWCLK/SWDIO/GND) must be exposed on PCB.

## Working with This Repo

- `docs/requirements/system-requirements.md` — system-level overview, BOM highlights, mandatory HW rules.
- `docs/requirements/hardware-requirements.md` — full BOM, power tree, keypad matrix, all mandatory design rules.
- `docs/requirements/software-requirements.md` — SW architecture, memory map, driver class specs, power states, UI/UX, IME.
- `docs/design-notes/mcu-gpio-allocation.md` contains the full GPIO table — keep in sync with the KiCad schematic.
- Production files in `hardware/production/` are **generated by KiCad** — do not edit manually.
- Firmware placeholder lives in `firmware/`; no compiled code exists yet.
- `firmware/core0/meshtastic/` is a git submodule — run `git submodule update --init --recursive` after cloning.
- `firmware/core1/freertos-kernel/` is a git submodule (FreeRTOS-Kernel V11.3.0, MIT). It contains a nested submodule `portable/ThirdParty/Community-Supported-Ports` which provides the `RP2350_ARM_NTZ` Cortex-M33 port. Init with `git submodule update --init --recursive firmware/core1/freertos-kernel`.
- To make Meshtastic changes: work inside `firmware/core0/meshtastic/`, commit there, then push to `tengigabytes/firmware` on branch `feat/rp2350b-mokya`.
- New RP2350B board variant goes in `firmware/core0/meshtastic/variants/rp2350b-mokya/`.

## Current Development Phase

**Phase 1 — MIE on PC** (active, pre-hardware, no RP2350 needed):
- Goal: build and test MokyaInput Engine on PC entirely independent of hardware.
- Entry point: `firmware/mie/` (standalone CMake project, no Pico SDK dependency).
- Reference: `docs/requirements/software-requirements.md` §5.7 Roadmap.

**Phase 2** (future): Core 0 / Core 1 firmware development starts after Rev A PCB arrives.

## Build & Flash Rules

- Always verify a build compiles successfully before moving on to additional changes.
- If a build fails, fix it immediately — do not layer more changes on top of a broken build.
- **Auto-flash:** After each bringup firmware change, always build **and flash** (`bash scripts/build_and_flash_bringup.sh`) — do not stop at compile-only. The user expects the board to be running the latest code immediately.
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

# Full firmware build (requires Pico SDK + ARM toolchain)
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S firmware -B build/firmware \
  -DCMAKE_TOOLCHAIN_FILE=$PICO_SDK_PATH/cmake/preload/toolchains/pico_arm_gcc.cmake
cmake --build build/firmware

# Bringup firmware — build only (from project root)
bash scripts/build_bringup.sh

# Bringup firmware — build + flash via J-Link (from project root)
bash scripts/build_and_flash_bringup.sh

# Run bringup commands over serial (PowerShell, from project root)
.\scripts\bringup_run.ps1 <command>              # run one command (e.g. psram_diag)
.\scripts\bringup_run.ps1 -Flash <command>       # build + flash + run command
.\scripts\bringup_run.ps1 scan_a scan_b lora     # run multiple commands in sequence

# Data generation tools
python firmware/mie/tools/gen_font.py   # produces font_glyphs.bin + font_index.bin
python firmware/mie/tools/gen_dict.py   # produces dict_dat.bin + dict_values.bin
```

## Hardware Debug Toolchain

A J-Link Ultra V6 is connected to the RP2350B target via SWD. Claude Code can
autonomously build, flash, run bringup commands, and perform SWD debug — no
manual intervention required.

### Toolchain Paths

| Tool | Path |
|------|------|
| J-Link Commander | `C:/Program Files/SEGGER/JLink_V932/JLink.exe` |
| J-Link GDB Server | `C:/Program Files/SEGGER/JLink_V932/JLinkGDBServerCL.exe` |
| ARM GDB | `C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-gdb.exe` |
| Pico SDK | `C:/pico-sdk` |
| Bringup ELF | `build/firmware/tools/bringup/i2c_custom_scan.elf` |
| Serial port | Auto-detected by VID `0x2E8A` (Raspberry Pi), 115200 baud, USB CDC. Helper: `scripts/_mokya-port.ps1` (`Resolve-MokyaPort`). Override via `-PortName COMxx`. |

### Automated Workflows

**Build → Flash → Run bringup command (end-to-end):**

```sh
# 1. Build
bash scripts/build_bringup.sh

# 2. Flash via J-Link Commander
JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
ELF_WIN=$(cygpath -w "$(pwd)/build/firmware/tools/bringup/i2c_custom_scan.elf")
printf 'connect\nr\nloadfile "%s"\nr\ng\nqc\n' "$ELF_WIN" > /tmp/jlink_flash.jlink
"$JLINK" -device RP2350_M33_0 -if SWD -speed 4000 -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_flash.jlink)"

# 3. Run bringup command via serial (wait 3 s for USB CDC re-enumerate)
#    Inline one-off approach — for routine use, prefer scripts/bringup_run.ps1
#    which auto-detects the port via VID 0x2E8A.
powershell.exe -NoProfile -File - <<'PS1'
. "$PWD/scripts/_mokya-port.ps1"
$port = Resolve-MokyaPort ''
$serial = New-Object System.IO.Ports.SerialPort $port, 115200
$serial.DtrEnable = $true; $serial.RtsEnable = $true; $serial.ReadTimeout = 300
$serial.Open(); Start-Sleep -Milliseconds 2500
try { $serial.ReadExisting() | Out-Null } catch {}
$serial.Write([byte[]]@(0x0D), 0, 1); Start-Sleep -Milliseconds 500
try { $serial.ReadExisting() | Out-Null } catch {}
$bytes = [System.Text.Encoding]::ASCII.GetBytes("COMMAND_HERE`r")
$serial.Write($bytes, 0, $bytes.Length); Start-Sleep -Milliseconds 3000
try { Write-Output ($serial.ReadExisting()) } catch {}
$serial.Close()
PS1
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

Transport: `firmware/shared/ipc/ipc_protocol.h` — the **only** cross-core shared header.

Pattern: `IpcMsgHeader` (4 bytes) + payload struct, written to shared SRAM buffer.
Sender pushes buffer offset into RP2350 HW FIFO as doorbell.

Key message types:
- `IPC_MSG_RX_TEXT`, `IPC_MSG_NODE_UPDATE`, `IPC_MSG_DEVICE_STATUS`, `IPC_MSG_TX_ACK` — Core 0 → Core 1
- `IPC_CMD_SEND_TEXT`, `IPC_CMD_SET_CHANNEL`, `IPC_CMD_POWER_STATE` — Core 1 → Core 0
- `IPC_MSG_LOG_LINE` — bidirectional debug log

**NEVER** add a Meshtastic `#include` to `core1/`, `mie/`, or `shared/ipc/`.

## Driver Development Rules

When writing drivers or bringup firmware, **never assume** any of the following — ask the user instead:
- Register addresses or field definitions (REG MAP)
- Initialization or operation sequences
- Hardware configuration (pin states, timing, protocol modes, default values)
- Any device-specific detail not already confirmed in the codebase or bringup log

If uncertain, stop and ask. The user will provide the correct information or point to the relevant datasheet section.

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
