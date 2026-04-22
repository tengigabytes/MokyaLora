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
  dict_values.bin  (v2 format):
    ValueRecord: (word_count: uint16, (freq: uint16, tone: uint8, word_len: uint8, word_utf8)...)
    tone: 1=陰平 2=陽平 3=上聲 4=去聲 5=輕聲  0=unknown (English / unspecified)

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
import itertools
import json
import struct
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "data"
MAGIC      = b"MIED"
VERSION    = 2   # v2 adds tone:u8 per word in ValueRecord

# MIED v4 — Composition Architecture (single combined dict file).
# Structure:
#   char_table:      unique CJK chars + their (key, tone, freq) readings
#   word_table:      multi-char words as char_id sequences + reading_idx[N]
#   first_char_idx:  per char_id -> sorted list of word_ids (freq desc)
#   key_to_char_idx: per phoneme key byte -> char_ids whose first reading
#                    starts with that byte
# Search composes user keys against char readings dynamically; abbreviation
# variants are NOT pre-computed.
MAGIC_V4   = b"MIE4"
VERSION_V4 = 4

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


def reading_to_tone(reading: str) -> int:
    """
    Extract the tone of the last syllable from a Bopomofo reading string.

    Returns an integer 1–5:
      1 = 陰平 (tone 1, no mark)
      2 = 陽平 (ˊ)
      3 = 上聲 (ˇ)
      4 = 去聲 (ˋ)
      5 = 輕聲 (˙)

    Handles both tone-mark characters and digit suffixes (2–5).
    Returns 1 when no tone mark is found (implicit first tone).
    """
    sylls = reading.split() if ' ' in reading else [reading]
    last = sylls[-1].strip() if sylls else ''
    # Digit-tone suffix check (rightmost digit 2–5).
    for ch in reversed(last):
        if ch in '2345':
            return int(ch)
        if ch in '1':
            return 1
    # Tone-mark character check.
    for ch in reversed(last):
        if ch == 'ˊ': return 2
        if ch == 'ˇ': return 3
        if ch == 'ˋ': return 4
        if ch == '˙': return 5
    return 1  # no marker → tone 1 (陰平)


def abbreviated_keyseqs(reading: str, full_keyseq: bytes) -> list:
    """
    Generate ALL abbreviated key sequences for a word via cartesian product
    of per-syllable key-prefix choices.  This enables flexible abbreviated
    input (聲母猜字) where each syllable may be typed as any prefix of its
    full phoneme key sequence (initial-only, initial+medial, or full).

    For a syllable with keys [k1, k2, k3] the prefix choices are:
        [k1],  [k1,k2],  [k1,k2,k3]

    The cartesian product across all N syllables yields up to
    max_choices_per_syllable ^ N variants.  For 臭豆腐 (3 syllables, 3 keys
    each) that is 3^3 − 1 = 26 variants (the full key is always excluded).

    Returns a list of unique bytes objects, each different from full_keyseq
    and from each other.  Returns [] when syllable data is empty or any
    syllable has no mappable phonemes.
    """
    syls = parse_reading_syllables(reading)
    if not syls:
        return []

    seen = {full_keyseq}
    variants: list = []

    # For each syllable build a list of non-empty, distinct key-prefix bytes
    # (length 1 to len(syl)).  Duplicates from ambiguous phoneme→key mapping
    # are collapsed so each choice is unique.
    syl_choices = []
    for syl in syls:
        choices: list = []
        for prefix_len in range(1, len(syl) + 1):
            key = phonemes_to_keyseq(syl[:prefix_len])
            if key and (not choices or key != choices[-1]):
                choices.append(key)
        if not choices:
            return []   # unmappable syllable → emit no variants at all
        syl_choices.append(choices)

    # Cartesian product: one prefix choice per syllable → concatenate → unique.
    for combo in itertools.product(*syl_choices):
        variant = b''.join(combo)
        if variant not in seen:
            seen.add(variant)
            variants.append(variant)

    return variants

# ── libchewing tsi.csv loader ─────────────────────────────────────────────

def load_libchewing(path: str,
                    min_freq: int = 0,
                    max_abbr_syls: int = 4) -> list:
    """
    Parse libchewing tsi.csv (comma-separated: word, freq, reading).

    tsi.csv column order: word, freq, reading
    freq is a numeric usage count (not a binary flag); typical distribution
    covers 0, 1, 2..10, ..., up to several thousand for very common words.
    Higher-freq words sort above lower-freq entries in the candidate list.

    min_freq:      drop both base entries AND abbreviation variants whose
                   freq < this value (0 = no filter, keep all).
    max_abbr_syls: only generate abbreviated variants for words with at most
                   this many syllables (0 = no limit; default 4).

    Returns list of (keyseq, word, freq, tone).  Lines starting with '#' are
    treated as comments.
    """
    entries = []
    skipped = 0
    dropped_freq = 0
    with open(path, newline='', encoding='utf-8', errors='strict') as f:
        reader = csv.reader(f)
        for lineno, row in enumerate(reader, 1):
            if not row or row[0].startswith('#'):
                continue
            if len(row) < 3:
                continue
            word    = row[0].strip()
            reading = row[2].strip()
            try:
                freq = int(row[1].strip())
            except (ValueError, IndexError):
                freq = 0

            if min_freq > 0 and freq < min_freq:
                dropped_freq += 1
                continue

            phonemes = parse_reading(reading)
            keyseq   = phonemes_to_keyseq(phonemes)
            if keyseq and word:
                tone = reading_to_tone(reading)
                entries.append((keyseq, word, freq, tone))
                n_syls = len(parse_reading_syllables(reading))
                if max_abbr_syls == 0 or n_syls <= max_abbr_syls:
                    for abbr in abbreviated_keyseqs(reading, keyseq):
                        entries.append((abbr, word, freq, tone))
            else:
                skipped += 1

    if dropped_freq:
        print(f"  libchewing: {dropped_freq:,} entries dropped (freq < {min_freq})",
              file=sys.stderr)

    if skipped:
        print(f"  libchewing: {skipped:,} entries skipped (unmappable phonemes)",
              file=sys.stderr)
    return entries

