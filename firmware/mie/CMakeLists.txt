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

# Host-only targets (REPL tool + unit tests)
if(NOT CMAKE_CROSSCOMPILING)
    # Interactive REPL — virtual half-keyboard + IME output
    add_executable(mie_repl
        tools/mie_repl.cpp
        hal/pc/hal_pc_stdin.cpp
    )
    target_link_libraries(mie_repl PRIVATE mie)
    target_include_directories(mie_repl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # Unit tests
    enable_testing()
    add_subdirectory(tests)
endif()
