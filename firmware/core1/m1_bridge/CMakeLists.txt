# MokyaLora Phase 2 M1.1-B — Core 1 m1_bridge (Pico SDK + FreeRTOS + TinyUSB)
#
# Standalone RP2350 application image launched by Core 0 via
# multicore_launch_core1_raw() at flash 0x10200000. Unlike M1.1-A (bare metal)
# this build uses the full Pico SDK + FreeRTOS RP2350_ARM_NTZ port + TinyUSB
# device stack, so the bridge can actually speak USB CDC to a host PC.
#
# Architecture notes:
#   1. Core 0 starts the bootrom at 0x10000000 and runs Meshtastic. It calls
#      multicore_launch_core1_raw(vector_table, stack_top, entry), which writes
#      VTOR on Core 1 and wakes Core 1 at the reset handler stored in
#      word[1] of this image's .vectors section.
#   2. This image therefore MUST NOT carry an IMAGE_DEF / .embedded_block —
#      its first two words must be (initial SP, reset handler). The custom
#      linker script below DISCARDs .embedded_block and .embedded_end_block.
#   3. SDK crt0 runs _reset_handler + runtime_init_* hooks. Hooks that would
#      stomp on Core 0's hardware state (clocks, resets, spin locks) are
#      disabled via PICO_RUNTIME_SKIP_INIT_* compile defs below. We keep
#      runtime_init_install_ram_vector_table because FreeRTOS RP2350_ARM_NTZ
#      port uses exception_set_exclusive_handler() to install SVC/PendSV/
#      SysTick, which requires a writable ram_vector_table with VTOR pointing
#      at it.
#   4. FreeRTOS runs in single-core mode (configNUMBER_OF_CORES=1) — this is
#      an independent image that only ever runs on Core 1.
#   5. Output is a raw .bin (not UF2 / not picotool-processed) so J-Link can
#      flash it via `loadbin <path> 0x10200000`.
#
# Build:
#   PICO_SDK_PATH=/c/pico-sdk cmake -S firmware/core1/m1_bridge \
#       -B build/core1_bridge -G Ninja
#   cmake --build build/core1_bridge

cmake_minimum_required(VERSION 3.20)

# ── Pico SDK discovery (before project()) ────────────────────────────────────
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")
# MokyaLora Rev A = RP2350B (48 GPIOs).  The stock `pico2` board header targets
# RP2350A (30 GPIOs) and leaves GPIO 36-47 pads isolated, so we supply our own
# header via PICO_BOARD_HEADER_DIRS + PICO_BOARD.
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards")
set(PICO_BOARD mokya_rev_a CACHE STRING "MokyaLora Rev A (RP2350B)" FORCE)

include(pico_sdk_import.cmake)

project(core1_bridge C CXX ASM)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

# Dev-only: replace the Teseo NMEA source with a fixed-position dummy
# injector that writes one $GPGGA per second into IpcGpsBuf. Lets the
# IpcGpsBuf pipeline be exercised without sky view. Default OFF.
option(MOKYA_GPS_DUMMY_NMEA
    "Inject dummy fixed-position NMEA into IpcGpsBuf instead of running Teseo"
    OFF)

# M5 Phase 2 — Cascade PhoneAPI client. Core 1 parses FromRadio bytes
# flowing from Core 0 in parallel with the existing USB CDC pass-through.
# Phase A–D landed; M5E.1 (2026-04-28) flips the default to ON. The
# stub IPC paths (IPC_MSG_RX_TEXT / IPC_CMD_SEND_TEXT / IPC_MSG_TX_ACK
# / IPC_MSG_NODE_UPDATE) get deleted in M5E.3, after which this option
# becomes a no-op and can be removed.
option(MOKYA_PHONEAPI_CASCADE
    "Enable Core 1 cascade PhoneAPI byte-stream parser (M5 Phase 2)"
    ON)

pico_sdk_init()

# ── FreeRTOS RP2350_ARM_NTZ port ─────────────────────────────────────────────
set(FREERTOS_KERNEL_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/../freertos-kernel"
    CACHE PATH "FreeRTOS-Kernel path" FORCE)
set(FREERTOS_CONFIG_FILE_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    CACHE PATH "Directory containing FreeRTOSConfig.h" FORCE)

add_subdirectory(
    "${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ"
    FreeRTOS-Kernel-RP2350)

