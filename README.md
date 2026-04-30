# MokyaLora

An open-hardware standalone Meshtastic feature phone built around the RP2350B dual-core MCU,
with a 36-key physical keyboard, 2.4″ IPS display, SX1262 LoRa radio, and GNSS.
Licensed under CERN-OHL-S-2.0 (hardware) and CC-BY-SA-4.0 (documentation).

## Project Status

### Hardware

| Revision | Status |
|----------|--------|
| Rev A | **Bringup complete (2026-04, Steps 1–26)** — see [`docs/bringup/rev-a-bringup-log.md`](docs/bringup/rev-a-bringup-log.md) |

All Rev A subsystems validated on hardware: power rails, MCU boot, USB CDC, I2C
sensors (LIS2MDL mag, LSM6DSV16X IMU, LPS22HH baro, BQ25622 charger, BQ27441
fuel gauge, LM27965 LED driver), Teseo-LIV3FL GNSS (NMEA + RF diagnostics),
ST7789VI TFT (PIO 8080 + DMA), 6×6 keypad (PIO scan), W25Q128JW flash + APS6404L
PSRAM (8 MB at 75 MHz cached XIP), SX1262 LoRa (Meshtastic mesh validated),
J-Link SWD debug, NAU8315 audio, IM69D130 PDM mic. Audio + mic to be removed in
Rev B per current product direction; the rest carry forward.

### Firmware (Phase 2 — RP2350B productisation, active)

| Component | Status |
|-----------|--------|
| Core 0 — Meshtastic modem (Arduino-Pico + single-core FreeRTOS) | **Live on Rev A** — `rp2350b-mokya` variant of Meshtastic 2.7.21, IPC byte bridge to Core 1 owns USB CDC, `meshtastic --info` round-trip ~4.5 s |
| Core 1 — FreeRTOS + LVGL + UI (separate Apache-2.0 image at flash 0x10200000) | **Live** — TinyUSB CDC, doorbell-driven IPC, all I2C/sensor drivers, GNSS, ST7789VI display + LVGL v9, keypad → LVGL view router. UI page coverage post D-series 2026-05-01: D-1/D-2/D-3/D-4 ✅, D-5 ✅ 部分 (GNSS-only save; manual lat/lon editor v2), D-6 ✅ 部分 (no waypoint nav target). Only ⏳ pages left are Z-1/Z-2/Z-3 SOS (blocked on power button driver + low-batt state machine). See `docs/ui/01-page-architecture.md` for the full row-level table |
| MokyaInput Engine (MIE) | **Phase 1.6.1 on hardware** — Smart Zh/En + Bopomofo, MIEF unicode font (19 320 glyphs), MIE4 v4 dictionary in flash, 128-entry LRU cache (flash-persisted), 120/120 host tests passing |
| IPC protocol | **3 SPSC rings + GPS double-buffer in 24 KB shared SRAM** — RX_TEXT, NODE_UPDATE, TX_ACK, SEND_TEXT, GET/SET/COMMIT_CONFIG (LoRa subset) all wired; full `IPCPhoneAPI` subclass deferred |
| GPS bridge | **Phase 1 — `IpcGpsBuf` end-to-end** — Core 1 owns Teseo, NMEA streamed to Core 0's `IpcGpsStream` adapter; Phase 2 (real Teseo NMEA into ring) deferred |

Phase 2 milestones M1–M3 ✅, M5 Phase 1+2+3 ✅, B2 step 1 ✅. See
[`docs/bringup/phase2-log.md`](docs/bringup/phase2-log.md) for the full log.

## System Overview

![System Block Diagram](docs/assets/system-block-diagram.png)

## PCB Preview (Rev A)

| Front | Back |
|:-----:|:----:|
| ![PCB Front](hardware/kicad/plots/MokyaLora_F.png) | ![PCB Back](hardware/kicad/plots/MokyaLora_B.png) |

## Design Documents

| Document | Description |
|----------|-------------|
| [Schematic (PDF)](hardware/production/rev-a/pdf/MokyaLora.pdf) | Full schematic — all 13 sheets |
| [Assembly Drawing (PDF)](hardware/production/rev-a/pdf/MokyaLora__Assembly.pdf) | Component placement drawing |
| [Bill of Materials (CSV)](hardware/production/rev-a/MokyaLora.csv) | Rev A BOM |
| [Board 3D Model (STEP)](hardware/production/rev-a/MokyaLora.step) | Full-board STEP export |
| [Gerber Files](hardware/production/rev-a/gerber/) | Rev A fabrication outputs |