# ── MoE CSV loader ────────────────────────────────────────────────────────

def load_moe_csv(path: str,
                 min_freq: int = 0,
                 max_abbr_syls: int = 4) -> list:
    """
    Parse MoE dictionary CSV (UTF-8 with BOM).
    Expected columns: 注音 / 詞語 / 頻率

    min_freq:      drop both base entries AND abbreviation variants whose
                   freq < this value (0 = no filter, keep all).
    max_abbr_syls: only generate abbreviated variants for words with at most
                   this many syllables (0 = no limit; default 4).

    Returns list of (keyseq, word, freq, tone).
    """
    entries = []
    skipped = 0
    dropped_freq = 0
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

            if min_freq > 0 and freq < min_freq:
                dropped_freq += 1
                continue

            phonemes = parse_reading(reading)
            keyseq   = phonemes_to_keyseq(phonemes)
            if keyseq:
                tone = reading_to_tone(reading)
                entries.append((keyseq, word, freq, tone))
                n_syls = len(parse_reading_syllables(reading))
                if max_abbr_syls == 0 or n_syls <= max_abbr_syls:
                    for abbr in abbreviated_keyseqs(reading, keyseq):
                        entries.append((abbr, word, freq, tone))
            else:
                skipped += 1

    if dropped_freq:
        print(f"  MoE CSV: {dropped_freq:,} entries dropped (freq < {min_freq})",
              file=sys.stderr)

    if skipped:
        print(f"  MoE CSV: {skipped:,} entries skipped (unmappable phonemes)",
              file=sys.stderr)
    return entries

# ── English wordlist loader ───────────────────────────────────────────────

def load_en_wordlist(path: str, min_freq: int = 0) -> list:
    """
    Parse an English word list — one word (+ optional frequency) per line.

    Supported formats:
      word               — no frequency; assigned freq = 1
      word<TAB>freq      — tab-separated (original format)
      word<SPACE>freq    — space-separated (hermitdave/FrequencyWords format)

    Both tab and space separators are handled by splitting on any whitespace.

    min_freq: skip words whose frequency is below this threshold (0 = no filter).
    Frequencies from hermitdave are raw corpus counts (can exceed 10^10); they
    are stored as min(freq, 65535) in the MIED binary, preserving relative order.

    Returns list of (keyseq: bytes, word: str, freq: int).
    Words with unmappable characters (digits, punctuation) are silently dropped.
    """
    entries = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            # Split on any whitespace — handles both TAB and SPACE separators.
            parts = line.split(None, 1)
            word  = parts[0].strip().lower()
            freq  = 1
            if len(parts) == 2:
                try:
                    freq = max(1, int(parts[1].strip()))
                except ValueError:
                    pass

            if min_freq > 0 and freq < min_freq:
                continue

            keyseq = word_to_eng_keyseq(word)
            if keyseq:
                entries.append((keyseq, word, freq, 0))  # tone=0: N/A for English

    return entries

# ── MIED binary builder ───────────────────────────────────────────────────

_OVERSIZED_WORDS = 0  # module-level counter reset in build_mied

def build_value_record(word_list: list) -> bytes:
    """Serialise [(word, freq, tone)] into a v2 ValueRecord blob (freq-descending).

    v2 per-word layout: freq:u16, tone:u8, word_len:u8, word_utf8
    tone: 1-5 for Bopomofo tones; 0 = unknown/unspecified (English).

    Words whose UTF-8 encoding exceeds 30 bytes are silently skipped
    (kCandidateMaxBytes=32 in TrieSearcher; max safe = 31 bytes incl. NUL;
    we cap at 30 to be safe).
    """
    global _OVERSIZED_WORDS
    sorted_words = sorted(word_list, key=lambda x: -x[1])
    valid = []
    for item in sorted_words:
        word, freq = item[0], item[1]
        tone = item[2] if len(item) > 2 else 0
        wb = word.encode('utf-8')
        if len(wb) >= 31:
            _OVERSIZED_WORDS += 1
            continue
        valid.append((wb, freq, tone))
    data = struct.pack('<H', len(valid))
    for wb, freq, tone in valid:
        data += struct.pack('<HBB', min(freq, 0xFFFF), tone, len(wb)) + wb
    return data