# ── LVGL v9.2.2 ──────────────────────────────────────────────────────────────
# Standalone LVGL build. lv_conf.h lives next to this CMakeLists (in src/) and
# the LVGL CMake option LV_CONF_PATH tells LVGL where to find it. Examples,
# demos, and ThorVG are disabled — Core 1 builds its own UI on top of the
# plain lvgl target. LVGL's OSAL wrappers (lv_freertos.c) need the
# FreeRTOS-Kernel include directories, which we propagate below.
set(LV_CONF_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h"
    CACHE FILEPATH "Path to project-specific lv_conf.h" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES       ON  CACHE BOOL "" FORCE)
# Demos enabled so we can link lv_demo_benchmark — see MOKYA_LVGL_BENCHMARK
# in lvgl_glue.c.
set(LV_CONF_BUILD_DISABLE_DEMOS          OFF CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lvgl" lvgl_build)

# LVGL's FreeRTOS OSAL needs FreeRTOS headers at compile time.
target_link_libraries(lvgl PUBLIC FreeRTOS-Kernel)
# lvgl_demos also compiles against FreeRTOS (timers etc via lvgl).
target_link_libraries(lvgl_demos PUBLIC FreeRTOS-Kernel)

# ── Shared IPC TU ────────────────────────────────────────────────────────────
set(SHARED_IPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ipc)

# ── Shared I2C bus module (M3.4.1) ───────────────────────────────────────────
set(I2C_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/i2c)

# ── Power drivers (M3.4.2 onwards) ───────────────────────────────────────────
set(POWER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/power)

# ── Sensor drivers (M3.4.5 onwards) ──────────────────────────────────────────
set(SENSOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/sensor)

# ── Display driver (M3) ──────────────────────────────────────────────────────
set(DISPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/display)

# ── Keypad driver (M3.3 Phase A) ─────────────────────────────────────────────
set(KEYPAD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/keypad)

# ── UI diagnostic views (M3.3 Phase C) ───────────────────────────────────────
set(UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui)

# ── PSRAM driver (M4 Step 3) ─────────────────────────────────────────────────
set(PSRAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/psram)

# ── Messages inbox (M5 Phase 1) ──────────────────────────────────────────────
set(MESSAGES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/messages)
set(METRICS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/metrics)

# ── IME module (M4 Step 3+) ──────────────────────────────────────────────────
set(IME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/ime)

# ── Settings UI (B2 Stage 2) ─────────────────────────────────────────────────
set(SETTINGS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/settings)

# ── Cascade PhoneAPI client (M5 Phase 2) ─────────────────────────────────────
set(PHONEAPI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/phoneapi)

# ── Debug / RTT trace ────────────────────────────────────────────────────────
# Reuse the SEGGER RTT C source bundled with pico-sdk (same file pico_stdio_rtt
# uses internally) but DO NOT enable pico_stdio_rtt — Core 1 doesn't use the
# pico_stdio framework. We call SEGGER_RTT_Write / SEGGER_RTT_printf directly
# from our own TRACE() macro so we can run multi-channel structured trace.
# Host: J-Link, OpenOCD, pyOCD and probe-rs all read the same _SEGGER_RTT
# control block, so this stays vendor-neutral.
set(DEBUG_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../src/debug)
set(SEGGER_DIR ${PICO_SDK_PATH}/src/rp2_common/pico_stdio_rtt/SEGGER)

# ── MIE library sources (compiled directly into core1_bridge) ────────────────
# Building MIE as an independent static library would require replicating the
# Pico SDK cross-compile toolchain state. Compiling straight into the Core 1
# executable avoids that and keeps the source-of-truth headers under
# firmware/mie/include. The C API (src/mie_c_api.cpp) is intentionally NOT
# compiled here — Core 1 uses C++ ImeLogic directly.
set(MIE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../mie/src)

# ── MIEF font blob (Phase F — Traditional Chinese rendering) ─────────────────
# Produces firmware/mie/data/mie_unifont_sm_16.bin, flashed as an
# independent partition at 0x10A00000 by scripts/build_and_flash.sh.
# Font glyphs stay XIP — Core 1's driver reads from the flash XIP
# address rather than copying to PSRAM (see mie_font_partition.h).
# gen_font.py is invoked automatically whenever the charlist, source
# OTF, or the generator script itself changes.
set(MIE_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../../mie)
set(MIEF_BLOB ${MIE_DIR}/data/mie_unifont_sm_16.bin)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT  ${MIEF_BLOB}
    COMMAND ${Python3_EXECUTABLE} ${MIE_DIR}/tools/gen_font.py
            --unifont  ${MIE_DIR}/data_sources/unifont-17.0.04.otf
            --charlist ${MIE_DIR}/data/charlist.txt
            --out      ${MIEF_BLOB}
            --no-subset
    DEPENDS ${MIE_DIR}/tools/gen_font.py
            ${MIE_DIR}/data/charlist.txt
            ${MIE_DIR}/data_sources/unifont-17.0.04.otf
    COMMENT "Compiling MIEF font blob -> ${MIEF_BLOB}"
    VERBATIM
)
add_custom_target(mie_font_data DEPENDS ${MIEF_BLOB})

