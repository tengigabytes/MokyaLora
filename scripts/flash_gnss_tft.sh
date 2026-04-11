#!/usr/bin/env bash
# Flash gnss_tft_standalone firmware to RP2350B via J-Link SWD.
# Run from project root. No USB cable needed after flash.
# On boot the TFT shows live GPS data; press BACK key to exit.

set -e
cd "$(dirname "$0")/.."

JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
ELF="build/firmware/tools/bringup/gnss_tft_standalone.elf"
ELF_WIN="$(cygpath -w "$(pwd)/$ELF")"

if [ ! -f "$ELF" ]; then
    echo "ERROR: $ELF not found — run build_bringup.sh first"
    exit 1
fi

echo "Flashing: $ELF_WIN"

printf 'connect\nr\nloadfile "%s"\nr\ng\nqc\n' "$ELF_WIN" > /tmp/jlink_flash_gnss.jlink

"$JLINK" \
    -device RP2350_M33_0 \
    -if SWD \
    -speed 4000 \
    -autoconnect 1 \
    -CommanderScript "$(cygpath -w /tmp/jlink_flash_gnss.jlink)" 2>&1 \
    | grep -E "O\.K\.|ownload|rror|Verify|bytes|speed"

echo "Done. Disconnect J-Link and USB; device starts GPS display on power-up."
