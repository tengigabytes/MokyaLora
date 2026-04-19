#!/usr/bin/env bash
# run_mie_repl.sh — launch the MIE interactive REPL with all dictionaries
# loaded (Chinese + English). Run from anywhere in the repo.
#
# Usage:
#   bash scripts/run_mie_repl.sh
#
# Requires: build/mie-host/Debug/mie_repl.exe (build via /build-mie skill)
#           firmware/mie/data/{dict,en}_{dat,values}.bin (via /gen-data)
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

EXE="build/mie-host/Debug/mie_repl.exe"
ZH_DAT="firmware/mie/data/dict_dat.bin"
ZH_VAL="firmware/mie/data/dict_values.bin"
EN_DAT="firmware/mie/data/en_dat.bin"
EN_VAL="firmware/mie/data/en_values.bin"

missing=0
for f in "$EXE" "$ZH_DAT" "$ZH_VAL" "$EN_DAT" "$EN_VAL"; do
    if [[ ! -f "$f" ]]; then
        echo "missing: $f" >&2
        missing=1
    fi
done
if (( missing )); then
    echo
    echo "Hint: run '/build-mie' to build the REPL and '/gen-data' to" >&2
    echo "regenerate the dictionary assets, then rerun this script." >&2
    exit 1
fi

exec "./$EXE" --dat "$ZH_DAT" --val "$ZH_VAL" --en-dat "$EN_DAT" --en-val "$EN_VAL"
