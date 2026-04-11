#!/usr/bin/env bash
# Build core1_freertos_test and flash to RP2350B via J-Link.
# Step 16 Stage B: validates FreeRTOS SMP on Core 1.
# Run from project root.
#
# Expected serial output (connect at 115200 after flashing):
#   === Step 16 Stage B: Core 1 FreeRTOS ===
#   [Stage B] HEARTBEAT 1/5 — core 1 — ...
#   [Stage B] PASS — FreeRTOS task ran 5 heartbeats on Core 1

set -e
cd "$(dirname "$0")/.."

# --- Build ---
echo "=== Building core1_freertos_test ==="
powershell.exe -NoProfile -Command "
\$env:PICO_SDK_PATH = 'C:\pico-sdk'
\$env:PATH += ';C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-arm-none-eabi\bin;C:\ProgramData\chocolatey\bin'
\$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
cmd /c \"\`\"\$vcvars\`\" x64 && cmake --build build/firmware --target core1_freertos_test --parallel\"
" 2>&1

ELF="build/firmware/tools/bringup/core1_freertos_test.elf"
if [ ! -f "$ELF" ]; then
    echo "ERROR: .elf not found — build failed"
    exit 1
fi
echo "OK: $ELF"

# --- Flash ---
echo ""
echo "=== Flashing via J-Link ==="
JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
ELF_WIN="$(cygpath -w "$(pwd)/$ELF")"

printf "connect\nr\nloadfile \"%s\"\nr\ng\nqc\n" "$ELF_WIN" \
    > /tmp/jlink_flash_freertos.jlink

"$JLINK" \
    -device RP2350_M33_0 \
    -if SWD \
    -speed 4000 \
    -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_flash_freertos.jlink)" 2>&1 \
    | grep -E "O\.K\.|ownload|rror|Verify|bytes|speed"

echo ""
echo "=== Done. Connect serial monitor to see heartbeat output: ==="
echo "    .\\serial_monitor.ps1"
echo "    Expected: 5 x HEARTBEAT lines then 'Stage B PASS'"
echo "    After observing PASS, restore bringup firmware:"
echo "    bash scripts/build_and_flash_bringup.sh"
