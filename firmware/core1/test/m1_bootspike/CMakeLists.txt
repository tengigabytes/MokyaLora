# MokyaLora Phase 2 M1.0b — Core 1 bootspike
#
# Standalone bare-metal Cortex-M33 project. Does NOT use the Pico SDK — the
# spike touches no peripherals and needs no runtime, so pulling in pico_sdk
# would only add boot2, XIP setup, and libc initialisation that Core 1 must
# avoid (Core 0 has already configured XIP when this image starts).
#
# Build:
#   cmake -S firmware/core1/test/m1_bootspike \
#         -B build/core1_bootspike \
#         -DCMAKE_TOOLCHAIN_FILE=firmware/core1/test/m1_bootspike/toolchain-arm-none-eabi.cmake \
#         -G Ninja
#   cmake --build build/core1_bootspike

cmake_minimum_required(VERSION 3.20)

project(core1_bootspike C ASM)

set(CMAKE_C_STANDARD   11)
set(CMAKE_C_EXTENSIONS OFF)

set(CPU_FLAGS
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=softfp
    -mfpu=fpv5-sp-d16
)

add_executable(core1_bootspike
    src/main_core1_bootspike.c
)

target_compile_options(core1_bootspike PRIVATE
    ${CPU_FLAGS}
    -Os
    -ffunction-sections
    -fdata-sections
    -fno-common
    -Wall
    -Wextra
    -Werror
    -nostdlib
)

target_link_options(core1_bootspike PRIVATE
    ${CPU_FLAGS}
    -nostdlib
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map,$<TARGET_FILE:core1_bootspike>.map
    -T${CMAKE_CURRENT_SOURCE_DIR}/memmap_core1_bootspike.ld
)

set_target_properties(core1_bootspike PROPERTIES
    SUFFIX ".elf"
)

# Emit a raw .bin alongside the ELF so it's easy to inspect with hexdump.
add_custom_command(TARGET core1_bootspike POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary $<TARGET_FILE:core1_bootspike> $<TARGET_FILE_DIR:core1_bootspike>/core1_bootspike.bin
    COMMAND "${CMAKE_SIZE}"    $<TARGET_FILE:core1_bootspike>
    VERBATIM
)
