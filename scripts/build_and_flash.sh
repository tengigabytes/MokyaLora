#!/usr/bin/env bash
# Build and flash dual-image firmware (Core 0 Meshtastic + Core 1 bridge)
# to RP2350B via J-Link SWD.
#
# Run from project root:
#   bash scripts/build_and_flash.sh          # build + flash both
#   bash scripts/build_and_flash.sh --core1  # build + flash Core 1 only
#
# Requires: PlatformIO, VS Build Tools 2019, ARM GCC, Ninja, Pico SDK,
#           J-Link Ultra connected via SWD.

set -e
cd "$(dirname "$0")/.."

JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
CORE1_BIN="build/core1_bridge/core1_bridge.bin"
CORE1_ONLY=false

if [ "$1" = "--core1" ]; then
    CORE1_ONLY=true
fi

# ── Core 0: PlatformIO build ────────────────────────────────────────────
if [ "$CORE1_ONLY" = false ]; then
    echo "=== Building Core 0 (Meshtastic via PlatformIO) ==="
    python -m platformio run -e rp2350b-mokya \
        -d firmware/core0/meshtastic 2>&1
    CORE0_ELF="$(ls firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware*.elf 2>/dev/null | head -1)"
    if [ -z "$CORE0_ELF" ]; then
        echo "ERROR: Core 0 .elf not found — PlatformIO build failed"
        exit 1
    fi
    echo "OK: $CORE0_ELF"
fi

# ── Core 1: CMake/Ninja build ───────────────────────────────────────────
echo ""
echo "=== Building Core 1 (m1_bridge via CMake) ==="
cmake --build build/core1_bridge 2>&1
if [ ! -f "$CORE1_BIN" ]; then
    echo "ERROR: $CORE1_BIN not found — CMake build failed"
    exit 1
fi
echo "OK: $CORE1_BIN"

# ── Flash via J-Link ────────────────────────────────────────────────────
echo ""
echo "=== Flashing via J-Link ==="

JLINK_SCRIPT="/tmp/jlink_flash_dual.jlink"
CORE1_BIN_WIN="$(cygpath -w "$(pwd)/$CORE1_BIN")"

{
    echo "connect"
    echo "r"
    if [ "$CORE1_ONLY" = false ]; then
        CORE0_ELF_WIN="$(cygpath -w "$(pwd)/$CORE0_ELF")"
        printf 'loadfile "%s"\n' "$CORE0_ELF_WIN"
    fi
    printf 'loadbin "%s" 0x10200000\n' "$CORE1_BIN_WIN"
    echo "r"
    echo "g"
    echo "qc"
} > "$JLINK_SCRIPT"

"$JLINK" \
    -device RP2350_M33_0 \
    -if SWD \
    -speed 4000 \
    -autoconnect 1 \
    -CommanderScript "$(cygpath -w "$JLINK_SCRIPT")" 2>&1

echo ""
echo "=== Done. Wait ~3 s for USB CDC re-enumeration. ==="
