# firmware/core0 — Core 0 Modem Firmware (GPL-3.0)
# Integrates the Meshtastic protocol stack.
# This target is compiled for core 0 of the RP2350B.
#
# May #include firmware/shared/ipc/ipc_protocol.h (MIT — compatible with GPL-3.0).
# Must NOT #include any header from firmware/core1/ or firmware/mie/.

cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/main_core0.cpp)
    return()  # no sources yet — skip until Phase 2
endif()

add_executable(mokya_core0
    src/main_core0.cpp
)

target_include_directories(mokya_core0 PRIVATE
    ${CMAKE_SOURCE_DIR}/firmware/shared/ipc
)

# Link against Pico SDK and Meshtastic (add when integrating)
# target_link_libraries(mokya_core0 pico_stdlib pico_multicore meshtastic)
