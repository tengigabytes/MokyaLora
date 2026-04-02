#!/usr/bin/env bash
# CMake configure for bringup firmware (run once, or after clean).
# Wipes build/firmware and reconfigures from scratch.
# Run from project root.

set -e
cd "$(dirname "$0")"

powershell.exe -NoProfile -Command "
\$env:PICO_SDK_PATH = 'C:\pico-sdk'
\$env:PATH += ';C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-arm-none-eabi\bin;C:\ProgramData\chocolatey\bin'
\$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
cmd /c \"\`\"\$vcvars\`\" x64 && rmdir /s /q build\firmware 2>nul & cmake -S firmware -B build/firmware -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/pico-sdk/cmake/preload/toolchains/pico_arm_cortex_m33_gcc.cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=none\"
" 2>&1

echo ""
echo "=== Configure done. Run build_bringup.sh to compile. ==="
