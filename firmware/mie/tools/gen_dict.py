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
                    min_freq_for_abbr: int = 0,
                    max_abbr_syls: int = 4) -> list:
    """
    Parse libchewing tsi.csv (comma-separated: word, freq, reading).

    tsi.csv column order: word, freq (0 or 1), reading
    freq is a binary flag: 1 = common word, 0 = rare character.
    Common words (freq=1) sort above rare characters (freq=0) in the candidate list.

    min_freq_for_abbr: only generate abbreviated variants when freq >= this value
                       (0 = no filter).
    max_abbr_syls:     only generate abbreviated variants for words with at most this
                       many syllables (0 = no limit).

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
                tone = reading_to_tone(reading)
                entries.append((keyseq, word, freq, tone))
                n_syls = len(parse_reading_syllables(reading))
                emit_abbr = (
                    (min_freq_for_abbr == 0 or freq >= min_freq_for_abbr) and
                    (max_abbr_syls == 0 or n_syls <= max_abbr_syls)
                )
                if emit_abbr:
                    for abbr in abbreviated_keyseqs(reading, keyseq):
                        entries.append((abbr, word, freq, tone))
            else:
                skipped += 1

    if skipped:
        print(f"  libchewing: {skipped:,} entries skipped (unmappable phonemes)",
              file=sys.stderr)
    return entries

# ── MoE CSV loader ────────────────────────────────────────────────────────

def load_moe_csv(path: str,
                 min_freq_for_abbr: int = 0,
                 max_abbr_syls: int = 4) -> list:
    """
    Parse MoE dictionary CSV (UTF-8 with BOM).
    Expected columns: 注音 / 詞語 / 頻率

    min_freq_for_abbr: only generate abbreviated variants when freq >= this value
                       (0 = no filter).
    max_abbr_syls:     only generate abbreviated variants for words with at most this
                       many syllables (0 = no limit).

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
                tone = reading_to_tone(reading)
                entries.append((keyseq, word, freq, tone))
                n_syls = len(parse_reading_syllables(reading))
                emit_abbr = (
                    (min_freq_for_abbr == 0 or freq >= min_freq_for_abbr) and
                    (max_abbr_syls == 0 or n_syls <= max_abbr_syls)
                )
                if emit_abbr:
                    for abbr in abbreviated_keyseqs(reading, keyseq):
                        entries.append((abbr, word, freq, tone))
            else:
                skipped += 1

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

# ── Argument parsing ──────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description='Compile Nokia-style key-sequence dictionary for MokyaLora MIE.')
    p.add_argument('--libchewing',     metavar='TSI',
                   help='libchewing-data tsi.csv path (comma-separated: word,freq,reading)')
    p.add_argument('--moe-csv',        metavar='CSV',
                   help='MoE word list CSV path (注音/詞語/頻率 columns, UTF-8 BOM)')
    p.add_argument('--zh-min-freq',     metavar='N', type=int, default=0,
                   help='Only generate abbreviated ZH variants for words with freq >= N '
                        '(0 = no filter, default).  libchewing freq is binary 0/1; '
                        'MoE freq is a numeric count.')
    p.add_argument('--zh-max-abbr-syls', metavar='N', type=int, default=4,
                   help='Only generate abbreviated ZH variants for words with <= N '
                        'syllables (0 = no limit; default: 4).  Words longer than N '
                        'syllables are stored under their full key sequence only.')
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
        print(f'ZH abbrev filter: 音節上限={abbr_syls_label}  最低頻率={freq_label}')

    zh_entries: list = []

    if args.libchewing:
        print(f'Loading libchewing  {args.libchewing} ...')
        loaded = load_libchewing(args.libchewing,
                                 min_freq_for_abbr=args.zh_min_freq,
                                 max_abbr_syls=args.zh_max_abbr_syls)
        zh_entries.extend(loaded)
        print(f'  {len(loaded):,} entries')

    if args.moe_csv:
        print(f'Loading MoE CSV     {args.moe_csv} ...')
        loaded = load_moe_csv(args.moe_csv,
                              min_freq_for_abbr=args.zh_min_freq,
                              max_abbr_syls=args.zh_max_abbr_syls)
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
        budget_ok = '\u2713 OK' if total_en < 1024 * 1024 else '\u26a0 OVER BUDGET'
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
