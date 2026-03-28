#!/usr/bin/env python3
"""
gen_dict.py — MokyaInput Engine Dictionary Compiler
====================================================
Nokia-style prefix prediction for the MokyaLora 5×5 half-keyboard.

Design
------
Each physical key carries 2–3 Bopomofo phonemes (or 1–2 English letters).
The user presses a sequence of keys; the engine returns candidate words
in real time without requiring the user to mentally resolve ambiguity.

Key encoding
------------
  key_index = row * 5 + col   (0–19, matching the 4×5 input grid)
  key_byte  = key_index + 0x21  → ASCII range '!'–'4' (no null bytes)

The resulting key-sequence byte string is used directly as the MIED lookup
key in TrieSearcher (binary prefix search on a sorted index).

KEYMAP (matches hardware-requirements.md §8.1 / kGuiKeys in mie_gui.cpp)
-------------------------------------------------------------------------
  idx  key   Bopomofo phonemes    English letters
   0  (0,0)  ㄅ ㄉ
   1  (0,1)  ˇ ˋ               (tone keys 3 & 4 — no letter)
   2  (0,2)  ㄓ ˊ              (+ tone 2)
   3  (0,3)  ˙ ㄚ              (tone 5 + vowel)
   4  (0,4)  ㄞ ㄢ ㄦ
   5  (1,0)  ㄆ ㄊ              Q W
   6  (1,1)  ㄍ ㄐ              E R
   7  (1,2)  ㄔ ㄗ              T Y
   8  (1,3)  ㄧ ㄛ              U I
   9  (1,4)  ㄟ ㄣ              O P
  10  (2,0)  ㄇ ㄋ              A S
  11  (2,1)  ㄎ ㄑ              D F
  12  (2,2)  ㄕ ㄘ              G H
  13  (2,3)  ㄨ ㄜ              J K
  14  (2,4)  ㄠ ㄤ              L
  15  (3,0)  ㄈ ㄌ              Z X
  16  (3,1)  ㄏ ㄒ              C V
  17  (3,2)  ㄖ ㄙ              B N
  18  (3,3)  ㄩ ㄝ              M
  19  (3,4)  ㄡ ㄥ              (— em-dash, no letter)

Tone 1 (陰平, no mark) has no physical key → skipped in key sequence.

Outputs
-------
  dict_dat.bin    Chinese key-sequence MIED index
  dict_values.bin Chinese word pool
  en_dat.bin      English key-sequence MIED index  (if --en-wordlist given)
  en_values.bin   English word pool
  dict_meta.json  Build metadata + license notices

MIED binary format (same as before, key changed from phoneme string to key-seq bytes)
---------------------
  dict_dat.bin:
    Header (16 bytes LE): magic="MIED" version=1 flags=0 key_count keys_data_off
    Index  (key_count × 8 bytes): (key_data_off: uint32, val_data_off: uint32)
    Keys   (variable): (key_len: uint8, key_bytes: bytes)  — sorted ascending
  dict_values.bin:
    ValueRecord: (word_count: uint16, (freq: uint16, word_len: uint8, word_utf8)...)

License notices
---------------
  libchewing-data © LibChewing contributors, LGPL-2.1
    https://github.com/chewing/libchewing-data
  MoE dictionary  © Republic of China Ministry of Education
    https://dict.revised.moe.edu.tw/ (public domain for educational use)
  English wordlist: credit varies by source — see --en-license argument.

Requires: Python ≥ 3.8, standard library only.

Usage
-----
  python gen_dict.py [--libchewing tsi.src]
                     [--moe-csv moe_dict.csv]
                     [--en-wordlist en_wordlist.txt]
                     [--output-dir firmware/mie/data]
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
MAGIC      = b"MIED"
VERSION    = 1

# ── 5×5 KEYMAP ────────────────────────────────────────────────────────────
# key_index (0–19) → list of Bopomofo phonemes on that physical key.
# Constructed as (row, col) pairs where key_index = row*5+col.

_BPMF_KEYMAP_RAW = [
    # idx  (row,col)  phonemes
    (  0,  'ㄅ', 'ㄉ'),
    (  1,  'ˇ',  'ˋ'),           # tone 3, tone 4
    (  2,  'ㄓ', 'ˊ'),           # consonant + tone 2
    (  3,  '˙',  'ㄚ'),          # tone 5 + vowel
    (  4,  'ㄞ', 'ㄢ', 'ㄦ'),
    (  5,  'ㄆ', 'ㄊ'),
    (  6,  'ㄍ', 'ㄐ'),
    (  7,  'ㄔ', 'ㄗ'),
    (  8,  'ㄧ', 'ㄛ'),
    (  9,  'ㄟ', 'ㄣ'),
    ( 10,  'ㄇ', 'ㄋ'),
    ( 11,  'ㄎ', 'ㄑ'),
    ( 12,  'ㄕ', 'ㄘ'),
    ( 13,  'ㄨ', 'ㄜ'),
    ( 14,  'ㄠ', 'ㄤ'),
    ( 15,  'ㄈ', 'ㄌ'),
    ( 16,  'ㄏ', 'ㄒ'),
    ( 17,  'ㄖ', 'ㄙ'),
    ( 18,  'ㄩ', 'ㄝ'),
    ( 19,  'ㄡ', 'ㄥ'),
]

# Reverse map: phoneme → key_index
PHONEME_TO_KEY: dict = {}
for _entry in _BPMF_KEYMAP_RAW:
    _idx = _entry[0]
    for _ph in _entry[1:]:
        PHONEME_TO_KEY[_ph] = _idx

# English letter → key_index (rows 1–3 of the input grid)
_ENG_KEYMAP_RAW = [
    (5,  'q', 'w'),
    (6,  'e', 'r'),
    (7,  't', 'y'),
    (8,  'u', 'i'),
    (9,  'o', 'p'),
    (10, 'a', 's'),
    (11, 'd', 'f'),
    (12, 'g', 'h'),
    (13, 'j', 'k'),
    (14, 'l'),
    (15, 'z', 'x'),
    (16, 'c', 'v'),
    (17, 'b', 'n'),
    (18, 'm'),
]
ENG_LETTER_TO_KEY: dict = {}
for _entry in _ENG_KEYMAP_RAW:
    _idx = _entry[0]
    for _ch in _entry[1:]:
        ENG_LETTER_TO_KEY[_ch] = _idx

KEY_OFFSET = 0x21   # key_byte = key_index + KEY_OFFSET  → printable ASCII, no NULLs

# ── Key-sequence encoding ─────────────────────────────────────────────────

def phonemes_to_keyseq(phoneme_list: list) -> bytes:
    """
    Convert a list of individual Bopomofo phonemes to a key-sequence byte string.

    Tone 1 (陰平) has no physical key and is skipped.
    Returns b'' if no phoneme maps to a key (e.g., pure punctuation).
    """
    seq = bytearray()
    for ph in phoneme_list:
        if ph in PHONEME_TO_KEY:
            seq.append(PHONEME_TO_KEY[ph] + KEY_OFFSET)
    return bytes(seq)


def word_to_eng_keyseq(word: str) -> bytes:
    """
    Convert an English word to a key-sequence byte string (lowercase only).
    Returns b'' if any letter is not mappable (punctuation, digits, etc.).
    """
    seq = bytearray()
    for ch in word.lower():
        if ch not in ENG_LETTER_TO_KEY:
            return b''      # unmappable character — skip entire word
        seq.append(ENG_LETTER_TO_KEY[ch] + KEY_OFFSET)
    return bytes(seq)

# ── Phoneme parsing ───────────────────────────────────────────────────────

# Digit → tone mark conversion (some sources use digit suffixes)
_DIGIT_TONE = {'2': 'ˊ', '3': 'ˇ', '4': 'ˋ', '5': '˙'}

def parse_syllable(syllable: str) -> list:
    """
    Split a single Bopomofo syllable string into individual phonemes.

    Handles:
    - Standard Bopomofo Unicode characters (U+3100–U+312F, U+31A0–U+31BF)
    - Spacing modifier tone marks (U+02CA ˊ, U+02C7 ˇ, U+02CB ˋ, U+02D9 ˙)
    - Digit tone suffixes 2–5 (converted to tone marks); '1' = tone 1 → skipped
    """
    phonemes = []
    for ch in syllable:
        cp = ord(ch)
        if 0x3100 <= cp <= 0x312F or 0x31A0 <= cp <= 0x31BF:
            phonemes.append(ch)
        elif ch in ('ˊ', 'ˇ', 'ˋ', '˙'):
            phonemes.append(ch)
        elif ch in _DIGIT_TONE:
            phonemes.append(_DIGIT_TONE[ch])
        # '1' = tone 1, no mark, no key → silently skip
    return phonemes


def parse_reading(reading: str) -> list:
    """
    Convert a full Bopomofo reading string (one or more syllables) to a flat
    list of individual phonemes.

    Syllables are typically space-separated; the function also handles
    unseparated syllable streams.
    """
    phonemes = []
    syllables = reading.split() if ' ' in reading else [reading]
    for syl in syllables:
        phonemes.extend(parse_syllable(syl.strip()))
    return phonemes


def parse_reading_syllables(reading: str) -> list:
    """
    Like parse_reading() but preserves syllable boundaries.

    Returns a list of per-syllable phoneme lists, e.g.:
      [['ㄋ', 'ㄧ', 'ˇ'], ['ㄏ', 'ㄠ', 'ˇ']]

    Syllables whose parse_syllable result is empty (e.g., tone-1-only) are
    dropped.  Used by abbreviated_keyseqs() to generate initial-key variants.
    """
    sylls = reading.split() if ' ' in reading else [reading]
    result = []
    for syl in sylls:
        phs = parse_syllable(syl.strip())
        if phs:
            result.append(phs)
    return result


def abbreviated_keyseqs(reading: str, full_keyseq: bytes) -> list:
    """
    Generate abbreviated key sequences for a word, enabling fast "initial-key"
    input (聲母猜字) in addition to the standard full-phoneme key sequence.

    For a word with N syllables:
      N == 1  → emit the single-syllable initial key (1 key byte).
      N >= 2  → emit TWO variants:
                 1. all-initials: one initial key per syllable  (N bytes)
                 2. prefix-initials + full-last: initials of syllables
                    0..N-2, then full phoneme of the last syllable

    Rationale for variant 2 (user example: `rwu0` = 今天):
      r = initial(今=ㄐ), w+u+0 = full(天=ㄊㄧㄢ)
      → typing the initial of all-but-the-last char plus the full phoneme
        of the last char is a natural fast-entry style.

    Returns a list of unique bytes objects, each different from full_keyseq
    and from each other.  Returns [] when syllable data is empty.
    """
    syls = parse_reading_syllables(reading)
    n = len(syls)
    if n == 0:
        return []

    seen = {full_keyseq}
    variants: list = []

    if n == 1:
        # Single-syllable word: initial key = first phoneme of the syllable.
        init = phonemes_to_keyseq(syls[0][:1])
        if init and init not in seen:
            seen.add(init)
            variants.append(init)
        return variants

    # Multi-syllable word (n >= 2) — two variants.
    # 1. All-initials: initial key of each syllable concatenated.
    all_init = b''.join(phonemes_to_keyseq(s[:1]) for s in syls)
    if all_init and all_init not in seen:
        seen.add(all_init)
        variants.append(all_init)

    # 2. Prefix-initials + full last syllable:
    #    initial keys of syllables 0..n-2, then full phoneme of syllable n-1.
    prefix = b''.join(phonemes_to_keyseq(s[:1]) for s in syls[:-1])
    last   = phonemes_to_keyseq(syls[-1])
    mixed  = prefix + last
    if mixed and mixed not in seen:
        seen.add(mixed)
        variants.append(mixed)

    return variants

# ── libchewing tsi.csv loader ─────────────────────────────────────────────

def load_libchewing(path: str) -> list:
    """
    Parse libchewing tsi.csv (comma-separated: word, freq, reading).

    tsi.csv column order: word, freq (0 or 1), reading
    freq is a binary flag: 1 = common word, 0 = rare character.
    Common words (freq=1) sort above rare characters (freq=0) in the candidate list.

    Returns list of (keyseq: bytes, word: str, freq: int).
    Lines starting with '#' are treated as comments.
    """
    entries = []
    skipped = 0
    with open(path, newline='', encoding='utf-8', errors='strict') as f:
        reader = csv.reader(f)
        for lineno, row in enumerate(reader, 1):
            if not row or row[0].startswith('#'):
                continue
            if len(row) < 3:
                continue
            word    = row[0].strip()
            # tsi.csv columns: word, freq (binary 0/1), reading
            reading = row[2].strip()
            try:
                freq = int(row[1].strip())
            except (ValueError, IndexError):
                freq = 0

            phonemes = parse_reading(reading)
            keyseq   = phonemes_to_keyseq(phonemes)
            if keyseq and word:
                entries.append((keyseq, word, freq))
                for abbr in abbreviated_keyseqs(reading, keyseq):
                    entries.append((abbr, word, freq))
            else:
                skipped += 1

    if skipped:
        print(f"  libchewing: {skipped:,} entries skipped (unmappable phonemes)",
              file=sys.stderr)
    return entries

# ── MoE CSV loader ────────────────────────────────────────────────────────

def load_moe_csv(path: str) -> list:
    """
    Parse MoE dictionary CSV (UTF-8 with BOM).
    Expected columns: 注音 / 詞語 / 頻率

    Returns list of (keyseq: bytes, word: str, freq: int).
    """
    entries = []
    skipped = 0
    with open(path, newline='', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        for row in reader:
            reading = row.get('注音', '').strip()
            word    = row.get('詞語', '').strip()
            try:
                freq = max(1, int(row.get('頻率', 1)))
            except (ValueError, TypeError):
                freq = 1

            if not reading or not word:
                continue

            phonemes = parse_reading(reading)
            keyseq   = phonemes_to_keyseq(phonemes)
            if keyseq:
                entries.append((keyseq, word, freq))
                for abbr in abbreviated_keyseqs(reading, keyseq):
                    entries.append((abbr, word, freq))
            else:
                skipped += 1

    if skipped:
        print(f"  MoE CSV: {skipped:,} entries skipped (unmappable phonemes)",
              file=sys.stderr)
    return entries

# ── English wordlist loader ───────────────────────────────────────────────

def load_en_wordlist(path: str) -> list:
    """
    Parse an English word list — one word (+ optional TAB frequency) per line.

    Returns list of (keyseq: bytes, word: str, freq: int).
    Words with unmappable characters (digits, punctuation except apostrophe-less)
    are silently dropped.
    """
    entries = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t', 1)
            word  = parts[0].strip().lower()
            freq  = 1
            if len(parts) == 2:
                try:
                    freq = max(1, int(parts[1].strip()))
                except ValueError:
                    pass

            keyseq = word_to_eng_keyseq(word)
            if keyseq:
                entries.append((keyseq, word, freq))

    return entries

# ── MIED binary builder ───────────────────────────────────────────────────

_OVERSIZED_WORDS = 0  # module-level counter reset in build_mied

def build_value_record(word_list: list) -> bytes:
    """Serialise [(word, freq)] into a ValueRecord blob (freq-descending).

    Words whose UTF-8 encoding exceeds 30 bytes are silently skipped
    (kCandidateMaxBytes=32 in TrieSearcher; runtime check is >=32, so max safe=31;
    we cap at 30 to leave room for the null terminator the runtime copies).
    """
    global _OVERSIZED_WORDS
    sorted_words = sorted(word_list, key=lambda x: -x[1])
    valid = []
    for word, freq in sorted_words:
        wb = word.encode('utf-8')
        if len(wb) >= 31:
            _OVERSIZED_WORDS += 1
            continue
        valid.append((wb, freq))
    data = struct.pack('<H', len(valid))
    for wb, freq in valid:
        data += struct.pack('<HB', min(freq, 0xFFFF), len(wb)) + wb
    return data


def build_mied(entries: list) -> tuple:
    """
    Build MIED binary from a list of (keyseq: bytes, word: str, freq: int).

    Returns (dat_bytes, val_bytes, stats_dict, key_to_words).
    key_to_words is the deduplicated dict (keyseq → [(word, freq)]) needed by
    callers that want to emit a charlist.
    """
    global _OVERSIZED_WORDS
    _OVERSIZED_WORDS = 0

    # Aggregate: keyseq → [(word, freq)]
    key_to_words = defaultdict(list)
    for keyseq, word, freq in entries:
        key_to_words[keyseq].append((word, freq))

    sorted_keys = sorted(key_to_words.keys())
    key_count   = len(sorted_keys)

    # --- dict_values.bin ---
    val_data    = bytearray()
    val_offsets = {}
    for ks in sorted_keys:
        val_offsets[ks] = len(val_data)
        val_data += build_value_record(key_to_words[ks])

    # --- Keys section ---
    keys_section     = bytearray()
    key_data_off_map = {}
    for ks in sorted_keys:
        key_data_off_map[ks] = len(keys_section)
        keys_section += struct.pack('B', len(ks)) + ks

    # --- dict_dat.bin layout ---
    header_size   = 16
    index_size    = key_count * 8
    keys_data_off = header_size + index_size

    header = MAGIC
    header += struct.pack('<HH', VERSION, 0)               # version, flags
    header += struct.pack('<II', key_count, keys_data_off)

    index = bytearray()
    for ks in sorted_keys:
        index += struct.pack('<II', key_data_off_map[ks], val_offsets[ks])

    dat_bytes = header + bytes(index) + bytes(keys_section)

    if _OVERSIZED_WORDS:
        print(f"  WARNING: {_OVERSIZED_WORDS:,} words skipped (UTF-8 length >= 31 bytes)",
              file=sys.stderr)

    stats = {
        'key_count':   key_count,
        'entry_count': len(entries),
        'dat_bytes':   len(dat_bytes),
        'val_bytes':   len(val_data),
    }
    return bytes(dat_bytes), bytes(val_data), stats, key_to_words

# ── Argument parsing ──────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='Compile Nokia-style key-sequence dictionary for MokyaLora MIE.')
    p.add_argument('--libchewing',     metavar='TSI',
                   help='libchewing-data tsi.csv path (comma-separated: word,freq,reading)')
    p.add_argument('--moe-csv',        metavar='CSV',
                   help='MoE word list CSV path (注音/詞語/頻率 columns, UTF-8 BOM)')
    p.add_argument('--en-wordlist',    metavar='TXT',
                   help='English word list (one word per line, optional TAB freq)')
    p.add_argument('--en-max-words',   metavar='N', type=int, default=0,
                   help='Limit English word list to first N words before deduplication '
                        '(0 = no limit, default)')
    p.add_argument('--emit-charlist',  metavar='PATH',
                   help='Write unique output characters (one per line) to PATH '
                        '(small-font charlist; derived from Chinese dict only)')
    p.add_argument('--output-dir',     default=str(OUTPUT_DIR),
                   help=f'Output directory  [default: {OUTPUT_DIR}]')
    return p.parse_args()

# ── Main ──────────────────────────────────────────────────────────────────

def main():
    args       = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not args.libchewing and not args.moe_csv and not args.en_wordlist:
        print('ERROR: supply at least one of --libchewing / --moe-csv / --en-wordlist',
              file=sys.stderr)
        sys.exit(1)

    # ── Chinese dictionary ─────────────────────────────────────────────────
    zh_entries: list = []

    if args.libchewing:
        print(f'Loading libchewing  {args.libchewing} ...')
        loaded = load_libchewing(args.libchewing)
        zh_entries.extend(loaded)
        print(f'  {len(loaded):,} entries')

    if args.moe_csv:
        print(f'Loading MoE CSV     {args.moe_csv} ...')
        loaded = load_moe_csv(args.moe_csv)
        zh_entries.extend(loaded)
        print(f'  {len(loaded):,} entries')

    zh_stats = None
    zh_key_to_words = None
    if zh_entries:
        print(f'Building Chinese MIED  ({len(zh_entries):,} total entries) ...')
        dat, val, zh_stats, zh_key_to_words = build_mied(zh_entries)
        (output_dir / 'dict_dat.bin').write_bytes(dat)
        (output_dir / 'dict_values.bin').write_bytes(val)
        print(f'  dict_dat.bin    : {zh_stats["dat_bytes"]:>9,} bytes  '
              f'({zh_stats["key_count"]:,} unique key sequences)')
        print(f'  dict_values.bin : {zh_stats["val_bytes"]:>9,} bytes')

    # ── Charlist emit (small font variant) ────────────────────────────────
    if args.emit_charlist and zh_key_to_words:
        seen: set = set()
        chars: list = []
        for ks in sorted(zh_key_to_words.keys()):
            for word, _ in zh_key_to_words[ks]:
                for ch in word:          # iterate individual Unicode codepoints
                    if ch not in seen:
                        seen.add(ch)
                        chars.append(ch)
        charlist_path = Path(args.emit_charlist)
        charlist_path.parent.mkdir(parents=True, exist_ok=True)
        charlist_path.write_text('\n'.join(chars) + '\n', encoding='utf-8')
        print(f'  charlist        : {len(chars):,} unique chars → {charlist_path}')

    # ── English dictionary ────────────────────────────────────────────────
    en_stats = None
    if args.en_wordlist:
        print(f'Loading English     {args.en_wordlist} ...')
        en_entries = load_en_wordlist(args.en_wordlist)
        if args.en_max_words and args.en_max_words > 0:
            en_entries = en_entries[:args.en_max_words]
            print(f'  truncated to {args.en_max_words:,} words (--en-max-words)')
        print(f'  {len(en_entries):,} entries')
        print(f'Building English MIED ...')
        dat, val, en_stats, _ = build_mied(en_entries)
        (output_dir / 'en_dat.bin').write_bytes(dat)
        (output_dir / 'en_values.bin').write_bytes(val)
        print(f'  en_dat.bin      : {en_stats["dat_bytes"]:>9,} bytes  '
              f'({en_stats["key_count"]:,} unique key sequences)')
        print(f'  en_values.bin   : {en_stats["val_bytes"]:>9,} bytes')

    # ── Metadata ──────────────────────────────────────────────────────────
    meta = {
        'format_version': VERSION,
        'key_offset':     KEY_OFFSET,
        'key_encoding':   'key_byte = key_index + 0x21 (printable ASCII, no NULLs)',
        'variant':        'sm' if args.emit_charlist else 'lg',
        'sources': {
            'libchewing':  args.libchewing,
            'moe_csv':     args.moe_csv,
            'en_wordlist': args.en_wordlist,
            'en_max_words': args.en_max_words if args.en_max_words else None,
        },
        'charlist_path':  args.emit_charlist,
        'chinese': zh_stats,
        'english': en_stats,
        'built_at': datetime.now(timezone.utc).isoformat(),
        'license_notices': [
            'libchewing-data: © LibChewing contributors, LGPL-2.1 '
            '(https://github.com/chewing/libchewing-data)',
            'MoE dictionary: © Republic of China Ministry of Education, '
            'public domain for educational use '
            '(https://dict.revised.moe.edu.tw/)',
            'English wordlist: credit per source file; see --en-wordlist provenance.',
        ],
    }
    (output_dir / 'dict_meta.json').write_text(
        json.dumps(meta, ensure_ascii=False, indent=2), encoding='utf-8')

    print()
    print('Done. Files written to', output_dir)
    print()
    print('License notices:')
    for notice in meta['license_notices']:
        print(' ', notice)


if __name__ == '__main__':
    main()
