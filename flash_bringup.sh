#!/usr/bin/env bash
# Flash bringup firmware to RP2350B via J-Link SWD.
# Run from project root. Requires: J-Link Ultra V6 connected via SWD.

set -e
cd "$(dirname "$0")"

JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
ELF="$(cygpath -w "$(pwd)/build/firmware/tools/bringup/i2c_custom_scan.elf")"

if [ ! -f "build/firmware/tools/bringup/i2c_custom_scan.elf" ]; then
    echo "ERROR: .elf not found — run build_bringup.sh first"
    exit 1
fi

echo "Flashing: $ELF"

printf "connect\nr\nloadfile \"%s\"\nr\ng\nqc\n" "$ELF" > /tmp/jlink_flash.jlink

"$JLINK" \
    -device RP2350_M33_0 \
    -if SWD \
    -speed 4000 \
    -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_flash.jlink)" 2>&1 \
    | grep -E "O\.K\.|ownload|rror|Verify|bytes|speed"
