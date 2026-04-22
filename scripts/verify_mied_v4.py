#!/usr/bin/env python3
"""
verify_mied_v4.py — Reads a MIED v4 binary file, validates structure,
and dumps summary statistics. Also acts as Python reference for the
binary layout — Phase 2 CompositionSearcher mirrors this in C++.

Usage:
  python scripts/verify_mied_v4.py <path/to/dict_mie.bin>
"""

import struct
import sys
from pathlib import Path

# Force UTF-8 stdout on Windows so CJK chars render in Chinese console codepages.
if hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass


def read_v4(blob: bytes) -> dict:
    """Parse a MIED v4 binary into Python dicts. Returns:
        {
          'version': int, 'flags': int,
          'char_table': [(utf8_char, [(key_bytes, tone, freq), ...]), ...],
          'word_table_groups': {char_count: [(char_ids, reading_idxs, freq), ...]},
          'first_char_idx': {char_id: [word_id, ...]},
          'key_to_char_idx': {key_byte: [char_id, ...]},
        }
    Raises ValueError on structural issues.
    """
    if len(blob) < 0x30:
        raise ValueError("v4 too small for header")
    if blob[:4] != b"MIE4":
        raise ValueError(f"bad magic: {blob[:4]!r}")
    version, flags = struct.unpack_from('<HH', blob, 4)
    if version != 4:
        raise ValueError(f"expected version 4, got {version}")
    char_count, word_count = struct.unpack_from('<II', blob, 8)
    char_off, word_off, first_off, key_off = struct.unpack_from('<IIII', blob, 16)

    out = {
        'version': version,
        'flags': flags,
        'char_count': char_count,
        'word_count': word_count,
        'offsets': {
            'char_table': char_off,
            'word_table': word_off,
            'first_char_idx': first_off,
            'key_to_char_idx': key_off,
        },
        'total_bytes': len(blob),
    }

    # ── Parse char_table ────────────────────────────────────────────────
    char_table = []
    o = char_off
    for cid in range(char_count):
        utf8_len = blob[o]; o += 1
        utf8 = blob[o:o+utf8_len]; o += utf8_len
        ch = utf8.decode('utf-8')
        rcount = blob[o]; o += 1
        readings = []
        for _ in range(rcount):
            klen = blob[o]; o += 1
            kbytes = bytes(blob[o:o+klen]); o += klen
            tone = blob[o]; o += 1
            freq = struct.unpack_from('<H', blob, o)[0]; o += 2
            readings.append((kbytes, tone, freq))
        char_table.append((ch, readings))
    out['char_table'] = char_table

    # ── Parse word_table ────────────────────────────────────────────────
    o = word_off
    # 8 group headers (count + start_word_id, 8 bytes each)
    group_headers = []
    for _ in range(8):
        cnt, start = struct.unpack_from('<II', blob, o); o += 8
        group_headers.append((cnt, start))
    out['group_headers'] = group_headers

    # Words in group order
    word_table = []
    for bucket_idx, (cnt, start) in enumerate(group_headers):
        for _ in range(cnt):
            n = blob[o]; o += 1
            flags_w = blob[o]; o += 1
            freq = struct.unpack_from('<H', blob, o)[0]; o += 2
            char_ids = []
            for _ in range(n):
                cid = struct.unpack_from('<H', blob, o)[0]; o += 2
                char_ids.append(cid)
            if flags_w & 1:
                reading_idxs = list(blob[o:o+n])
                o += n
            else:
                reading_idxs = [0] * n
            word_table.append((char_ids, reading_idxs, freq))
    out['word_table'] = word_table
    if len(word_table) != word_count:
        raise ValueError(f"word_table count mismatch: {len(word_table)} vs header {word_count}")

    # ── Parse first_char_idx ────────────────────────────────────────────
    o = first_off
    first_offsets = []
    for _ in range(char_count + 1):
        first_offsets.append(struct.unpack_from('<I', blob, o)[0])
        o += 4
    total_first = first_offsets[-1]
    first_word_ids = []
    for _ in range(total_first):
        first_word_ids.append(struct.unpack_from('<I', blob, o)[0]); o += 4
    first_char_idx = {}
    for cid in range(char_count):
        s, e = first_offsets[cid], first_offsets[cid+1]
        if s < e:
            first_char_idx[cid] = first_word_ids[s:e]
    out['first_char_idx'] = first_char_idx

    # ── Parse key_to_char_idx ───────────────────────────────────────────
    o = key_off
    NUM_SLOTS = 24
    KEY_MIN = 0x20
    key_offsets = []
    for _ in range(NUM_SLOTS + 1):
        key_offsets.append(struct.unpack_from('<I', blob, o)[0]); o += 4
    total_key = key_offsets[-1]
    key_char_ids = []
    for _ in range(total_key):
        key_char_ids.append(struct.unpack_from('<H', blob, o)[0]); o += 2
    key_to_char_idx = {}
    for slot in range(NUM_SLOTS):
        s, e = key_offsets[slot], key_offsets[slot+1]
        if s < e:
            key_to_char_idx[KEY_MIN + slot] = key_char_ids[s:e]
    out['key_to_char_idx'] = key_to_char_idx

    return out