# ── Executable ───────────────────────────────────────────────────────────────
# core1_reset.S MUST be the first source so its .vectors input section lands
# at offset 0 of the final .text output (i.e. flash 0x10200000). The SDK
# crt0.S also emits a .vectors section (which bounces non-Core-0 back to the
# bootrom via a CPUID check); keeping ours first overrides that behaviour.
# The linker script belt-and-braces this via an explicit file-name match.
add_executable(core1_bridge
    src/core1_reset.S
    src/main_core1_bridge.c
    src/usb_descriptors.c
    ${SHARED_IPC_DIR}/ipc_ringbuf.c
    ${I2C_DIR}/i2c_bus.c
    ${POWER_DIR}/bq25622.c
    ${POWER_DIR}/bq27441.c
    ${POWER_DIR}/lm27965.c
    ${POWER_DIR}/watchdog_task.c
    ${SENSOR_DIR}/sensor_task.c
    ${SENSOR_DIR}/lps22hh.c
    ${SENSOR_DIR}/lis2mdl.c
    ${SENSOR_DIR}/lsm6dsv16x.c
    ${SENSOR_DIR}/teseo_liv3fl.c
    ${SENSOR_DIR}/gps_task.c
    $<$<BOOL:${MOKYA_GPS_DUMMY_NMEA}>:${SENSOR_DIR}/gps_dummy.c>
    ${DISPLAY_DIR}/display.c
    ${DISPLAY_DIR}/st7789vi.c
    ${DISPLAY_DIR}/lvgl_glue.c
    ${DISPLAY_DIR}/mie_font.c
    ${KEYPAD_DIR}/keypad_scan.c
    ${KEYPAD_DIR}/key_event.c
    ${KEYPAD_DIR}/key_inject.c
    ${KEYPAD_DIR}/key_inject_frame.c
    ${KEYPAD_DIR}/key_inject_rtt.c
    ${UI_DIR}/global/ui_theme.c
    ${UI_DIR}/global/status_bar.c
    ${UI_DIR}/global/hint_bar.c
    ${UI_DIR}/global/global_alert.c
    ${UI_DIR}/boot_home_view.c
    ${UI_DIR}/launcher_view.c
    ${UI_DIR}/chat_list_view.c
    ${UI_DIR}/conversation_view.c
    ${UI_DIR}/message_detail_view.c
    ${UI_DIR}/canned_messages.c
    ${UI_DIR}/canned_view.c
    ${UI_DIR}/tools_view.c
    ${UI_DIR}/keypad_view.c
    ${UI_DIR}/rf_debug_view.c
    ${UI_DIR}/font_test_view.c
    ${UI_DIR}/ime_view.c
    ${UI_DIR}/nodes_view.c
    ${UI_DIR}/node_detail_view.c
    ${UI_DIR}/node_ops_view.c
    ${UI_DIR}/remote_admin_view.c
    ${UI_DIR}/my_node_view.c
    ${UI_DIR}/telemetry_view.c
    ${UI_DIR}/map_view.c
    ${UI_DIR}/map_nav_view.c
    ${METRICS_DIR}/history.c
    ${UI_DIR}/channels_view.c
    ${UI_DIR}/channel_edit_view.c
    ${UI_DIR}/traceroute_view.c
    ${UI_DIR}/gnss_sky_view.c
    ${UI_DIR}/firmware_info_view.c
    ${UI_DIR}/modules_index_view.c
    ${UI_DIR}/settings/settings_tree.c
    ${UI_DIR}/settings/settings_app_view.c
    ${UI_DIR}/settings/template_toggle.c
    ${UI_DIR}/settings/template_enum.c
    ${UI_DIR}/settings/template_number.c
    ${UI_DIR}/settings/template_text.c
    ${UI_DIR}/view_router.c
    ${UI_DIR}/view_registry.c
    ${SETTINGS_DIR}/settings_keys.c
    ${SETTINGS_DIR}/settings_client.c
    ${PSRAM_DIR}/psram.c
    ${IME_DIR}/mie_dict_loader.c
    ${IME_DIR}/ime_task.cpp
    ${SEGGER_DIR}/RTT/SEGGER_RTT.c
    ${DEBUG_DIR}/mokya_trace.c
    ${DEBUG_DIR}/postmortem.c
    ${DEBUG_DIR}/msp_canary.c
    ${MIE_SRC_DIR}/utf8.c
    ${MIE_SRC_DIR}/mie_init.cpp
    ${MIE_SRC_DIR}/trie_searcher.cpp
    ${MIE_SRC_DIR}/composition_searcher.cpp
    ${MIE_SRC_DIR}/ime_logic.cpp
    ${MIE_SRC_DIR}/ime_keys.cpp
    ${MIE_SRC_DIR}/ime_search.cpp
    ${MIE_SRC_DIR}/ime_display.cpp
    ${MIE_SRC_DIR}/ime_commit.cpp
    ${MIE_SRC_DIR}/ime_smart.cpp
    ${MIE_SRC_DIR}/ime_direct.cpp
    ${MIE_SRC_DIR}/lru_cache.cpp
    ${IME_DIR}/flash_safety_wrap.c
    ${IME_DIR}/lru_persist.cpp
    ${IME_DIR}/draft_store.c
    ${MESSAGES_DIR}/messages_send.c
    ${MESSAGES_DIR}/messages_tx_status.c
    ${MESSAGES_DIR}/dm_store.c
    ${MESSAGES_DIR}/node_alias.c
    $<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:${PHONEAPI_DIR}/phoneapi_framing.c>
    $<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:${PHONEAPI_DIR}/phoneapi_decode.c>
    $<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:${PHONEAPI_DIR}/phoneapi_cache.c>
    $<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:${PHONEAPI_DIR}/phoneapi_encode.c>
    $<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:${PHONEAPI_DIR}/phoneapi_tx.c>
    $<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:${PHONEAPI_DIR}/phoneapi_session.c>
)

