#!/usr/bin/env bash
# Flash core1_bringup_test — bringup REPL running on Core 1.
# Step 16 Stage B-0: validates Core 1 peripheral + USB CDC access.
# Run from project root.

set -e
cd "$(dirname "$0")"

ELF="build/firmware/tools/bringup/core1_bringup_test.elf"
if [ ! -f "$ELF" ]; then
    echo "ERROR: .elf not found — run build first"
    exit 1
fi
echo "Flashing: $ELF"

JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
ELF_WIN="$(cygpath -w "$(pwd)/$ELF")"

printf "connect\nr\nloadfile \"%s\"\nr\ng\nqc\n" "$ELF_WIN" \
    > /tmp/jlink_flash_core1_bringup.jlink

"$JLINK" \
    -device RP2350_M33_0 \
    -if SWD \
    -speed 4000 \
    -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_flash_core1_bringup.jlink)" 2>&1 \
    | grep -E "O\.K\.|ownload|rror|Verify|bytes|Skipped"

echo ""
echo "=== Done. Connect serial: .\\bringup_run.ps1 help ==="