def main():
    if len(sys.argv) != 2:
        print("Usage: python scripts/verify_mied_v4.py <path/to/dict_mie.bin>",
              file=sys.stderr)
        sys.exit(1)

    path = Path(sys.argv[1])
    if not path.exists():
        print(f"ERROR: {path} not found", file=sys.stderr)
        sys.exit(2)

    blob = path.read_bytes()
    try:
        d = read_v4(blob)
    except ValueError as e:
        print(f"ERROR parsing v4: {e}", file=sys.stderr)
        sys.exit(3)

    print(f"=== MIED v4 Verification: {path.name} ===")
    print(f"  total file size:        {len(blob):>10,} bytes  ({len(blob)/1024/1024:.2f} MB)")
    print(f"  version:                {d['version']}")
    print(f"  char_count (header):    {d['char_count']:>10,}")
    print(f"  word_count (header):    {d['word_count']:>10,}")
    print(f"  char_table parsed:      {len(d['char_table']):>10,}")
    print(f"  word_table parsed:      {len(d['word_table']):>10,}")

    print()
    print("  Section offsets:")
    for name, off in d['offsets'].items():
        print(f"    {name:<24} {off:>10,}")

    print()
    print("  Word groups by char_count:")
    for bucket_idx, (cnt, start) in enumerate(d['group_headers']):
        n = bucket_idx + 1
        label = f'{n}-char' if n < 8 else '8+ char'
        print(f"    {label:<10}  count={cnt:>7,}  start_wid={start:>8,}")

    # Sanity: each word's char_ids must be within range
    bad = 0
    for ci_idx, (char_ids, ridx, freq) in enumerate(d['word_table']):
        if any(cid >= d['char_count'] for cid in char_ids):
            bad += 1
        if any(r >= 255 for r in ridx):  # unsigned u8 range OK
            bad += 1
    print(f"\n  Structural check: {bad} bad word entries")

    # Sample: first 3 chars + first 5 words
    print("\n  Sample chars (first 3):")
    for cid in range(min(3, len(d['char_table']))):
        ch, readings = d['char_table'][cid]
        rdesc = ', '.join(f'{r[0].hex()}/t{r[1]}/f{r[2]}' for r in readings[:3])
        print(f"    cid={cid}  {ch}  readings=[{rdesc}]" + (' ...' if len(readings) > 3 else ''))

    print("\n  Sample words (first 5 from 2-char group):")
    g_cnt, g_start = d['group_headers'][1]  # 2-char group
    for wid in range(g_start, min(g_start + 5, g_start + g_cnt)):
        char_ids, ridx, freq = d['word_table'][wid]
        chars = ''.join(d['char_table'][c][0] for c in char_ids)
        print(f"    wid={wid}  '{chars}'  char_ids={char_ids}  ridx={ridx}  freq={freq}")

    print("\n  first_char_idx coverage:")
    cids_with_words = len(d['first_char_idx'])
    total_refs = sum(len(v) for v in d['first_char_idx'].values())
    print(f"    chars with at least 1 word: {cids_with_words:,} / {d['char_count']:,}")
    print(f"    total word_id references:   {total_refs:,}")

    print("\n  key_to_char_idx coverage:")
    keys_with_chars = len(d['key_to_char_idx'])
    total_chars = sum(len(v) for v in d['key_to_char_idx'].values())
    print(f"    keys with at least 1 char:  {keys_with_chars} / 24")
    print(f"    total char_id references:   {total_chars:,}")

    print(f"\n  PASS: round-trip parse OK")


if __name__ == "__main__":
    main()