## Repository Layout

```
MokyaLora/
├── docs/
│   ├── assets/                         # Documentation images (block diagrams, etc.)
│   ├── requirements/                   # Requirements specifications
│   │   ├── system-requirements.md      # System-level spec, operating modes, mandatory HW rules
│   │   ├── hardware-requirements.md    # Full BOM, power tree, GPIO map, keypad matrix
│   │   └── software-requirements.md    # SRS (WHAT): driver needs, power states, UI/UX, IME
│   ├── design-notes/                   # Design decision records
│   │   ├── firmware-architecture.md    # HOW: memory map, IPC byte layout, build, boot sequence
│   │   ├── core1-driver-development.md # Required reading before adding any Core 1 driver
│   │   ├── core1-memory-budget.md      # Heap / stack / static-buffer ledger (must update on add)
│   │   ├── ipc-ram-replan.md           # Shared SRAM layout planning
│   │   ├── mie-architecture.md         # MIE module structure + roadmap
│   │   ├── mie-p1.6-lru-plan.md        # Personalised LRU cache design
│   │   ├── mie-smarten-ranking.md      # Smart-EN ranking analysis
│   │   ├── usb-control-protocol.md     # CDC#1 control protocol (M9, future)
│   │   ├── power-architecture.md       # Power tree, rail definitions, charger config
│   │   ├── rf-matching.md              # LoRa / GNSS RF frontend, TCXO coupling
│   │   └── mcu-gpio-allocation.md      # Full GPIO pin map, I2C bus allocation
│   ├── bringup/                        # Bring-up and debug logs
│   │   ├── rev-a-bringup-log.md        # Steps 1–26 (complete)
│   │   ├── phase2-log.md               # Phase 2 firmware milestones + Issues Log (P2-x, P3-x)
│   │   ├── mie-v4-status.md            # MIE Phase 1.6 hardware regression results
│   │   ├── tft-layouts.md              # LVGL view layouts
│   │   └── measurements/               # Oscilloscope / spectrum captures
│   └── manufacturing/                  # Manufacturing-related documents
│       ├── fab-notes.md                # PCB specification, Gerber list, assembly notes
│       └── compliance.md               # Regulatory notes (CE / FCC / NCC, deferred)
├── hardware/
│   ├── kicad/                          # KiCad 8 source design files (Rev A)
│   │   ├── MokyaLora.kicad_{pro,sch,pcb,sym}
│   │   ├── *.kicad_dbl                 # Component database libraries
│   │   ├── MokyaLora.pretty/           # Project footprint library (61 footprints)
│   │   ├── packages3D/                 # Component 3D models — see NOTICE for licensing
│   │   ├── FabricationFiles/           # KiCad fabrication output (source)
│   │   └── plots/                      # Schematic PDF + PCB renders
│   ├── production/                     # Released fabrication snapshots
│   │   └── rev-a/                      # Gerber + drill + PDF + BOM + STEP
│   └── mechanical/                     # Enclosure and stack-up drawings (future)
├── firmware/                           # Phase 2 active — dual-image RP2350B firmware
│   ├── CMakeLists.txt
│   ├── core0/                          # Core 0 modem firmware [GPL-3.0]
│   │   ├── meshtastic/                 # git submodule: tengigabytes/firmware @ feat/rp2350b-mokya
│   │   └── src/                        # RP2350B platform glue (variants/, radio init)
│   ├── core1/                          # Core 1 UI & application firmware [Apache-2.0]
│   │   ├── freertos-kernel/            # git submodule: FreeRTOS-Kernel V11.3.0 [MIT]
│   │   ├── m1_bridge/                  # CMake target — main_core1_bridge.c entry, IPC dispatch
│   │   └── src/                        # LVGL, drivers, MIE integration, view router
│   ├── mie/                            # MokyaInput Engine [MIT] — host-buildable static library
│   │   ├── include/mie/                # Public C headers
│   │   ├── src/                        # Trie-Searcher, IME-Logic
│   │   ├── hal/                        # IHalPort interface + rp2350 / pc adapters
│   │   ├── tools/                      # gen_font.py, gen_dict.py (data pipeline)
│   │   ├── data/                       # Generated .bin assets (gitignored)
│   │   └── tests/                      # 120 C++ host-only GoogleTest cases
│   ├── shared/
│   │   └── ipc/                        # Inter-core IPC protocol [MIT]
│   │       ├── ipc_protocol.h          # Sole shared header between Core 0 and Core 1
│   │       ├── ipc_shared_layout.h     # Shared-SRAM POD at 0x2007A000 (24 KB)
│   │       └── ipc_ringbuf.{c,h}       # SPSC ring helpers
│   └── tools/                          # Flash scripts and test utilities
├── scripts/                            # Build, flash, debug, test helpers
│   ├── build_and_flash.sh              # Primary workflow — builds both cores + J-Link flash
│   ├── ime_text_test.py                # MIE bench harness (SWD / RTT key-inject)
│   └── test_ipc_config.sh              # B2 IPC config soft-reload SWD-inject test
├── CLAUDE.md                           # Working-set entry point (this is what Claude Code reads)
└── .gitignore
```

