# MokyaLora Phase 2 M1.1-A — bare-metal arm-none-eabi toolchain file
# Used only by the Core 1 m1_bridge project; does not affect Core 0 build.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX "C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PREFIX}gcc.exe")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}gcc.exe")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_PREFIX}objcopy.exe" CACHE INTERNAL "")
set(CMAKE_SIZE         "${TOOLCHAIN_PREFIX}size.exe"    CACHE INTERNAL "")

# Don't try to link a test executable during compiler detection — there's no
# crt0 or libc on the bare-metal target.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
