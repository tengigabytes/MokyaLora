cmake_minimum_required(VERSION 3.20)

# MokyaInput Engine (MIE) — built as a portable static library.
# This CMakeLists.txt has NO dependency on the Pico SDK or any
# platform-specific headers. Platform glue lives in hal/rp2350/ or hal/pc/
# and is added by the parent build when targeting a specific platform.

project(mie C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC: treat all source files as UTF-8 and suppress code-page warnings
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(mie STATIC
    src/mie_init.cpp        # keeps the library non-empty while all modules are present
    src/trie_searcher.cpp
    src/ime_logic.cpp
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
    target_include_directories(mie_repl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/hal
    )

    # Unit tests
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Data pipeline — optional host targets (run manually, need data sources) ──
# These targets call the Python compilers to regenerate the .bin assets.
# They are NOT part of the default build and do NOT block cross-compilation.
#
# Usage:
#   cmake -DMIE_UNIFONT_TTF=/path/to/unifont.ttf \
#         -DMIE_LIBCHEWING=/path/to/tsi.src       \
#         -DMIE_MOE_CSV=/path/to/moe.csv          \
#         -DMIE_EN_WORDLIST=/path/to/words.txt    \
#         -S firmware/mie -B build/mie-host
#   cmake --build build/mie-host --target mie_data

if(NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 QUIET COMPONENTS Interpreter)

    if(Python3_FOUND)
        set(MIE_DATA_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/data")
        set(MIE_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools")

        # ── Font: mie_unifont_16.bin ──────────────────────────────────────
        set(MIE_UNIFONT_TTF "" CACHE FILEPATH
            "Path to GNU Unifont .ttf (required for mie_font_data target)")

        if(MIE_UNIFONT_TTF)
            add_custom_command(
                OUTPUT  "${MIE_DATA_DIR}/mie_unifont_16.bin"
                COMMAND ${Python3_EXECUTABLE}
                        "${MIE_TOOLS_DIR}/gen_font.py"
                        --unifont "${MIE_UNIFONT_TTF}"
                        --out     "${MIE_DATA_DIR}/mie_unifont_16.bin"
                DEPENDS "${MIE_TOOLS_DIR}/gen_font.py" "${MIE_UNIFONT_TTF}"
                COMMENT "Generating mie_unifont_16.bin from GNU Unifont TTF"
                VERBATIM)
            add_custom_target(mie_font_data
                DEPENDS "${MIE_DATA_DIR}/mie_unifont_16.bin")
        else()
            add_custom_target(mie_font_data
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Set -DMIE_UNIFONT_TTF=/path/to/unifont.ttf and re-run cmake."
                COMMENT "mie_font_data: MIE_UNIFONT_TTF not set")
        endif()

        # ── Dictionary: dict_dat.bin / en_dat.bin ─────────────────────────
        set(MIE_LIBCHEWING  "" CACHE FILEPATH "Path to libchewing-data tsi.src")
        set(MIE_MOE_CSV     "" CACHE FILEPATH "Path to MoE dictionary CSV")
        set(MIE_EN_WORDLIST "" CACHE FILEPATH "Path to English word list")

        set(_dict_args "")
        set(_dict_depends "${MIE_TOOLS_DIR}/gen_dict.py")
        if(MIE_LIBCHEWING)
            list(APPEND _dict_args --libchewing "${MIE_LIBCHEWING}")
            list(APPEND _dict_depends "${MIE_LIBCHEWING}")
        endif()
        if(MIE_MOE_CSV)
            list(APPEND _dict_args --moe-csv "${MIE_MOE_CSV}")
            list(APPEND _dict_depends "${MIE_MOE_CSV}")
        endif()
        if(MIE_EN_WORDLIST)
            list(APPEND _dict_args --en-wordlist "${MIE_EN_WORDLIST}")
            list(APPEND _dict_depends "${MIE_EN_WORDLIST}")
        endif()

        if(_dict_args)
            add_custom_command(
                OUTPUT  "${MIE_DATA_DIR}/dict_dat.bin"
                        "${MIE_DATA_DIR}/dict_values.bin"
                        "${MIE_DATA_DIR}/dict_meta.json"
                COMMAND ${Python3_EXECUTABLE}
                        "${MIE_TOOLS_DIR}/gen_dict.py"
                        ${_dict_args}
                        --output-dir "${MIE_DATA_DIR}"
                DEPENDS ${_dict_depends}
                COMMENT "Generating MIE dictionary binaries"
                VERBATIM)
            add_custom_target(mie_dict_data
                DEPENDS "${MIE_DATA_DIR}/dict_dat.bin")
        else()
            add_custom_target(mie_dict_data
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Set MIE_LIBCHEWING / MIE_MOE_CSV / MIE_EN_WORDLIST and re-run cmake."
                COMMENT "mie_dict_data: no data sources configured")
        endif()

        # ── Umbrella target ───────────────────────────────────────────────
        add_custom_target(mie_data
            DEPENDS mie_font_data mie_dict_data
            COMMENT "Build all MIE data assets (font + dictionary)")

    endif() # Python3_FOUND
endif() # NOT CMAKE_CROSSCOMPILING