## Licenses

| Component             | Directory              | License          |
|-----------------------|------------------------|------------------|
| Hardware design       | `hardware/`            | CERN-OHL-S-2.0   |
| Documentation         | `docs/`                | CC-BY-SA-4.0     |
| Core 0 firmware       | `firmware/core0/`      | GPL-3.0          |
| Core 1 firmware       | `firmware/core1/`      | Apache-2.0       |
| MokyaInput Engine     | `firmware/mie/`        | MIT              |
| IPC protocol          | `firmware/shared/ipc/` | MIT              |

See [LICENSE](LICENSE) for the full rationale. License files are also present in each component directory.

## Authorship and Development Approach

| Area | Authorship |
|------|-----------|
| Requirements — technical concepts and design decisions | Project owner |
| Requirements — document writing and formatting | Assisted by [Claude Code](https://claude.ai/claude-code) |
| Hardware design (schematic, PCB layout) | Project owner |
| Documentation — organisation, formatting, and structure | Assisted by [Claude Code](https://claude.ai/claude-code) |
| Firmware and software development | Assisted by [Claude Code](https://claude.ai/claude-code) |

All technical concepts, design decisions, and hardware choices originate with the project
owner. Claude Code is used as a development tool; all AI-assisted output is reviewed and
accepted by the project owner before being committed.

## Disclaimer

> **This project is an experimental prototype. Use at your own risk.**

### Hardware

- **Prototype status** — Rev A is an unverified first prototype. The design has not been
  independently tested, reviewed, or certified. Do not use this design as the sole basis
  for a production product without thorough independent validation.

- **RF and wireless regulations** — This device incorporates a LoRa radio transmitter.
  Operation of radio transmitters is regulated by law in most jurisdictions (NCC in Taiwan,
  CE/RED in the EU, FCC Part 15 in the US, and equivalents elsewhere). The user is solely
  responsible for ensuring that any use of this design complies with applicable local
  wireless regulations. No regulatory certification (CE, FCC, NCC) has been obtained for
  this design.

- **Battery and electrical safety** — This design includes a lithium-ion battery charging
  circuit. Improper assembly, use of incorrect components, or failure to follow safe
  lithium-ion handling practices may result in fire, injury, or damage to property.
  Always verify the design independently before assembly.

- **No warranty** — This hardware is provided "as-is" without any warranty of any kind,
  express or implied. See [CERN-OHL-S-2.0](LICENSE-CERN-OHL-S-2.0.txt) Section 6 for the
  full disclaimer of warranties and limitation of liability.

### Firmware

- **Phase 2 active development** — Core 0 (Meshtastic 2.7.21 modem) and Core 1 (FreeRTOS
  + LVGL UI) are under active development on Rev A hardware. The architecture is validated
  end-to-end (LoRa mesh, USB CDC over IPC bridge, IME on hardware, GPS bridge, scrollable
  message inbox, soft-reload config), but features are still landing — see
  [`docs/bringup/phase2-log.md`](docs/bringup/phase2-log.md) for the milestone-by-milestone
  log and the Issues Log of resolved defects (P2-1 through P3-5).

- **MokyaInput Engine (MIE)** — On-hardware Phase 1.6.1 with personalised LRU cache and
  MIEF unicode font. 120 host-side unit tests pass. Public C API stable.

- **Bringup tooling** — Rev A hardware bringup is complete (Steps 1–26); the legacy
  bringup shell at `firmware/tools/bringup/` is no longer the active entry point. Current
  development drives the dual-image firmware via `scripts/build_and_flash.sh`.

- **No warranty** — All firmware is provided "as-is" under its respective open-source
  licence (GPL-3.0 for Core 0, Apache-2.0 for Core 1, MIT for MIE / IPC / shared / tools)
  without any warranty of fitness for a particular purpose.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution guidelines.
