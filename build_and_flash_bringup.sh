#!/usr/bin/env bash
# Build bringup firmware and flash to RP2350B via J-Link.
# Run from project root. Requires: J-Link connected via SWD.

set -e
cd "$(dirname "$0")"

# --- Build ---
echo "=== Building i2c_custom_scan ==="
powershell.exe -NoProfile -Command "
\$env:PICO_SDK_PATH = 'C:\pico-sdk'
\$env:PATH += ';C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-arm-none-eabi\bin;C:\ProgramData\chocolatey\bin'
\$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
cmd /c \"\`\"\$vcvars\`\" x64 && cmake --build build/firmware --target i2c_custom_scan --parallel\"
" 2>&1

if [ ! -f "build/firmware/tools/bringup/i2c_custom_scan.elf" ]; then
    echo "ERROR: .elf not found — build failed"
    exit 1
fi
echo "OK: i2c_custom_scan.elf"

# --- Flash ---
echo ""
echo "=== Flashing via J-Link ==="
JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
ELF="$(cygpath -w "$(pwd)/build/firmware/tools/bringup/i2c_custom_scan.elf")"

printf "connect\nr\nloadfile \"%s\"\nr\ng\nqc\n" "$ELF" > /tmp/jlink_flash.jlink

"$JLINK" \
    -device RP2350_M33_0 \
    -if SWD \
    -speed 4000 \
    -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_flash.jlink)" 2>&1 \
    | grep -E "O\.K\.|ownload|rror|Verify|bytes|speed"

echo ""
echo "=== Done. Run: .\\serial_monitor.ps1 key ==="
