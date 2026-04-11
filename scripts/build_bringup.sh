#!/usr/bin/env bash
# Build bringup firmware (i2c_custom_scan) for RP2350B.
# Run from project root. Requires: VS Build Tools 2019, ARM GCC, Ninja, Pico SDK at C:\pico-sdk.

set -e
cd "$(dirname "$0")/.."

powershell.exe -NoProfile -Command "
\$env:PICO_SDK_PATH = 'C:\pico-sdk'
\$env:PATH += ';C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-arm-none-eabi\bin;C:\ProgramData\chocolatey\bin'
\$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
cmd /c \"\`\"\$vcvars\`\" x64 && cmake --build build/firmware --target i2c_custom_scan --parallel\"
" 2>&1

echo ""
echo "=== Build output ==="
ls build/firmware/tools/bringup/i2c_custom_scan.elf 2>/dev/null && echo "OK: i2c_custom_scan.elf" || echo "ERROR: .elf not found"
