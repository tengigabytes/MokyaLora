cmake_minimum_required(VERSION 3.20)

add_executable(i2c_custom_scan
    i2c_custom_scan.c
    bringup_power.c
    bringup_sensors.c
    bringup_audio.c
    bringup_flash.c
    bringup_lora.c
    bringup_tft.c
    OpenPDMFilter.c
    # WAV audio data — uncomment + set BRINGUP_WAV=1 in bringup.h to enable (~12 MB flash)
    # voice_pcm.c
    # gc_pcm.c
    # test01_8k_pcm.c
    # test01_16k_pcm.c
    # test01_44k_pcm.c
    # test01_48k_pcm.c
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
    hardware_i2c
    hardware_pwm
    hardware_pio
    hardware_spi
    hardware_flash
    m
)

# PICO_BUILD activates the per-instance Gain field in OpenPDMFilter
target_compile_definitions(i2c_custom_scan PRIVATE PICO_BUILD)

# Route stdio over USB CDC; disable UART
pico_enable_stdio_usb(i2c_custom_scan 1)
pico_enable_stdio_uart(i2c_custom_scan 0)

# Generate .uf2 / .elf / .bin / .map outputs
pico_add_extra_outputs(i2c_custom_scan)
