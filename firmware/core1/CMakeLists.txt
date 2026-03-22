# firmware/core1 — Core 1 UI & Application Firmware (Apache-2.0)
# Contains the UI, IME integration, and application logic.
# This target is compiled for core 1 of the RP2350B.
#
# May #include firmware/shared/ipc/ipc_protocol.h (MIT — compatible with Apache-2.0).
# May link against firmware/mie (MIT — compatible with Apache-2.0).
# Must NOT #include any Meshtastic header or any file from firmware/core0/.

cmake_minimum_required(VERSION 3.20)

add_executable(mokya_core1
    # src/main_core1.cpp   # add when sources exist
)

target_include_directories(mokya_core1 PRIVATE
    ${CMAKE_SOURCE_DIR}/firmware/shared/ipc
)

target_link_libraries(mokya_core1
    mie   # MokyaInput Engine (MIT)
    # pico_stdlib pico_multicore lvgl freertos   # add when integrating
)
