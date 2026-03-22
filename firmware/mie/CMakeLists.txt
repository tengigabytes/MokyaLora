cmake_minimum_required(VERSION 3.20)

# MokyaInput Engine (MIE) — built as a portable static library.
# This CMakeLists.txt has NO dependency on the Pico SDK or any
# platform-specific headers. Platform glue lives in hal/rp2350/ or hal/pc/
# and is added by the parent build when targeting a specific platform.

project(mie C CXX)
set(CMAKE_CXX_STANDARD 11)

add_library(mie STATIC
    # Core layer — add source files here as they are created
    # src/trie_searcher.cpp
    # src/ime_logic.cpp
)

target_include_directories(mie PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Unit tests (host build only — not included when cross-compiling for RP2350)
if(CMAKE_CROSSCOMPILING STREQUAL "" OR NOT CMAKE_CROSSCOMPILING)
    # add_subdirectory(tests)
endif()
