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
set(PICO_BOARD pico2 CACHE STRING "Pico board (closest match for RP2350B)")

include(pico_sdk_import.cmake)

project(core1_bridge C CXX ASM)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

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
set(LV_CONF_BUILD_DISABLE_EXAMPLES       ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS          ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lvgl" lvgl_build)

# LVGL's FreeRTOS OSAL needs FreeRTOS headers at compile time.
target_link_libraries(lvgl PUBLIC FreeRTOS-Kernel)

# ── Shared IPC TU ────────────────────────────────────────────────────────────
set(SHARED_IPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ipc)

# ── Display driver (M3) ──────────────────────────────────────────────────────
set(DISPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/display)

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
    ${DISPLAY_DIR}/display.c
    ${DISPLAY_DIR}/st7789vi.c
    ${DISPLAY_DIR}/lvgl_glue.c
)

# Generate tft_8080.pio.h next to the .pio source via pioasm.
pico_generate_pio_header(core1_bridge
    ${DISPLAY_DIR}/tft_8080.pio
    OUTPUT_DIR ${DISPLAY_DIR}
)

target_include_directories(core1_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SHARED_IPC_DIR}
    ${DISPLAY_DIR}
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
    tinyusb_device
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    lvgl
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
