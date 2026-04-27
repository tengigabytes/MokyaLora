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

# src/mie_c_api.cpp is excluded until Phase D rewrites it against the
# v2 listener API (set_listener / pending_view / candidate accessors).
# Core 1 uses C++ ImeLogic directly via <mie/ime_logic.h>, so the C API
# is only needed by external bindings (Android JNI, Windows TSF).
add_library(mie STATIC
    src/mie_init.cpp
    src/trie_searcher.cpp
    src/composition_searcher.cpp
    src/ime_logic.cpp
    src/ime_keys.cpp
    src/ime_search.cpp
    src/ime_display.cpp
    src/ime_commit.cpp
    src/ime_smart.cpp
    src/ime_direct.cpp
    src/lru_cache.cpp
    src/utf8.c
)

target_include_directories(mie PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Host-only targets: REPL + unit tests.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(mie_repl
        tools/mie_repl.cpp
        hal/pc/hal_pc_stdin.cpp
    )
    target_link_libraries(mie_repl PRIVATE mie)
    target_include_directories(mie_repl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/hal
    )

    enable_testing()
    add_subdirectory(tests)
endif()

# ── Data pipeline — optional host targets (run manually, need data sources) ──
# These targets call the Python compilers to regenerate the .bin assets.
# They are NOT part of the default build and do NOT block cross-compilation.
#
# Two build variants:
#   sm (small)  — for RP2350 Flash (embedded); charlist-subsetted font
#   lg (large)  — for PC testing; full Unicode ranges
#
# Usage (small variant):
#   cmake -DMIE_DATA_SOURCES_DIR=/path/to/data_sources \
#         -DMIE_UNIFONT_OTF=/path/to/unifont-17.0.04.otf \
#         -S firmware/mie -B build/mie-host
#   cmake --build build/mie-host --target mie_data_sm
#
# Usage (large variant):
#   cmake -DMIE_DATA_SOURCES_DIR=/path/to/data_sources \
#         -DMIE_UNIFONT_OTF=/path/to/unifont-17.0.04.otf \
#         -S firmware/mie -B build/mie-host
#   cmake --build build/mie-host --target mie_data_lg
#
# Download data sources first (requires internet, run once):
#   cmake --build build/mie-host --target mie_fetch_data

if(NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 QUIET COMPONENTS Interpreter)

    if(Python3_FOUND)
        set(MIE_DATA_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/data")
        set(MIE_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools")

        set(MIE_DATA_SOURCES_DIR "" CACHE PATH
            "Directory containing downloaded data sources (tsi.csv, en_words.txt, unifont.otf). Run target mie_fetch_data to populate.")
        set(MIE_UNIFONT_OTF "" CACHE FILEPATH
            "Path to GNU Unifont .otf/.ttf file (required for font data targets)")
        set(MIE_MOE_CSV "" CACHE FILEPATH
            "Path to MoE dictionary CSV (optional supplement)")

        # ── Download helper (EXCLUDE_FROM_ALL — requires internet) ────────
        add_custom_target(mie_fetch_data
            COMMAND ${Python3_EXECUTABLE}
                    "${MIE_TOOLS_DIR}/fetch_data.py"
                    --data-dir  "${MIE_DATA_SOURCES_DIR}"
                    --skip-if-exists
            COMMENT "Downloading MIE data sources (requires internet access)"
            VERBATIM)
        set_target_properties(mie_fetch_data PROPERTIES EXCLUDE_FROM_ALL TRUE)

        # ── Small variant ─────────────────────────────────────────────────
        # mie_dict_data_sm: Chinese + English (max 3000 words) + charlist
        # mie_font_data_sm: charlist-subset font (depends on mie_dict_data_sm)
        if(MIE_DATA_SOURCES_DIR)
            # sm variant dictionary knobs — tuned 2026-04-19 to fit the
            # ~5 MB PSRAM slot while keeping the most-used ZH vocabulary
            # (freq>=5 covers 112k / 160k libchewing entries; per-key=5 prunes
            # long-tail homophones; abbr<=2 keeps fast-typing for 2-syllable
            # phrases). ZH total ~4.7 MB, EN total ~80 KB. See
            # project_mie_dict_compression.md memory for future format-level
            # compression that would shrink this further.
            set(_sm_dict_args
                --libchewing  "${MIE_DATA_SOURCES_DIR}/tsi.csv"
                --en-wordlist "${MIE_DATA_SOURCES_DIR}/google-10000-english-no-swears.txt"
                --en-max-words 3000
                --zh-min-freq 5
                --zh-max-abbr-syls 2
                --zh-max-per-key 5
                --emit-charlist "${MIE_DATA_DIR}/charlist.txt"
                --output-dir "${MIE_DATA_DIR}")
            set(_sm_dict_depends
                "${MIE_TOOLS_DIR}/gen_dict.py"
                "${MIE_DATA_SOURCES_DIR}/tsi.csv"
                "${MIE_DATA_SOURCES_DIR}/google-10000-english-no-swears.txt")
            if(MIE_MOE_CSV)
                list(APPEND _sm_dict_args --moe-csv "${MIE_MOE_CSV}")
                list(APPEND _sm_dict_depends "${MIE_MOE_CSV}")
            endif()

            add_custom_command(
                OUTPUT  "${MIE_DATA_DIR}/dict_dat.bin"
                        "${MIE_DATA_DIR}/dict_values.bin"
                        "${MIE_DATA_DIR}/en_dat.bin"
                        "${MIE_DATA_DIR}/en_values.bin"
                        "${MIE_DATA_DIR}/dict_meta.json"
                        "${MIE_DATA_DIR}/charlist.txt"
                COMMAND ${Python3_EXECUTABLE}
                        "${MIE_TOOLS_DIR}/gen_dict.py"
                        ${_sm_dict_args}
                DEPENDS ${_sm_dict_depends}
                COMMENT "Generating small MIE dictionary + charlist"
                VERBATIM)
            add_custom_target(mie_dict_data_sm
                DEPENDS "${MIE_DATA_DIR}/charlist.txt")

            # Backward-compat alias
            add_custom_target(mie_dict_data DEPENDS mie_dict_data_sm)
        else()
            add_custom_target(mie_dict_data_sm
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Set -DMIE_DATA_SOURCES_DIR=<path> and re-run cmake, or run target mie_fetch_data."
                COMMENT "mie_dict_data_sm: MIE_DATA_SOURCES_DIR not set")
            add_custom_target(mie_dict_data DEPENDS mie_dict_data_sm)
        endif()

        if(MIE_UNIFONT_OTF AND MIE_DATA_SOURCES_DIR)
            add_custom_command(
                OUTPUT  "${MIE_DATA_DIR}/mie_unifont_sm_16.bin"
                COMMAND ${Python3_EXECUTABLE}
                        "${MIE_TOOLS_DIR}/gen_font.py"
                        --unifont  "${MIE_UNIFONT_OTF}"
                        --charlist "${MIE_DATA_DIR}/charlist.txt"
                        --out      "${MIE_DATA_DIR}/mie_unifont_sm_16.bin"
                        --no-subset
                DEPENDS "${MIE_TOOLS_DIR}/gen_font.py"
                        "${MIE_UNIFONT_OTF}"
                        "${MIE_DATA_DIR}/charlist.txt"
                COMMENT "Generating small MIE font (charlist-subset)"
                VERBATIM)
            add_custom_target(mie_font_data_sm
                DEPENDS "${MIE_DATA_DIR}/mie_unifont_sm_16.bin")
            add_dependencies(mie_font_data_sm mie_dict_data_sm)

            # Backward-compat alias
            add_custom_target(mie_font_data DEPENDS mie_font_data_sm)
        else()
            add_custom_target(mie_font_data_sm
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Set -DMIE_UNIFONT_OTF=<path> and -DMIE_DATA_SOURCES_DIR=<path> and re-run cmake."
                COMMENT "mie_font_data_sm: MIE_UNIFONT_OTF or MIE_DATA_SOURCES_DIR not set")
            add_custom_target(mie_font_data DEPENDS mie_font_data_sm)
        endif()

        # MDBL blob: packs the four sm .bin sections into a single
        # flash-resident image. Consumed by Core 1 at boot (copy to PSRAM
        # → mie_dict_open_memory). Flashed as an independent partition
        # by scripts/build_and_flash.sh at MIE_DICT_PARTITION_ADDR.
        # J-Link's loadbin rejects unknown file extensions — keep .bin.
        set(MIE_DICT_BLOB "${CMAKE_BINARY_DIR}/dict.bin")
        add_custom_command(
            OUTPUT  "${MIE_DICT_BLOB}"
            COMMAND ${Python3_EXECUTABLE}
                    "${MIE_TOOLS_DIR}/pack_dict_blob.py"
                    --zh-dat "${MIE_DATA_DIR}/dict_dat.bin"
                    --zh-val "${MIE_DATA_DIR}/dict_values.bin"
                    --en-dat "${MIE_DATA_DIR}/en_dat.bin"
                    --en-val "${MIE_DATA_DIR}/en_values.bin"
                    --out    "${MIE_DICT_BLOB}"
            DEPENDS "${MIE_TOOLS_DIR}/pack_dict_blob.py"
                    "${MIE_DATA_DIR}/dict_dat.bin"
                    "${MIE_DATA_DIR}/dict_values.bin"
                    "${MIE_DATA_DIR}/en_dat.bin"
                    "${MIE_DATA_DIR}/en_values.bin"
            COMMENT "Packing MDBL dictionary blob for Core 1"
            VERBATIM)
        add_custom_target(mie_dict_blob
            DEPENDS "${MIE_DICT_BLOB}")
        add_dependencies(mie_dict_blob mie_dict_data_sm)

        add_custom_target(mie_data_sm
            DEPENDS mie_dict_data_sm mie_font_data_sm mie_dict_blob
            COMMENT "Build small MIE data assets (embedded RP2350 variant)")

        # ── Large variant ─────────────────────────────────────────────────
        # mie_dict_data_lg: all English words, no charlist emit
        # mie_font_data_lg: full Unicode ranges (for PC testing)
        #
        # Outputs go to MIE_DATA_DIR/lg/ — NOT the top-level MIE_DATA_DIR.
        # The `lg` variant emits identically-named files (dict_dat.bin,
        # dict_values.bin) as `sm` but without the freq/per-key filtering
        # knobs, so sharing the directory silently overwrites the sm
        # outputs with 89 MB blobs. Root cause for MIE_DICT_LOAD_ERR_GEOMETRY
        # bricks seen on-device before 2026-04-23.
        if(MIE_DATA_SOURCES_DIR)
            set(_lg_dict_args
                --libchewing  "${MIE_DATA_SOURCES_DIR}/tsi.csv"
                --en-wordlist "${MIE_DATA_SOURCES_DIR}/google-10000-english-no-swears.txt"
                --output-dir  "${MIE_DATA_DIR}/lg")
            set(_lg_dict_depends
                "${MIE_TOOLS_DIR}/gen_dict.py"
                "${MIE_DATA_SOURCES_DIR}/tsi.csv"
                "${MIE_DATA_SOURCES_DIR}/google-10000-english-no-swears.txt")
            if(MIE_MOE_CSV)
                list(APPEND _lg_dict_args --moe-csv "${MIE_MOE_CSV}")
                list(APPEND _lg_dict_depends "${MIE_MOE_CSV}")
            endif()

            add_custom_target(mie_dict_data_lg
                COMMAND ${Python3_EXECUTABLE}
                        "${MIE_TOOLS_DIR}/gen_dict.py"
                        ${_lg_dict_args}
                DEPENDS ${_lg_dict_depends}
                COMMENT "Generating large MIE dictionary (full English word list)"
                VERBATIM)
        else()
            add_custom_target(mie_dict_data_lg
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Set -DMIE_DATA_SOURCES_DIR=<path> and re-run cmake."
                COMMENT "mie_dict_data_lg: MIE_DATA_SOURCES_DIR not set")
        endif()

        if(MIE_UNIFONT_OTF)
            # lg font also lives under MIE_DATA_DIR/lg/ for parity with the
            # lg dict (see note above about sm/lg filename collision).
            add_custom_command(
                OUTPUT  "${MIE_DATA_DIR}/lg/mie_unifont_16.bin"
                COMMAND ${Python3_EXECUTABLE}
                        "${MIE_TOOLS_DIR}/gen_font.py"
                        --unifont "${MIE_UNIFONT_OTF}"
                        --out     "${MIE_DATA_DIR}/lg/mie_unifont_16.bin"
                DEPENDS "${MIE_TOOLS_DIR}/gen_font.py" "${MIE_UNIFONT_OTF}"
                COMMENT "Generating large MIE font (full Unicode ranges)"
                VERBATIM)
            add_custom_target(mie_font_data_lg
                DEPENDS "${MIE_DATA_DIR}/lg/mie_unifont_16.bin")
        else()
            add_custom_target(mie_font_data_lg
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Set -DMIE_UNIFONT_OTF=<path> and re-run cmake."
                COMMENT "mie_font_data_lg: MIE_UNIFONT_OTF not set")
        endif()

        add_custom_target(mie_data_lg
            DEPENDS mie_dict_data_lg mie_font_data_lg
            COMMENT "Build large MIE data assets (PC testing variant)")

        # ── Umbrella target (defaults to small variant) ───────────────────
        add_custom_target(mie_data
            DEPENDS mie_data_sm
            COMMENT "Build MIE data assets (alias for mie_data_sm)")

    endif() # Python3_FOUND
endif() # NOT CMAKE_CROSSCOMPILING