# SEGGER_RTT.c triggers -Wcast-qual and -Wcast-align under our default
# warning flags. pico_stdio_rtt's CMakeLists silences the same two warnings
# locally — we match that.
set_source_files_properties(${SEGGER_DIR}/RTT/SEGGER_RTT.c
    PROPERTIES COMPILE_OPTIONS "-Wno-cast-qual;-Wno-cast-align")

# RTT up-buffer enlarged from default 1 KB → 4 KB so trace bursts during
# host CLI `--info` / cascade Config replay (~10 KB of phapi rx_frame +
# cfg_*/mc_* events) survive long enough for JLinkRTTLogger to drain
# them. configTOTAL_HEAP_SIZE shrunk 56 → 53 KB in the same commit to
# offset the +3 KB BSS — see docs/design-notes/core1-memory-budget.md
# §4 (Notable static buffers) and the linker assertion at the bottom of
# memmap_core1_bridge.ld.
target_compile_definitions(core1_bridge PRIVATE
    BUFFER_SIZE_UP=4096
)

# Font .bin is a flashable partition, not embedded in Core 1 image.
# Build-time dep so the blob is regenerated when charlist.txt changes
# — scripts/build_and_flash.sh then loadbin's it to 0x10A00000.
add_dependencies(core1_bridge mie_font_data)

# Generate tft_8080.pio.h next to the .pio source via pioasm.
pico_generate_pio_header(core1_bridge
    ${DISPLAY_DIR}/tft_8080.pio
    OUTPUT_DIR ${DISPLAY_DIR}
)

# Generate keypad_scan.pio.h next to its source.
pico_generate_pio_header(core1_bridge
    ${KEYPAD_DIR}/keypad_scan.pio
    OUTPUT_DIR ${KEYPAD_DIR}
)

target_include_directories(core1_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SHARED_IPC_DIR}
    ${I2C_DIR}
    ${POWER_DIR}
    ${SENSOR_DIR}
    ${DISPLAY_DIR}
    ${KEYPAD_DIR}
    ${UI_DIR}
    ${PSRAM_DIR}
    ${IME_DIR}
    ${MESSAGES_DIR}
    ${METRICS_DIR}
    ${SETTINGS_DIR}
    ${PHONEAPI_DIR}
    ${DEBUG_DIR}
    ${SEGGER_DIR}/RTT
    ${SEGGER_DIR}/Config
    ${CMAKE_CURRENT_SOURCE_DIR}/../../mie/include
)

