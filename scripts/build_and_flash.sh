#!/usr/bin/env bash
# Build and flash dual-image firmware (Core 0 Meshtastic + Core 1 bridge)
# plus the MIE dictionary blob to RP2350B via J-Link SWD.
#
# Flash layout (W25Q128JW, 16 MB):
#   0x10000000  Core 0 Meshtastic image   (2 MB slot)
#   0x10200000  Core 1 bridge image       (2 MB slot)
#   0x10400000  MIE dictionary (MIE4 blob, 6 MB reserved, ~4 MB used)
#   0x10A00000  MIE font blob (MIEF, 2 MB reserved)
#   0x10C00000  MIE LRU persist           (64 KB reserved, Phase 1.6)
#   0x10C10000  Free / future LittleFS    (~3.94 MB)
#
# Run from project root:
#   bash scripts/build_and_flash.sh          # build + flash everything (MIE4 v4 dict, default)
#   bash scripts/build_and_flash.sh --core1  # build + flash ONLY Core 1 image —
#                                            # preserves whatever dict/font are
#                                            # already on the board (use this for
#                                            # fast Core 1 iteration)
#   bash scripts/build_and_flash.sh --dict   # flash dict blob only (v4)
#   bash scripts/build_and_flash.sh --font   # flash font blob only
#   bash scripts/build_and_flash.sh --v4     # NO-OP alias (v4 is now default;
#                                            # kept for backward compat with
#                                            # older muscle memory / docs)
#   bash scripts/build_and_flash.sh --v2-deprecated [--dict]
#                                            # Escape hatch: flash the legacy
#                                            # MDBL v2 dict. RETIRED 2026-04-26
#                                            # (P3-5) — opt-in only, prints a
#                                            # loud warning. Reach for this
#                                            # only when bisecting v4 vs v2
#                                            # behaviour.
#
# v2 (MDBL) retirement notes:
# - Default has been v4 (MIE4) since 2026-04-26. All performance baselines
#   (~430 ms/char with --user-sim) are measured against v4 + the personalised
#   LRU cache; v2 hits ~1700 ms/char and has different candidate rankings.
# - The v2 generator (`firmware/mie/tools/gen_dict.py` --output-dir) is kept
#   as archaeology; binaries at firmware/mie/data/dict_dat.bin and
#   dict_values.bin are likewise kept (see README in that dir) but should
#   not be flashed except via the explicit --v2-deprecated escape hatch.
# - Core 1 still auto-detects MDBL vs MIE4 by magic, so the v2 path stays
#   functional. ime_text_test.py refuses to run against an MDBL flash and
#   will direct the user back here.
#
# IMPORTANT: --core1 used to ALSO reflash dict + font, which silently overwrote
# a prior --v4 MIE4 dict with the default MDBL v2 build (bug fixed 2026-04-24).
# That bug was the original motivation for retiring v2 — even with --core1
# tightened, having the no-flag default produce v2 made it too easy to
# accidentally regress.
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
# v4 (MIE4) is now the default. --v4 is kept as a no-op alias; --v2-deprecated
# is the only way to opt back into the retired MDBL v2 path.
USE_V4=true
V2_DEPRECATED_OPT_IN=false

for arg in "$@"; do
    case "$arg" in
        --core1)          CORE1_ONLY=true ;;
        --dict)           DICT_ONLY=true ;;
        --font)           FONT_ONLY=true ;;
        --v4)             USE_V4=true ;;        # no-op, default
        --v2-deprecated)  V2_DEPRECATED_OPT_IN=true ;;
    esac
done

if [ "$V2_DEPRECATED_OPT_IN" = true ]; then
    USE_V4=false
    cat >&2 <<'EOF'
================================================================
WARNING: --v2-deprecated selected. Flashing legacy MDBL v2 dict.
  - v2 was retired 2026-04-26 (P3-5). All current perf baselines
    are measured against v4. v2 ranks differently and benchmarks
    on v2 will look ~4× slower than v4.
  - ime_text_test.py refuses to run against an MDBL flash; if you
    just want a working device, drop --v2-deprecated and rerun.
  - Continuing in 3 s. Ctrl-C to abort.
================================================================
EOF
    sleep 3
fi

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
    echo "=== Dict source: MIE4 v4 (default) — $DICT_BLOB ==="
else
    echo "=== Dict source: MDBL v2 (DEPRECATED) — will rebuild via mie-host ==="
fi

# ── Core 0: PlatformIO build ────────────────────────────────────────────
if [ "$CORE1_ONLY" = false ] && [ "$DICT_ONLY" = false ] && [ "$FONT_ONLY" = false ]; then
    echo "=== Building Core 0 (Meshtastic via PlatformIO) ==="
    python -m platformio run -e rp2350b-mokya \
        -d firmware/core0/meshtastic 2>&1
    # -t sorts by mtime, newest first. PlatformIO emits a new ELF per
    # commit-hash suffix without removing old ones, so plain `ls | head -1`
    # picks the alphabetically-earliest (often the oldest) and silently
    # flashes stale firmware.
    CORE0_ELF="$(ls -t firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware*.elf 2>/dev/null | head -1)"
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
    # Core 1 build also regenerates $FONT_BLOB via mie_font_data target,
    # but we only check for the font blob when we're actually going to flash
    # it (i.e. NOT --core1, which leaves the font partition alone).
    if [ "$CORE1_ONLY" = false ]; then
        if [ ! -f "$FONT_BLOB" ]; then
            echo "ERROR: $FONT_BLOB not found — font blob build failed"
            exit 1
        fi
        echo "OK: $FONT_BLOB ($(stat -c%s "$FONT_BLOB" 2>/dev/null || wc -c < "$FONT_BLOB") bytes)"
    fi
fi

# ── Dict blob: pre-built v4 (default) or CMake/MSBuild MDBL v2 (deprecated) ────
# --core1 skips this section entirely: the flash step below won't reflash
# the dict partition either, so preserving whatever is already there wins
# over wasted build time + the real footgun of downgrading a prior --v4
# MIE4 blob back to MDBL v2.
if [ "$FONT_ONLY" = false ] && [ "$CORE1_ONLY" = false ]; then
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
    # Dict + font are flashed only when the caller explicitly wants them.
    # --core1 is Core-1-only on purpose (see header comment for the bug
    # that forced this tightening).
    if [ "$FONT_ONLY" = false ] && [ "$CORE1_ONLY" = false ]; then
        printf 'loadbin "%s" %s\n' "$DICT_BLOB_WIN" "$DICT_ADDR"
    fi
    if [ "$DICT_ONLY" = false ] && [ "$CORE1_ONLY" = false ]; then
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