def build_mied(entries: list, max_per_key: int = 0) -> tuple:
    """
    Build MIED binary from a list of (keyseq: bytes, word: str, freq: int).

    max_per_key: collision pruning — keep only the top N words per key sequence
                 (sorted by frequency descending) and discard the rest.
                 0 = no limit (default).  Recommended value: 5 (matches the
                 runtime kMaxCandidates limit in TrieSearcher / ImeLogic).

    Returns (dat_bytes, val_bytes, stats_dict, key_to_words).
    key_to_words is the post-pruning dict (keyseq → [(word, freq)]) needed by
    callers that want to emit a charlist.
    """
    global _OVERSIZED_WORDS
    _OVERSIZED_WORDS = 0

    # Aggregate: keyseq → [(word, freq, tone)]
    key_to_words: dict = defaultdict(list)
    for entry in entries:
        keyseq, word, freq = entry[0], entry[1], entry[2]
        tone = entry[3] if len(entry) > 3 else 0
        key_to_words[keyseq].append((word, freq, tone))

    # ── Collision pruning ─────────────────────────────────────────────────
    # For keys that resolve to more words than max_per_key, keep only the top-N
    # most frequent ones.  This bounds en_values.bin size and removes long-tail
    # words that the runtime would never return anyway (kMaxCandidates = 5).
    pruned_total = 0
    if max_per_key > 0:
        for ks in key_to_words:
            wlist = key_to_words[ks]
            if len(wlist) > max_per_key:
                key_to_words[ks] = sorted(wlist, key=lambda x: -x[1])[:max_per_key]
                pruned_total += len(wlist) - max_per_key
                # (tone is at index 2; sorting by freq[-x[1]] preserves tone)

    if pruned_total:
        print(f"  Collision pruning (max_per_key={max_per_key}): "
              f"{pruned_total:,} low-frequency words removed",
              file=sys.stderr)

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

    stored_entries = sum(len(v) for v in key_to_words.values())
    stats = {
        'key_count':          key_count,
        'entry_count_in':     len(entries),     # raw input before pruning
        'entry_count':        stored_entries,   # words actually stored
        'pruned_count':       len(entries) - stored_entries,
        'dat_bytes':          len(dat_bytes),
        'val_bytes':          len(val_data),
    }
    return bytes(dat_bytes), bytes(val_data), stats, dict(key_to_words)

# ── MIED v4 (Composition Architecture) ───────────────────────────────────

# v4 file layout (single binary, all little-endian):
#   header (0x30 bytes):
#     magic                  4 B    "MIE4"
#     version                2 B    u16 = 4
#     flags                  2 B    u16 (reserved, 0)
#     char_count             4 B    u32
#     word_count             4 B    u32
#     char_table_off         4 B    u32 (offset from file start)
#     word_table_off         4 B    u32
#     first_char_idx_off     4 B    u32
#     key_to_char_idx_off    4 B    u32
#     reserved               16 B   (zero)
#
#   char_table:                     (sorted by char_id 0..char_count-1)
#     per char:
#       utf8_len             1 B    u8
#       utf8_bytes           N B    UTF-8 (typically 3 for CJK)
#       reading_count        1 B    u8
#       per reading:
#         key_len            1 B    u8
#         key_bytes          M B    phoneme key sequence
#         tone               1 B    u8 (1-5, 0 = unspecified)
#         base_freq          2 B    u16
#
#   word_table:                     (grouped by char_count for O(1) filter)
#     8 group headers (for char_count 1..8+):
#       group_word_count     4 B    u32  (number of words in this group)
#       group_start_word_id  4 B    u32  (first word_id in this group)
#     per word (in group order):
#       char_count           1 B    u8
#       flags                1 B    u8 (bit 0 = has_reading_overrides)
#       freq                 2 B    u16
#       char_id[N]           2N B   u16 each
#       if flags & 1:
#         reading_idx[N]     N B    u8 each (else implicit 0)
#
#   first_char_idx:
#     offsets[char_count+1]  4 * (char_count+1) B   u32 prefix sum
#     word_ids[total]        4 * total B            u32 (sorted by word.freq desc)
#
#   key_to_char_idx:
#     offsets[24]            4 * 24 B               u32 prefix sum (one slot
#                                                   per phoneme key byte 0x20-0x37)
#     char_ids[total]        2 * total B            u16


def syllable_tone(syllable_phonemes):
    """Extract tone (1-5) from a syllable's phoneme list."""
    for ph in syllable_phonemes:
        if ph == 'ˊ': return 2
        if ph == 'ˇ': return 3
        if ph == 'ˋ': return 4
        if ph == '˙': return 5
    return 1  # implicit tone 1 (no mark)


def load_libchewing_v4(path: str, min_freq: int = 0) -> list:
    """Load libchewing tsi.csv preserving the raw reading string per entry.

    Returns list of (word, freq, reading_string). No abbreviation expansion
    here -- v4 builder derives readings per char from the reading string.
    """
    out = []
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.reader(f)
        for row in reader:
            if not row or row[0].startswith('#') or len(row) < 3:
                continue
            word = row[0].strip()
            reading = row[2].strip()
            try:
                freq = int(row[1].strip())
            except (ValueError, IndexError):
                freq = 0
            if min_freq > 0 and freq < min_freq:
                continue
            if word and reading:
                out.append((word, freq, reading))
    return out