# Compile defs — skip runtime_init hooks that would fight Core 0 for hardware.
# See runtime_init.c for the full list; we keep:
#   - runtime_init_install_ram_vector_table  (FreeRTOS needs writable VTOR)
#   - runtime_init_mutex                     (C runtime mutex init)
#   - per_core_* hooks                        (NVIC priorities etc.)
# and skip everything that touches global hardware state.
target_compile_definitions(core1_bridge PRIVATE
    # Hardware-state hooks — Core 0 already ran these
    PICO_RUNTIME_SKIP_INIT_CLOCKS=1
    PICO_RUNTIME_SKIP_INIT_EARLY_RESETS=1
    PICO_RUNTIME_SKIP_INIT_POST_CLOCK_RESETS=1
    PICO_RUNTIME_SKIP_INIT_SPIN_LOCKS_RESET=1
    PICO_RUNTIME_SKIP_INIT_BOOT_LOCKS_RESET=1
    PICO_RUNTIME_SKIP_INIT_BOOTROM_LOCKING_ENABLE=1
    PICO_RUNTIME_SKIP_INIT_USB_POWER_DOWN=1
    # No printf-over-UART / no stdio
    PICO_NO_BINARY_INFO=1
    PICO_NO_PROGRAM_VERSION_STRING=1
    # FreeRTOS single core
    configNUMBER_OF_CORES=1
    # Opt MIE source files into RTT perf trace (mie/src/ime_search.cpp etc.).
    # PC tests do not define this so MIE stays decoupled from mokya_trace.h.
    MOKYA_MIE_PERF_TRACE=1
    # Include debug-only views (rf_debug, font_test) in the FUNC cycle.
    # Production builds should set this to 0 to drop them from the
    # registry — the view_id_t enum + registry table both honour it.
    MOKYA_DEBUG_VIEWS=1
    $<$<BOOL:${MOKYA_GPS_DUMMY_NMEA}>:MOKYA_GPS_DUMMY_NMEA=1>
    $<$<BOOL:${MOKYA_PHONEAPI_CASCADE}>:MOKYA_PHONEAPI_CASCADE=1>
)

# Stamp Core 1 binary with the git commit hash + dirty flag so T-8
# firmware-info view can show the user what they're running. Captured
# at configure time — re-run cmake (or rebuild after `git commit`) to
# refresh.  Falls back to "unknown" if git isn't available.
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE MOKYA_CORE1_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" diff --quiet HEAD --
    RESULT_VARIABLE MOKYA_CORE1_GIT_DIRTY_RC
    ERROR_QUIET
)
if(NOT MOKYA_CORE1_GIT_HASH)
    set(MOKYA_CORE1_GIT_HASH "unknown")
endif()
if(NOT MOKYA_CORE1_GIT_DIRTY_RC EQUAL 0)
    set(MOKYA_CORE1_GIT_HASH "${MOKYA_CORE1_GIT_HASH}+")
endif()
target_compile_definitions(core1_bridge PRIVATE
    MOKYA_CORE1_GIT_HASH="${MOKYA_CORE1_GIT_HASH}"
)

target_link_libraries(core1_bridge
    pico_stdlib
    pico_unique_id
    # pico_multicore — NOT linked; doorbell uses raw SIO register writes
    # to avoid pico_multicore's runtime_init side effects on Core 1.
    hardware_resets
    hardware_irq
    hardware_sync
    hardware_exception
    hardware_pio
    hardware_dma
    hardware_i2c
    hardware_watchdog
    tinyusb_device
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    lvgl
    lvgl_demos
)

# Phase 1.6 P2-11-mirror: wrap flash_range_erase/program so Core 1's own
# flash writes (e.g. LRU persist) park Core 0 before disabling XIP. See
# firmware/core1/src/ime/flash_safety_wrap.c for the protocol.
target_link_options(core1_bridge PRIVATE
    -Wl,--wrap=flash_range_erase
    -Wl,--wrap=flash_range_program
)

# Override the SDK's default memmap with our own — this is what redirects
# FLASH to 0x10200000 and DISCARDs .embedded_block.
pico_set_linker_script(core1_bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/memmap_core1_bridge.ld)

# Do NOT call pico_add_extra_outputs() — it runs picotool which expects an
# IMAGE_DEF block. We only need a raw .bin for `J-Link loadbin 0x10200000`.
add_custom_command(TARGET core1_bridge POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary $<TARGET_FILE:core1_bridge>
            $<TARGET_FILE_DIR:core1_bridge>/core1_bridge.bin
    COMMAND "${CMAKE_SIZE}"    $<TARGET_FILE:core1_bridge>
    VERBATIM
)
