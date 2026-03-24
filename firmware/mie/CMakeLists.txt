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

# ── Optional GUI test tool (Dear ImGui + SDL2 + OpenGL 3) ─────────────────
option(MIE_BUILD_GUI "Build mie_gui graphical test tool (requires Dear ImGui + SDL2 + OpenGL)" OFF)

if(MIE_BUILD_GUI AND NOT CMAKE_CROSSCOMPILING)
    include(FetchContent)

    # Dear ImGui — no CMakeLists.txt; create target manually after fetch
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui)

    # SDL2 2.26.x — static build, strip unnecessary subsystems.
    # Using 2.26.5 rather than 2.30.x: the newer branch has a CMake bug
    # where the install section references the SDL2 shared target even
    # when SDL_SHARED=OFF, causing a configure-time error.
    set(SDL_SHARED   OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC   ON  CACHE BOOL "" FORCE)
    set(SDL_TEST     OFF CACHE BOOL "" FORCE)
    set(SDL_AUDIO    OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC   OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR   OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI   OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.26.5
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL2)

    find_package(OpenGL REQUIRED)

    # ImGui static library target (Dear ImGui has no CMakeLists.txt)
    add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_lib PUBLIC SDL2::SDL2-static OpenGL::GL)
    if(MSVC)
        target_compile_options(imgui_lib PRIVATE /utf-8 /W0)
    else()
        target_compile_options(imgui_lib PRIVATE -w)
    endif()

    # mie_gui executable
    add_executable(mie_gui tools/mie_gui.cpp)
    target_compile_definitions(mie_gui PRIVATE SDL_MAIN_HANDLED)
    if(MSVC)
        target_compile_options(mie_gui PRIVATE /utf-8)
    endif()
    target_include_directories(mie_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/hal
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(mie_gui PRIVATE mie imgui_lib)
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
