#!/usr/bin/env python3
"""
gen_dict.py — MokyaInput Engine Dictionary Compiler
====================================================
Processes the Taiwan Ministry of Education (MoE) standard character/word list
and compiles a sorted binary dictionary for the MIE Trie-Searcher.

Output files (written to ../data/ by default):
  dict_dat.bin    — MIED header + sorted key index + keys data
  dict_values.bin — ValueRecord sequences (word lists per key)
  dict_meta.json  — Build metadata

Binary format — dict_dat.bin
─────────────────────────────
Header (16 bytes, little-endian):
  Offset  Size  Field
   0       4    magic = "MIED"
   4       2    version = 1
   6       2    flags = 0
   8       4    key_count      — number of unique Bopomofo keys
  12       4    keys_data_off  — byte offset to keys-data section

Index table (immediately after header, key_count × 8 bytes each):
  Offset  Size  Field
   0       4    key_data_off  — byte offset relative to keys-data section start
   4       4    val_data_off  — byte offset in dict_values.bin

Keys-data section (variable-length records, sorted lexicographically):
  Offset  Size  Field
   0       1    key_len    — byte length of UTF-8 Bopomofo key
   1      N     key_utf8   — key_len bytes (NOT null-terminated)

Binary format — dict_values.bin
────────────────────────────────
Sequence of ValueRecords at arbitrary offsets (pointed to by index table):
  Offset  Size  Field
   0       2    word_count
  For each word (sorted by frequency descending):
   0       2    freq
   2       1    word_len
   3      N     word_utf8   — word_len bytes (NOT null-terminated)

Usage:
  python gen_dict.py --moe-csv moe_dict.csv [--output-dir ../data]

CSV columns expected (UTF-8 with BOM):
  注音   — Bopomofo reading (may contain multiple syllables)
  詞語   — Word (Traditional Chinese UTF-8)
  頻率   — Frequency weight (integer; defaults to 1 if missing/invalid)

Dependencies: none (standard library only)
"""

import argparse
import csv
import json
import struct
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "data"

MAGIC   = b"MIED"
VERSION = 1


# ── Argument parsing ──────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(description="Compile MoE dictionary to MIE binary format")
    p.add_argument("--moe-csv",    required=True, help="Path to MoE word list CSV")
    p.add_argument("--output-dir", default=str(OUTPUT_DIR), help="Output directory")
    return p.parse_args()


# ── Bopomofo normalisation ────────────────────────────────────────────────

def normalise_bopomofo(reading: str) -> str:
    """
    Strip whitespace and normalise tone marks to standard positions.
    Full normalisation (variant syllable correction) is deferred to Phase 3.
    """
    return reading.strip()


# ── CSV loading ───────────────────────────────────────────────────────────

def load_moe_csv(csv_path: str) -> list:
    """
    Load MoE CSV and return list of (bopomofo_key: str, word: str, freq: int).
    Skips rows with empty reading or word.
    """
    entries = []
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            reading = normalise_bopomofo(row.get("注音", ""))
            word    = row.get("詞語", "").strip()
            try:
                freq = int(row.get("頻率", 1))
            except (ValueError, TypeError):
                freq = 1
            if reading and word:
                entries.append((reading, word, freq))
    return entries


# ── Binary serialisation ──────────────────────────────────────────────────

def build_value_record(word_list: list) -> bytes:
    """
    Serialise a list of (word, freq) pairs into a ValueRecord binary blob.
    Words are sorted by frequency descending.
    """
    sorted_words = sorted(word_list, key=lambda x: -x[1])
    data = struct.pack("<H", len(sorted_words))
    for word, freq in sorted_words:
        wb = word.encode("utf-8")
        data += struct.pack("<HB", min(freq, 0xFFFF), len(wb)) + wb
    return data


def build_outputs(entries: list) -> tuple:
    """
    Build dict_dat.bin and dict_values.bin byte strings.

    Returns (dat_bytes: bytes, val_bytes: bytes, stats: dict).
    """
    # Aggregate: bopomofo_key → [(word, freq)]
    key_to_words = defaultdict(list)
    for reading, word, freq in entries:
        key_to_words[reading].append((word, freq))

    sorted_keys = sorted(key_to_words.keys())
    key_count   = len(sorted_keys)

    # --- Build dict_values.bin and collect val_data_off per key ---
    val_data   = bytearray()
    val_offsets = {}          # key → byte offset in val_data
    for key in sorted_keys:
        val_offsets[key] = len(val_data)
        val_data += build_value_record(key_to_words[key])

    # --- Build keys-data section ---
    keys_section      = bytearray()
    key_data_off_map  = {}    # key → byte offset within keys_section
    for key in sorted_keys:
        key_data_off_map[key] = len(keys_section)
        kb = key.encode("utf-8")
        keys_section += struct.pack("B", len(kb)) + kb

    # --- Layout ---
    header_size    = 16
    index_size     = key_count * 8
    keys_data_off  = header_size + index_size    # absolute offset in dat file

    # --- Header ---
    header = MAGIC
    header += struct.pack("<HH", VERSION, 0)            # version, flags
    header += struct.pack("<II", key_count, keys_data_off)

    # --- Index table ---
    index = bytearray()
    for key in sorted_keys:
        index += struct.pack("<II", key_data_off_map[key], val_offsets[key])

    dat_data = header + bytes(index) + bytes(keys_section)

    stats = {
        "key_count":    key_count,
        "entry_count":  len(entries),
        "dat_bytes":    len(dat_data),
        "val_bytes":    len(val_data),
    }
    return bytes(dat_data), bytes(val_data), stats


# ── Main ──────────────────────────────────────────────────────────────────

def main():
    args       = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Loading MoE dictionary from {args.moe_csv} ...")
    entries = load_moe_csv(args.moe_csv)
    if not entries:
        print("ERROR: no entries loaded — check CSV format and column names.", file=sys.stderr)
        sys.exit(1)
    print(f"Loaded {len(entries):,} entries.")

    print("Building binary outputs ...")
    dat_bytes, val_bytes, stats = build_outputs(entries)

    (output_dir / "dict_dat.bin").write_bytes(dat_bytes)
    (output_dir / "dict_values.bin").write_bytes(val_bytes)
    print(f"  dict_dat.bin:    {stats['dat_bytes']:,} bytes  ({stats['key_count']:,} unique keys)")
    print(f"  dict_values.bin: {stats['val_bytes']:,} bytes")

    meta = {
        "format_version": VERSION,
        "source":         args.moe_csv,
        "entry_count":    stats["entry_count"],
        "unique_keys":    stats["key_count"],
        "dat_bytes":      stats["dat_bytes"],
        "val_bytes":      stats["val_bytes"],
        "built_at":       datetime.now(timezone.utc).isoformat(),
    }
    (output_dir / "dict_meta.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print("Done.")


if __name__ == "__main__":
    main()
