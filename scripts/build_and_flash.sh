#!/usr/bin/env bash
# Build and flash dual-image firmware (Core 0 Meshtastic + Core 1 bridge)
# plus the MIE dictionary blob to RP2350B via J-Link SWD.
#
# Flash layout (W25Q128JW, 16 MB):
#   0x10000000  Core 0 Meshtastic image   (2 MB slot)
#   0x10200000  Core 1 bridge image       (2 MB slot)
#   0x10400000  MIE dictionary (MDBL blob, 6 MB reserved, ~5 MB used)
#   0x10A00000  MIE font blob (MIEF, 2 MB reserved)
#   0x10C00000  LittleFS / free           (4 MB)
#
# Run from project root:
#   bash scripts/build_and_flash.sh          # build + flash everything (MDBL v2 dict)
#   bash scripts/build_and_flash.sh --core1  # build + flash Core 1 + assets
#   bash scripts/build_and_flash.sh --dict   # flash dict blob only
#   bash scripts/build_and_flash.sh --font   # flash font blob only
#   bash scripts/build_and_flash.sh --v4     # flash MIED v4 (composition) dict
#                                            # in place of MDBL — Core 1 firmware
#                                            # auto-detects via MIE4 magic
#
# Requires: PlatformIO, VS Build Tools 2019, ARM GCC, Ninja, Pico SDK,
#           J-Link Ultra connected via SWD.

set -e
cd "$(dirname "$0")/.."

JLINK="C:/Program Files/SEGGER/JLink_V932/JLink.exe"
CORE1_BIN="build/core1_bridge/core1_bridge.bin"
DICT_BLOB="build/mie-host/dict.bin"
DICT_BLOB_V4="firmware/mie/data/dict_mie_v4.bin"
DICT_ADDR="0x10400000"
FONT_BLOB="firmware/mie/data/mie_unifont_sm_16.bin"
FONT_ADDR="0x10A00000"
CORE1_ONLY=false
DICT_ONLY=false
FONT_ONLY=false
USE_V4=false

for arg in "$@"; do
    case "$arg" in
        --core1) CORE1_ONLY=true ;;
        --dict)  DICT_ONLY=true ;;
        --font)  FONT_ONLY=true ;;
        --v4)    USE_V4=true ;;
    esac
done

# When --v4 is set, swap the dict source to the MIED v4 single binary.
# Core 1's mie_dict_loader auto-detects by magic byte.
if [ "$USE_V4" = true ]; then
    if [ ! -f "$DICT_BLOB_V4" ]; then
        echo "ERROR: $DICT_BLOB_V4 not found."
        echo "  Build it first: python firmware/mie/tools/gen_dict.py \\"
        echo "                    --libchewing firmware/mie/data_sources/tsi.csv \\"
        echo "                    --zh-max-abbr-syls 4 \\"
        echo "                    --v4-output $DICT_BLOB_V4 \\"
        echo "                    --output-dir /tmp/mie_v2_throwaway"
        exit 1
    fi
    DICT_BLOB="$DICT_BLOB_V4"
    echo "=== --v4 mode: dict will be flashed from $DICT_BLOB ==="
fi

# ── Core 0: PlatformIO build ────────────────────────────────────────────
if [ "$CORE1_ONLY" = false ] && [ "$DICT_ONLY" = false ] && [ "$FONT_ONLY" = false ]; then
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
if [ "$DICT_ONLY" = false ] && [ "$FONT_ONLY" = false ]; then
    echo ""
    echo "=== Building Core 1 (m1_bridge via CMake) ==="
    cmake --build build/core1_bridge 2>&1
    if [ ! -f "$CORE1_BIN" ]; then
        echo "ERROR: $CORE1_BIN not found — CMake build failed"
        exit 1
    fi
    echo "OK: $CORE1_BIN"
    # Core 1 build also regenerates $FONT_BLOB via mie_font_data target.
    if [ ! -f "$FONT_BLOB" ]; then
        echo "ERROR: $FONT_BLOB not found — font blob build failed"
        exit 1
    fi
    echo "OK: $FONT_BLOB ($(stat -c%s "$FONT_BLOB" 2>/dev/null || wc -c < "$FONT_BLOB") bytes)"
fi

# ── Dict blob: CMake/MSBuild via mie-host (MDBL v2) or pre-built v4 ────
if [ "$FONT_ONLY" = false ]; then
    if [ "$USE_V4" = true ]; then
        # v4 dict is built directly by gen_dict.py (no CMake target yet).
        # Caller is responsible for keeping $DICT_BLOB_V4 fresh.
        echo ""
        echo "=== Using pre-built v4 dict at $DICT_BLOB ==="
    else
        echo ""
        echo "=== Building MIE dict blob (mie-host/mie_dict_blob) ==="
        cmake --build build/mie-host --config Debug --target mie_dict_blob 2>&1
    fi
    if [ ! -f "$DICT_BLOB" ]; then
        echo "ERROR: $DICT_BLOB not found"
        exit 1
    fi
    echo "OK: $DICT_BLOB ($(stat -c%s "$DICT_BLOB" 2>/dev/null || wc -c < "$DICT_BLOB") bytes)"
fi

# ── Flash via J-Link ────────────────────────────────────────────────────
echo ""
echo "=== Flashing via J-Link ==="

JLINK_SCRIPT="/tmp/jlink_flash_dual.jlink"
DICT_BLOB_WIN="$(cygpath -w "$(pwd)/$DICT_BLOB")"
FONT_BLOB_WIN="$(cygpath -w "$(pwd)/$FONT_BLOB")"

{
    echo "connect"
    echo "r"
    if [ "$CORE1_ONLY" = false ] && [ "$DICT_ONLY" = false ] && [ "$FONT_ONLY" = false ]; then
        CORE0_ELF_WIN="$(cygpath -w "$(pwd)/$CORE0_ELF")"
        printf 'loadfile "%s"\n' "$CORE0_ELF_WIN"
    fi
    if [ "$DICT_ONLY" = false ] && [ "$FONT_ONLY" = false ]; then
        CORE1_BIN_WIN="$(cygpath -w "$(pwd)/$CORE1_BIN")"
        printf 'loadbin "%s" 0x10200000\n' "$CORE1_BIN_WIN"
    fi
    if [ "$FONT_ONLY" = false ]; then
        printf 'loadbin "%s" %s\n' "$DICT_BLOB_WIN" "$DICT_ADDR"
    fi
    if [ "$DICT_ONLY" = false ]; then
        printf 'loadbin "%s" %s\n' "$FONT_BLOB_WIN" "$FONT_ADDR"
    fi
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
