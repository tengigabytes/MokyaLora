cmake_minimum_required(VERSION 3.20)

# Shared bringup sources (used by both Core 0 and Core 1 bringup targets)
set(BRINGUP_SOURCES
    i2c_custom_scan.c
    bringup_power.c
    bringup_led.c
    bringup_charger.c
    bringup_gauge.c
    bringup_sensors.c
    bringup_amp.c
    bringup_mic.c
    bringup_sram.c
    bringup_psram.c
    bringup_memory_tft.c
    bringup_flash.c
    bringup_lora.c
    bringup_tft.c
    bringup_tft_draw.c
    bringup_gnss_tft.c
    bringup_core1.c
    bringup_menu.c
)

# ── Core 0 bringup (normal: Core 0 owns all peripherals) ─────────────────────
add_executable(i2c_custom_scan
    ${BRINGUP_SOURCES}
    main_core0.c
)

# Generate the C header from the PIO assembler source.
# The header (i2s_out.pio.h) is placed in the build directory and included
# by i2c_custom_scan.c via the target's include path below.
pico_generate_pio_header(i2c_custom_scan
    ${CMAKE_CURRENT_LIST_DIR}/i2s_out.pio
)
pico_generate_pio_header(i2c_custom_scan
    ${CMAKE_CURRENT_LIST_DIR}/pdm_mic.pio
)
pico_generate_pio_header(i2c_custom_scan
    ${CMAKE_CURRENT_LIST_DIR}/tft_8080.pio
)

target_link_libraries(i2c_custom_scan
    pico_stdlib
    pico_multicore
    hardware_i2c
    hardware_pwm
    hardware_pio
    hardware_spi
    hardware_flash
    hardware_dma
    hardware_sync
    m
)

# Route stdio over USB CDC; disable UART
pico_enable_stdio_usb(i2c_custom_scan 1)
pico_enable_stdio_uart(i2c_custom_scan 0)

# Generate .uf2 / .elf / .bin / .map outputs
pico_add_extra_outputs(i2c_custom_scan)

# ── Step 14 standalone: boots directly into GNSS TFT display (no REPL) ──────
# Minimal dependency set: only bringup_power (bus_b/LM27965) + bringup_gnss_tft.
# No USB stdio required — exits via physical BACK key.
add_executable(gnss_tft_standalone
    bringup_power.c
    bringup_led.c
    bringup_charger.c
    bringup_gauge.c
    bringup_gnss_tft.c
    bringup_tft_draw.c
    bringup_menu.c
    gnss_tft_main.c
)

pico_generate_pio_header(gnss_tft_standalone
    ${CMAKE_CURRENT_LIST_DIR}/tft_8080.pio
)

target_link_libraries(gnss_tft_standalone
    pico_stdlib
    hardware_i2c
    hardware_pwm
    hardware_pio
    hardware_spi
    hardware_flash
    hardware_dma
    hardware_sync
    m
)

# USB stdio enabled but not required — printf visible when cable is plugged in.
pico_enable_stdio_usb(gnss_tft_standalone 1)
pico_enable_stdio_uart(gnss_tft_standalone 0)
pico_add_extra_outputs(gnss_tft_standalone)

# ── Step 16 Stage B-0: Core 1 bringup REPL (same code, Core 1 owns periph) ───
add_executable(core1_bringup_test
    ${BRINGUP_SOURCES}
    core1_bringup_test_main.c
)

pico_generate_pio_header(core1_bringup_test ${CMAKE_CURRENT_LIST_DIR}/i2s_out.pio)
pico_generate_pio_header(core1_bringup_test ${CMAKE_CURRENT_LIST_DIR}/pdm_mic.pio)
pico_generate_pio_header(core1_bringup_test ${CMAKE_CURRENT_LIST_DIR}/tft_8080.pio)

target_link_libraries(core1_bringup_test
    pico_stdlib
    pico_multicore
    hardware_i2c
    hardware_pwm
    hardware_pio
    hardware_spi
    hardware_flash
    hardware_dma
    hardware_sync
    m
)

pico_enable_stdio_usb(core1_bringup_test 1)
pico_enable_stdio_uart(core1_bringup_test 0)
pico_add_extra_outputs(core1_bringup_test)

# ── Step 16 Stage B: Core 1 FreeRTOS heartbeat test ───────────────────────────
# Separate ELF; not linked into the bringup REPL because vTaskStartScheduler()
# never returns on ARM Cortex-M.

set(FREERTOS_KERNEL_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core1/freertos-kernel"
    CACHE PATH "FreeRTOS Kernel path" FORCE)
set(FREERTOS_CONFIG_FILE_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE PATH "Directory containing FreeRTOSConfig.h" FORCE)

# Add the RP2350_ARM_NTZ SMP port.  Because pico_sdk_init() has already run
# at this point (SDK >= 1.3.2 / 2.x), the port's CMakeLists.txt detects the
# _pico_sdk_inclusion_marker target and includes library.cmake immediately,
# registering FreeRTOS-Kernel, FreeRTOS-Kernel-Heap{1..5}, etc.
add_subdirectory(
    "${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ"
    FreeRTOS-Kernel-RP2350
)

add_executable(core1_freertos_test
    core1_freertos_test.c
)

target_link_libraries(core1_freertos_test
    pico_stdlib
    pico_multicore
    pico_async_context_freertos
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

# Core 1 stack: default 2 KB is too small for vTaskStartScheduler().
target_compile_definitions(core1_freertos_test PRIVATE
    PICO_CORE1_STACK_SIZE=0x1000
)

pico_enable_stdio_usb(core1_freertos_test 1)
pico_enable_stdio_uart(core1_freertos_test 0)
pico_add_extra_outputs(core1_freertos_test)

# ── Step 16 Stages E–H: FreeRTOS multi-task + SPI + IPC + I2C on Core 1 ─────
add_executable(core1_rtos_efgh
    core1_rtos_efgh.c
)

target_link_libraries(core1_rtos_efgh
    pico_stdlib
    pico_multicore
    pico_async_context_freertos
    hardware_spi
    hardware_i2c
    hardware_sync
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

# Core 1 stack: 4 KB (same as Stage B2 — USB + FreeRTOS startup).
target_compile_definitions(core1_rtos_efgh PRIVATE
    PICO_CORE1_STACK_SIZE=0x1000
)

pico_enable_stdio_usb(core1_rtos_efgh 1)
pico_enable_stdio_uart(core1_rtos_efgh 0)
pico_add_extra_outputs(core1_rtos_efgh)
