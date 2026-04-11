# MokyaLora Phase 2 M1.1-A — Core 1 m1_bridge (bare metal)
#
# Standalone bare-metal Cortex-M33 project. Does NOT use the Pico SDK.
# Compiles the shared ipc_ringbuf.c TU alongside the bridge main so the ring
# code is identical on both sides of the handshake.
#
# Build:
#   cmake -S firmware/core1/m1_bridge \
#         -B build/core1_bridge \
#         -DCMAKE_TOOLCHAIN_FILE=firmware/core1/m1_bridge/toolchain-arm-none-eabi.cmake \
#         -G Ninja
#   cmake --build build/core1_bridge

cmake_minimum_required(VERSION 3.20)

project(core1_bridge C ASM)

set(CMAKE_C_STANDARD   11)
set(CMAKE_C_EXTENSIONS OFF)

set(CPU_FLAGS
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=softfp
    -mfpu=fpv5-sp-d16
)

set(SHARED_IPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/ipc)

add_executable(core1_bridge
    src/main_core1_bridge.c
    ${SHARED_IPC_DIR}/ipc_ringbuf.c
)

target_include_directories(core1_bridge PRIVATE
    ${SHARED_IPC_DIR}
)

target_compile_options(core1_bridge PRIVATE
    ${CPU_FLAGS}
    -Os
    -ffunction-sections
    -fdata-sections
    -fno-common
    -Wall
    -Wextra
    -Werror
    -nostdlib
    # libc string.h is still visible (header-only), but we never call into
    # the libc implementation — __builtin_memcpy inlines and ipc_ringbuf.c's
    # memcpy calls are lowered by GCC to builtin copies for small sizes. If
    # the linker ever complains about an undefined memcpy we can switch to
    # --specs=nano.specs; for now -nostdlib keeps the image minimal.
)

target_link_options(core1_bridge PRIVATE
    ${CPU_FLAGS}
    -nostdlib
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map,$<TARGET_FILE:core1_bridge>.map
    -T${CMAKE_CURRENT_SOURCE_DIR}/memmap_core1_bridge.ld
)

set_target_properties(core1_bridge PROPERTIES
    SUFFIX ".elf"
)

# Emit a raw .bin alongside the ELF so scripts/flash can pass it straight
# through to J-Link loadbin at offset 0x10200000.
add_custom_command(TARGET core1_bridge POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O binary $<TARGET_FILE:core1_bridge> $<TARGET_FILE_DIR:core1_bridge>/core1_bridge.bin
    COMMAND "${CMAKE_SIZE}"    $<TARGET_FILE:core1_bridge>
    VERBATIM
)