def load_moe_csv_v4(path: str, min_freq: int = 0) -> list:
    """Load MoE CSV preserving reading. Returns (word, freq, reading)."""
    out = []
    with open(path, newline='', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        for row in reader:
            reading = row.get('注音', '').strip()
            word    = row.get('詞語', '').strip()
            try:
                freq = max(1, int(row.get('頻率', 1)))
            except (ValueError, TypeError):
                freq = 1
            if min_freq > 0 and freq < min_freq:
                continue
            if word and reading:
                out.append((word, freq, reading))
    return out


def _is_cjk(ch: str) -> bool:
    cp = ord(ch)
    return 0x3400 <= cp <= 0x9FFF or 0x20000 <= cp <= 0x2FFFF


def build_char_table_v4(raw_entries: list,
                        unihan_readings: dict = None) -> tuple:
    """Build char_table from raw (word, freq, reading) entries.

    For each char in each word, parse the corresponding syllable and record
    (key_bytes, tone, freq) as one of the char's readings. Aggregate freq
    via max across all sightings.

    unihan_readings: optional {char: [(key_bytes, tone)]} dict to seed
    chars not present in raw_entries (low base_freq for rarity).

    Returns (char_to_id, char_table) where char_table is a list ordered by
    char_id, each entry = (utf8_char, sorted_readings_freq_desc).
    """
    # char -> {(key_bytes, tone): max_freq_seen}
    # Use plain dict to avoid defaultdict side-effect of creating empty entries
    # when a char's reading is unmappable.
    accum = {}

    for word, freq, reading in raw_entries:
        chars = list(word)
        if not all(_is_cjk(c) for c in chars):
            continue  # skip non-CJK words (rare in MIE corpus)
        sylls = parse_reading_syllables(reading)
        if len(sylls) != len(chars):
            # Reading/word mismatch -- could be erhua, foreign loanwords, etc.
            # Skip per-char inference but the WORD itself might still go to
            # word_table later (it'll be skipped if any char missing).
            continue
        # libchewing assigns freq=0 to many low-usage entries; treat as 1 so
        # the reading is still recorded with a minimum-rank base_freq.
        eff_freq = max(freq, 1)
        for ch, syl in zip(chars, sylls):
            keyseq = phonemes_to_keyseq(syl)
            tone = syllable_tone(syl)
            if not keyseq:
                continue
            key = (keyseq, tone)
            ch_dict = accum.setdefault(ch, {})
            existing = ch_dict.get(key, 0)
            if eff_freq > existing:
                ch_dict[key] = eff_freq

    # Optionally seed from Unihan for chars never seen in raw entries
    DEFAULT_RARE_FREQ = 5
    if unihan_readings:
        for ch, readings in unihan_readings.items():
            if ch not in accum:
                for keyseq, tone in readings:
                    accum[ch][(keyseq, tone)] = DEFAULT_RARE_FREQ

    # Build sorted char_table: ascending by codepoint for stable char_ids
    char_to_id = {}
    char_table = []
    for cid, ch in enumerate(sorted(accum.keys())):
        char_to_id[ch] = cid
        # Sort readings: most-frequent first (so reading[0] is the default)
        sorted_readings = sorted(
            ((k, t, f) for (k, t), f in accum[ch].items()),
            key=lambda r: -r[2]
        )
        char_table.append((ch, sorted_readings))

    return char_to_id, char_table


def build_word_table_v4(raw_entries: list,
                        char_to_id: dict, char_table: list) -> list:
    """Build word_table from raw entries.

    For each multi-char word with a parseable reading whose syllable count
    matches char count: derive reading_idx[i] for each char by finding
    which char_table reading matches the word's syllable for that position.

    Skips words where any char is missing from char_to_id (i.e., chars
    that didn't make it into char_table). This shouldn't happen if char_table
    was built from the same raw_entries.

    Words are deduped; max freq wins.

    Returns list of (word_str, char_ids, reading_idxs, freq, tone).
    """
    # First pass: dedup by word, keep max-freq entry's reading
    best = {}
    for word, freq, reading in raw_entries:
        if len(word) < 2:
            continue
        if not all(_is_cjk(c) for c in word):
            continue
        if not all(c in char_to_id for c in word):
            continue
        sylls = parse_reading_syllables(reading)
        if len(sylls) != len(word):
            continue
        prev = best.get(word)
        if prev is None or freq > prev[0]:
            best[word] = (freq, reading, sylls)

    # Second pass: for each best entry, derive reading_idx per char
    word_table = []
    for word, (freq, reading, sylls) in best.items():
        char_ids = [char_to_id[c] for c in word]
        reading_idxs = []
        for ch_idx, (ch, syl) in enumerate(zip(word, sylls)):
            keyseq = phonemes_to_keyseq(syl)
            tone = syllable_tone(syl)
            cid = char_to_id[ch]
            readings = char_table[cid][1]
            # Find matching reading; default to 0 if not found
            idx = 0
            for i, (rk, rt, _rf) in enumerate(readings):
                if rk == keyseq and rt == tone:
                    idx = i
                    break
            reading_idxs.append(idx)
        last_tone = syllable_tone(sylls[-1]) if sylls else 0
        word_table.append((word, char_ids, reading_idxs, freq, last_tone))

    return word_table


def serialize_char_table_v4(char_table: list) -> tuple:
    """Per char: utf8_len + utf8 + reading_count + per reading.
    Returns (bytes, offsets_list) where offsets_list[i] is the byte offset
    of char_id i from char_table start. offsets_list has char_count + 1
    entries (last entry = total bytes, useful as a sentinel)."""
    buf = bytearray()
    offsets = []
    for ch, readings in char_table:
        offsets.append(len(buf))
        utf8 = ch.encode('utf-8')
        buf.append(len(utf8))
        buf += utf8
        buf.append(len(readings))
        for keyseq, tone, freq in readings:
            buf.append(len(keyseq))
            buf += keyseq
            buf.append(tone)
            buf += struct.pack('<H', min(freq, 0xFFFF))
    offsets.append(len(buf))
    return bytes(buf), offsets


def serialize_word_table_v4(word_table: list) -> tuple:
    """Group words by char_count (1..8+, 8 groups) so the runtime can
    seek directly to a target_char_count's word_id range without scanning.

    Within each group, words are sorted by freq desc (so first_char_index
    can reference word_ids in freq order naturally).

    Returns (bytes, group_ranges, flat, offsets) where:
      group_ranges: {char_count_bucket -> (start_word_id, count)}
      flat:         list of word entries in word_id order
      offsets:      list of word_count + 1 byte offsets within word_table
                    (each word_id's start; last entry = total bytes)
    """
    MAX_GROUP = 8  # bucket >=8 chars together
    grouped = defaultdict(list)
    for word, char_ids, reading_idxs, freq, tone in word_table:
        n = len(char_ids)
        bucket = n if n < MAX_GROUP else MAX_GROUP
        grouped[bucket].append((word, char_ids, reading_idxs, freq, tone))

    # Sort each group by freq desc
    for bucket in grouped:
        grouped[bucket].sort(key=lambda w: -w[3])

    # Assign word_ids in group order: bucket 1 first, then 2, ..., then 8+
    flat = []
    group_ranges = {}
    for bucket in range(1, MAX_GROUP + 1):
        wlist = grouped.get(bucket, [])
        group_ranges[bucket] = (len(flat), len(wlist))
        flat.extend(wlist)

    # Group headers: per bucket 1..8 -> u32 count + u32 start_word_id.
    # NOTE: word_offsets are measured from the START of the FIRST WORD RECORD,
    # i.e. AFTER the 8 group headers (8*8 = 64 bytes). This matches the
    # CompositionSearcher::build_offset_indexes loop which steps `w` past
    # the 64-byte group-headers prelude before recording the first offset.
    buf = bytearray()
    for bucket in range(1, MAX_GROUP + 1):
        start, count = group_ranges[bucket]
        buf += struct.pack('<II', count, start)

    headers_size = len(buf)
    offsets = []  # offsets relative to the WHOLE word_table section start

    # Word records
    for word, char_ids, reading_idxs, freq, tone in flat:
        offsets.append(len(buf))
        n = len(char_ids)
        # Decide whether reading_idx is all-zero (omit it then)
        has_overrides = any(r != 0 for r in reading_idxs)
        flags = 1 if has_overrides else 0
        buf.append(n)
        buf.append(flags)
        buf += struct.pack('<H', min(freq, 0xFFFF))
        for cid in char_ids:
            buf += struct.pack('<H', cid)
        if has_overrides:
            for r in reading_idxs:
                buf.append(r)
    offsets.append(len(buf))

    return bytes(buf), group_ranges, flat, offsets


def build_syllable_prefix_set(raw_entries: list) -> list:
    """Extract every distinct Bopomofo syllable prefix (1..4 key bytes) from
    the raw libchewing/pj-data entries — the canonical phonetic source.
    Ships with the v4 file so the runtime never needs to re-derive rules
    from char_table.
    Returns a sorted list of uint64-packed prefixes ready to serialize.
    """
    prefixes = set()
    for word, _freq, reading in raw_entries:
        sylls = parse_reading_syllables(reading) if reading else []
        for syl in sylls:
            kb = phonemes_to_keyseq(syl)
            if not kb:
                continue
            max_len = min(len(kb), 4)
            for L in range(1, max_len + 1):
                prefix = kb[:L]
                v = L << 32
                for i, b in enumerate(prefix):
                    v |= b << (i * 8)
                prefixes.add(v)
    return sorted(prefixes)


def serialize_syllable_prefix_table_v4(prefixes: list) -> bytes:
    """Serialize the sorted prefix list as: u32 count | count * u64 entries."""
    out = bytearray()
    out += struct.pack('<I', len(prefixes))
    for v in prefixes:
        out += struct.pack('<Q', v)
    return bytes(out)


def build_syllable_prefix_set_from_char_table(char_table: list) -> list:
    """Fallback: extract prefixes from the already-built char_table. Used
    when raw_entries don't carry per-char reading lists (e.g. Unihan-only
    chars that were injected after char_table assembly)."""
    prefixes = set()
    for _ch, readings in char_table:
        for (kb, _tone, _freq) in readings:
            if not kb:
                continue
            max_len = min(len(kb), 4)
            for L in range(1, max_len + 1):
                prefix = kb[:L]
                v = L << 32
                for i, b in enumerate(prefix):
                    v |= b << (i * 8)
                prefixes.add(v)
    return sorted(prefixes)


def serialize_offsets_v4(offsets: list) -> bytes:
    """Pack a list of u32 byte offsets little-endian."""
    return b''.join(struct.pack('<I', o) for o in offsets)


def serialize_first_char_idx_v4(flat_word_table: list, char_count: int) -> bytes:
    """For each char_id, list of word_ids whose first char is that char_id,
    sorted by freq desc. Layout: u32 offsets[char_count+1] + u32 word_ids[total].
    """
    by_first_char = defaultdict(list)
    for wid, (word, char_ids, reading_idxs, freq, tone) in enumerate(flat_word_table):
        if char_ids:
            by_first_char[char_ids[0]].append((freq, wid))
    for cid in by_first_char:
        by_first_char[cid].sort(key=lambda x: -x[0])  # freq desc

    # Offsets
    offsets = [0] * (char_count + 1)
    for cid in range(char_count):
        offsets[cid + 1] = offsets[cid] + len(by_first_char.get(cid, []))

    buf = bytearray()
    for off in offsets:
        buf += struct.pack('<I', off)
    for cid in range(char_count):
        for _freq, wid in by_first_char.get(cid, []):
            buf += struct.pack('<I', wid)
    return bytes(buf)


def serialize_key_to_char_idx_v4(char_table: list) -> bytes:
    """For each phoneme key byte (0x20..0x37), list of char_ids whose first
    reading's first byte matches. Layout: u32 offsets[25] + u16 char_ids[total].
    Slot 0 -> key byte 0x20, slot 24 -> key byte 0x38 (exclusive).
    """
    KEY_MIN = 0x20
    KEY_MAX = 0x38  # exclusive
    NUM_SLOTS = KEY_MAX - KEY_MIN  # 24

    by_key = defaultdict(list)
    for cid, (ch, readings) in enumerate(char_table):
        # Each char contributes one entry per unique first-byte across its readings.
        # The runtime uses this to find candidate first-chars matching user_keys[0].
        seen_first = set()
        for keyseq, tone, freq in readings:
            if keyseq:
                fb = keyseq[0]
                if fb not in seen_first:
                    seen_first.add(fb)
                    if KEY_MIN <= fb < KEY_MAX:
                        by_key[fb].append(cid)
    for fb in by_key:
        by_key[fb].sort()  # ascending char_id (deterministic order)

    offsets = [0] * (NUM_SLOTS + 1)
    for slot in range(NUM_SLOTS):
        fb = KEY_MIN + slot
        offsets[slot + 1] = offsets[slot] + len(by_key.get(fb, []))

    buf = bytearray()
    for off in offsets:
        buf += struct.pack('<I', off)
    for slot in range(NUM_SLOTS):
        fb = KEY_MIN + slot
        for cid in by_key.get(fb, []):
            buf += struct.pack('<H', cid)
    return bytes(buf)


def build_mied_v4(raw_entries: list,
                  unihan_readings: dict = None) -> tuple:
    """Build a complete MIED v4 binary blob from raw (word, freq, reading)
    entries. Returns (bytes, stats_dict).

    The v4 binary embeds char_offsets and word_offsets sections so the
    runtime CompositionSearcher does NOT need heap allocations for them
    (Core 1 has only 48 KB FreeRTOS heap, far less than the ~590 KB the
    on-the-fly offset arrays would need).
    """
    char_to_id, char_table = build_char_table_v4(raw_entries, unihan_readings)
    word_table = build_word_table_v4(raw_entries, char_to_id, char_table)

    char_section, char_offsets = serialize_char_table_v4(char_table)
    word_section, group_ranges, flat, word_offsets = \
        serialize_word_table_v4(word_table)
    first_idx = serialize_first_char_idx_v4(flat, len(char_to_id))
    key_idx   = serialize_key_to_char_idx_v4(char_table)
    char_off_section = serialize_offsets_v4(char_offsets)
    word_off_section = serialize_offsets_v4(word_offsets)

    # Syllable prefix table: extract directly from raw reading strings so
    # the ruleset is grounded in the source data, not a re-derivation of
    # the encoded char_table. Fall back to char_table if raw_entries don't
    # expose per-char phoneme lists.
    prefixes = build_syllable_prefix_set(raw_entries)
    if not prefixes:
        prefixes = build_syllable_prefix_set_from_char_table(char_table)
    prefix_section = serialize_syllable_prefix_table_v4(prefixes)

    # Header is 0x30 = 48 bytes
    HEADER_SIZE = 0x30
    char_off       = HEADER_SIZE
    word_off       = char_off + len(char_section)
    first_off      = word_off + len(word_section)
    key_off        = first_off + len(first_idx)
    char_offs_off  = key_off + len(key_idx)
    word_offs_off  = char_offs_off + len(char_off_section)
    prefix_off     = word_offs_off + len(word_off_section)
    total_size     = prefix_off + len(prefix_section)

    header = bytearray()
    header += MAGIC_V4
    header += struct.pack('<HH', VERSION_V4, 0)
    header += struct.pack('<II', len(char_table), len(flat))
    header += struct.pack('<IIII',
                          char_off, word_off, first_off, key_off)
    header += struct.pack('<I', total_size)         # offset 0x20
    header += struct.pack('<I', char_offs_off)      # offset 0x24
    header += struct.pack('<I', word_offs_off)      # offset 0x28
    header += struct.pack('<I', prefix_off)         # offset 0x2C (NEW)
    header += b'\x00' * (HEADER_SIZE - len(header))
    assert len(header) == HEADER_SIZE

    blob = (bytes(header)
            + char_section + word_section
            + first_idx + key_idx
            + char_off_section + word_off_section
            + prefix_section)
    assert len(blob) == total_size

    # Stats
    n_words_with_overrides = sum(
        1 for (_w, _c, ridx, _f, _t) in flat if any(r != 0 for r in ridx))
    char_count_dist = defaultdict(int)
    for (_w, char_ids, _r, _f, _t) in flat:
        n = len(char_ids)
        bucket = n if n < 8 else 8  # collapse 8+ into one bucket for display
        char_count_dist[bucket] += 1
    total_readings = sum(len(rs) for _ch, rs in char_table)

    stats = {
        'magic': 'MIE4',
        'version': VERSION_V4,
        'char_count': len(char_table),
        'word_count': len(flat),
        'word_count_by_size': dict(char_count_dist),
        'total_readings': total_readings,
        'avg_readings_per_char': total_readings / max(1, len(char_table)),
        'words_with_reading_overrides': n_words_with_overrides,
        'header_bytes':            HEADER_SIZE,
        'char_table_bytes':        len(char_section),
        'word_table_bytes':        len(word_section),
        'first_char_idx_bytes':    len(first_idx),
        'key_to_char_idx_bytes':   len(key_idx),
        'char_offsets_bytes':      len(char_off_section),
        'word_offsets_bytes':      len(word_off_section),
        'total_bytes':             len(blob),
    }
    return blob, stats


# ── Argument parsing ──────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='Compile Nokia-style key-sequence dictionary for MokyaLora MIE.')
    p.add_argument('--libchewing',     metavar='TSI',
                   help='libchewing-data tsi.csv path (comma-separated: word,freq,reading)')
    p.add_argument('--moe-csv',        metavar='CSV',
                   help='MoE word list CSV path (注音/詞語/頻率 columns, UTF-8 BOM)')
    p.add_argument('--zh-min-freq',     metavar='N', type=int, default=0,
                   help='Drop Chinese entries (both base and abbreviated) whose freq < N '
                        '(0 = no filter, default).  libchewing freq is a numeric usage '
                        'count; MoE freq likewise.  Raise this to shrink the dict under '
                        'a fixed PSRAM budget while keeping the most-used words.')
    p.add_argument('--zh-max-abbr-syls', metavar='N', type=int, default=4,
                   help='Only generate abbreviated ZH variants for words with <= N '
                        'syllables (0 = no limit; default: 4).  Words longer than N '
                        'syllables are stored under their full key sequence only.')
    p.add_argument('--zh-max-per-key',  metavar='N', type=int, default=0,
                   help='Collision pruning: keep only the top-N Chinese words per key '
                        'sequence (sorted by freq descending).  0 = no limit (default).  '
                        'Use to bound dict_values.bin when many homophones share a key.')
    p.add_argument('--en-wordlist',    metavar='TXT',
                   help='English word list — one word per line, optional space or TAB '
                        'frequency.  Supports hermitdave/FrequencyWords (space-separated) '
                        'and the original TAB-separated format.')
    p.add_argument('--en-max-words',   metavar='N', type=int, default=0,
                   help='Limit English word list to first N words before deduplication '
                        '(0 = no limit, default)')
    p.add_argument('--en-min-freq',    metavar='N', type=int, default=0,
                   help='Skip English words whose frequency is below N  '
                        '(0 = no filter, default).  Useful with hermitdave/FrequencyWords '
                        'to drop very rare words and keep en_dat+en_values under 1 MB.')
    p.add_argument('--en-max-per-key', metavar='N', type=int, default=5,
                   help='Collision pruning: keep only the top N words per key sequence '
                        '(default: 5, matching the runtime kMaxCandidates limit).  '
                        '0 = no limit.')
    p.add_argument('--emit-charlist',  metavar='PATH',
                   help='Write unique output characters (one per line) to PATH '
                        '(small-font charlist; derived from Chinese dict only)')
    p.add_argument('--output-dir',     default=str(OUTPUT_DIR),
                   help=f'Output directory  [default: {OUTPUT_DIR}]')
    p.add_argument('--v4-output',      metavar='PATH', default=None,
                   help='Also build the MIED v4 (Composition Architecture) dict '
                        'and write the single combined binary to PATH. '
                        'When set, gen_dict.py emits BOTH v2 (legacy MIED) and v4 '
                        '(MIE4); they consume the same Chinese sources but the v4 '
                        'output skips abbreviation expansion (composed at search '
                        'time). v4 is typically much smaller. Default: not built.')
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
    if args.libchewing or args.moe_csv:
        abbr_syls_label = str(args.zh_max_abbr_syls) if args.zh_max_abbr_syls else '無限制'
        freq_label      = f'>= {args.zh_min_freq}' if args.zh_min_freq else '不限'
        per_key_label   = str(args.zh_max_per_key) if args.zh_max_per_key else '無限制'
        print(f'ZH filter: 音節上限={abbr_syls_label}  最低頻率={freq_label}  '
              f'每鍵上限={per_key_label}')

    zh_entries: list = []

    if args.libchewing:
        print(f'Loading libchewing  {args.libchewing} ...')
        loaded = load_libchewing(args.libchewing,
                                 min_freq=args.zh_min_freq,
                                 max_abbr_syls=args.zh_max_abbr_syls)
        zh_entries.extend(loaded)
        print(f'  {len(loaded):,} entries')

    if args.moe_csv:
        print(f'Loading MoE CSV     {args.moe_csv} ...')
        loaded = load_moe_csv(args.moe_csv,
                              min_freq=args.zh_min_freq,
                              max_abbr_syls=args.zh_max_abbr_syls)
        zh_entries.extend(loaded)
        print(f'  {len(loaded):,} entries')

    # PSRAM budget for the Chinese dict (plan: docs/design-notes/mie-architecture.md).
    # Printed purely as a guideline; exceeding it does not fail the build.
    ZH_BUDGET_BYTES = 3 * 1024 * 1024

    zh_stats = None
    zh_key_to_words = None
    if zh_entries:
        print(f'Building Chinese MIED  ({len(zh_entries):,} total entries) ...')
        dat, val, zh_stats, zh_key_to_words = build_mied(
            zh_entries, max_per_key=args.zh_max_per_key)
        (output_dir / 'dict_dat.bin').write_bytes(dat)
        (output_dir / 'dict_values.bin').write_bytes(val)
        total_zh  = zh_stats['dat_bytes'] + zh_stats['val_bytes']
        budget_mb = total_zh / (1024 * 1024)
        budget_ok = '[OK]' if total_zh <= ZH_BUDGET_BYTES else '[OVER BUDGET]'
        print(f'  dict_dat.bin    : {zh_stats["dat_bytes"]:>9,} bytes  '
              f'({zh_stats["key_count"]:,} unique key sequences)')
        print(f'  dict_values.bin : {zh_stats["val_bytes"]:>9,} bytes')
        print(f'  ZH total        : {total_zh:>9,} bytes  '
              f'({budget_mb:.2f} MB / 3.00 MB budget)  {budget_ok}')

    # ── MIED v4 build (Composition Architecture) ──────────────────────────
    # Built from the SAME Chinese source files (libchewing, MoE) but using
    # the raw word/freq/reading triples (no pre-computed abbreviation keys).
    # Search-time composition derives readings dynamically from char_table.
    if args.v4_output and (args.libchewing or args.moe_csv):
        v4_path = Path(args.v4_output)
        v4_path.parent.mkdir(parents=True, exist_ok=True)

        print(f'\nBuilding MIED v4 (Composition Architecture) ...')
        raw_v4 = []
        if args.libchewing:
            r = load_libchewing_v4(args.libchewing,
                                    min_freq=args.zh_min_freq)
            raw_v4.extend(r)
            print(f'  v4 raw libchewing entries: {len(r):,}')
        if args.moe_csv:
            r = load_moe_csv_v4(args.moe_csv,
                                min_freq=args.zh_min_freq)
            raw_v4.extend(r)
            print(f'  v4 raw MoE entries:        {len(r):,}')

        v4_blob, v4_stats = build_mied_v4(raw_v4, unihan_readings=None)
        v4_path.write_bytes(v4_blob)

        v4_mb = len(v4_blob) / (1024 * 1024)
        print(f'  v4 char_table:             {v4_stats["char_table_bytes"]:>9,} bytes  '
              f'({v4_stats["char_count"]:,} unique chars, '
              f'avg {v4_stats["avg_readings_per_char"]:.2f} readings/char)')
        print(f'  v4 word_table:             {v4_stats["word_table_bytes"]:>9,} bytes  '
              f'({v4_stats["word_count"]:,} multi-char words, '
              f'{v4_stats["words_with_reading_overrides"]:,} with reading overrides)')
        print(f'  v4 first_char_idx:         {v4_stats["first_char_idx_bytes"]:>9,} bytes')
        print(f'  v4 key_to_char_idx:        {v4_stats["key_to_char_idx_bytes"]:>9,} bytes')
        print(f'  v4 char_offsets:           {v4_stats["char_offsets_bytes"]:>9,} bytes')
        print(f'  v4 word_offsets:           {v4_stats["word_offsets_bytes"]:>9,} bytes')
        print(f'  v4 TOTAL                   {v4_stats["total_bytes"]:>9,} bytes  '
              f'({v4_mb:.2f} MB)  -> {v4_path}')

        # Word size distribution
        sz_dist = v4_stats['word_count_by_size']
        if sz_dist:
            print(f'  v4 word_count_by_size:')
            for n in sorted(sz_dist):
                label = f'{n}-char' if n < 8 else '8+ char'
                print(f'    {label}: {sz_dist[n]:,}')

    # ── Charlist emit (small font variant) ────────────────────────────────
    if args.emit_charlist and zh_key_to_words:
        seen: set = set()
        chars: list = []
        for ks in sorted(zh_key_to_words.keys()):
            for item in zh_key_to_words[ks]:
                word = item[0]
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
        freq_label = f'>= {args.en_min_freq}' if args.en_min_freq else '不限'
        key_label  = str(args.en_max_per_key) if args.en_max_per_key else '無限制'
        print(f'EN filter: 最低頻率={freq_label}  每鍵上限={key_label}')
        print(f'Loading English     {args.en_wordlist} ...')
        en_entries = load_en_wordlist(args.en_wordlist, min_freq=args.en_min_freq)
        if args.en_max_words and args.en_max_words > 0:
            en_entries = en_entries[:args.en_max_words]
            print(f'  truncated to {args.en_max_words:,} words (--en-max-words)')
        print(f'  {len(en_entries):,} entries loaded')
        print(f'Building English MIED ...')
        dat, val, en_stats, _ = build_mied(en_entries, max_per_key=args.en_max_per_key)
        (output_dir / 'en_dat.bin').write_bytes(dat)
        (output_dir / 'en_values.bin').write_bytes(val)
        total_en  = en_stats['dat_bytes'] + en_stats['val_bytes']
        budget_mb = total_en / (1024 * 1024)
        budget_ok = '[OK]' if total_en < 1024 * 1024 else '[OVER BUDGET]'
        print(f'  en_dat.bin      : {en_stats["dat_bytes"]:>9,} bytes  '
              f'({en_stats["key_count"]:,} unique key sequences)')
        pruned_note = (f', {en_stats["pruned_count"]:,} pruned'
                       if en_stats['pruned_count'] else '')
        print(f'  en_values.bin   : {en_stats["val_bytes"]:>9,} bytes  '
              f'({en_stats["entry_count"]:,} words stored{pruned_note})')
        print(f'  EN total        : {total_en:>9,} bytes  '
              f'({budget_mb:.2f} MB / 1.00 MB budget)  {budget_ok}')

    # ── Metadata ──────────────────────────────────────────────────────────
    meta = {
        'format_version': VERSION,
        'key_offset':     KEY_OFFSET,
        'key_encoding':   'key_byte = key_index + 0x21 (printable ASCII, no NULLs)',
        'variant':        'sm' if args.emit_charlist else 'lg',
        'sources': {
            'libchewing':    args.libchewing,
            'moe_csv':       args.moe_csv,
            'en_wordlist':   args.en_wordlist,
            'en_max_words':  args.en_max_words  if args.en_max_words  else None,
            'en_min_freq':   args.en_min_freq   if args.en_min_freq   else None,
            'en_max_per_key': args.en_max_per_key if args.en_max_per_key else None,
            'zh_min_freq':      args.zh_min_freq      if args.zh_min_freq      else None,
            'zh_max_abbr_syls': args.zh_max_abbr_syls if args.zh_max_abbr_syls else None,
            'zh_max_per_key':   args.zh_max_per_key   if args.zh_max_per_key   else None,
        },
        'charlist_path':  args.emit_charlist,
        'chinese': zh_stats,
        'english': en_stats,
        'built_at': datetime.now(timezone.utc).isoformat(),
        'license_notices': [
            'libchewing-data: (c) LibChewing contributors, LGPL-2.1 '
            '(https://github.com/chewing/libchewing-data)',
            'MoE dictionary: (c) Republic of China Ministry of Education, '
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
