#!/usr/bin/env python3
"""
gen_dict.py — MokyaInput Engine Dictionary Compiler
====================================================
Processes the Taiwan Ministry of Education (MoE) standard character/word list,
corrects Taiwan-standard Bopomofo readings, and compiles a Double-Array Trie (DAT)
for efficient binary search at runtime within the 4 MB PSRAM budget.

Output files (written to ../data/):
  dict_dat.bin      — Double-Array Trie arrays (base[] and check[])
  dict_values.bin   — Value records: (word_utf8, frequency: uint16)
  dict_meta.json    — Build metadata (source version, entry count, build date)

Usage:
  python gen_dict.py --moe-csv moe_dict.csv --output-dir ../data

Dependencies:
  pip install datrie  (Double-Array Trie implementation)
"""

import argparse
import csv
import json
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "data"


def parse_args():
    p = argparse.ArgumentParser(description="Compile MoE dictionary to MIE binary DAT format")
    p.add_argument("--moe-csv", required=True, help="Path to MoE word list CSV")
    p.add_argument("--output-dir", default=str(OUTPUT_DIR), help="Output directory")
    return p.parse_args()


def normalise_bopomofo(reading: str) -> str:
    """
    Placeholder: normalise MoE readings to Taiwan-standard Bopomofo.
    E.g. correct variant tone marks, remove non-standard syllables.
    """
    # TODO: implement full normalisation rules
    return reading.strip()


def load_moe_csv(csv_path: str) -> list:
    """Load MoE CSV and return list of (bopomofo_key, word, frequency)."""
    entries = []
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            reading = normalise_bopomofo(row.get("注音", ""))
            word = row.get("詞語", "").strip()
            try:
                freq = int(row.get("頻率", 1))
            except ValueError:
                freq = 1
            if reading and word:
                entries.append((reading, word, freq))
    return entries


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Loading MoE dictionary from {args.moe_csv} ...")
    entries = load_moe_csv(args.moe_csv)
    print(f"Loaded {len(entries):,} entries.")

    # Sort by Bopomofo key for trie insertion
    entries.sort(key=lambda e: e[0])

    # --- Build DAT ---
    # Requires: pip install datrie
    try:
        import datrie
    except ImportError:
        print("ERROR: 'datrie' package not installed. Run: pip install datrie", file=sys.stderr)
        sys.exit(1)

    alphabet = set()
    for reading, _, _ in entries:
        alphabet.update(reading)
    trie = datrie.Trie(sorted(alphabet))

    values = []
    for reading, word, freq in entries:
        key = reading
        if trie.has_key(key):
            values[trie[key]].append((word, freq))
        else:
            trie[key] = len(values)
            values.append([(word, freq)])

    # Serialise values
    value_data = bytearray()
    value_offsets = []
    for word_list in values:
        value_offsets.append(len(value_data))
        # Record count (uint16) followed by (freq: uint16, word_len: uint8, word_utf8)
        value_data += struct.pack("<H", len(word_list))
        for word, freq in sorted(word_list, key=lambda x: -x[1]):
            wb = word.encode("utf-8")
            value_data += struct.pack("<HB", freq, len(wb)) + wb

    (output_dir / "dict_values.bin").write_bytes(value_data)
    print(f"Values: {len(value_data):,} bytes")

    # Serialise DAT arrays
    base, check = trie._da.base, trie._da.check  # datrie internals
    dat_data = struct.pack("<II", len(base), len(check))
    dat_data += struct.pack(f"<{len(base)}i", *base)
    dat_data += struct.pack(f"<{len(check)}i", *check)
    (output_dir / "dict_dat.bin").write_bytes(dat_data)
    print(f"DAT arrays: {len(dat_data):,} bytes")

    # Metadata
    meta = {
        "source": args.moe_csv,
        "entry_count": len(entries),
        "unique_keys": len(values),
        "built_at": datetime.now(timezone.utc).isoformat(),
    }
    (output_dir / "dict_meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2))
    print(f"Done. Metadata written to dict_meta.json")


if __name__ == "__main__":
    main()
