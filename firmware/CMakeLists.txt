cmake_minimum_required(VERSION 3.20)

# Pull in Pico SDK (must be before project())
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(MokyaLora C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ── Sub-libraries ─────────────────────────────────────────────────────────────

# MokyaInput Engine (MIT) — portable static library, no Pico SDK dependency
add_subdirectory(mie)

# ── Firmware targets ──────────────────────────────────────────────────────────

# Core 0: Meshtastic modem (GPL-3.0)
add_subdirectory(core0)

# Core 1: UI & application layer (Apache-2.0)
add_subdirectory(core1)

# ── Bring-up / development tools ──────────────────────────────────────────────
add_subdirectory(tools)
