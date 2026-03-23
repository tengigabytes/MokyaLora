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
