#!/usr/bin/env python3
"""estimate_rare_savings.py — Quantify the dict footprint of chars whose
every reading is unreachable (rank > 100 in its half-keyboard bucket).

Two scenarios:
  1. "Drop reading" — keep the char alive (multi-reading chars stay
     reachable via their other readings; word_table char_ids remain
     valid) but remove just the unreachable readings from char_table.
  2. "Drop char" — remove chars whose EVERY reading is unreachable
     (and consequently every word in word_table that references such
     a char_id).
"""
import sys
from pathlib import Path
from collections import defaultdict

sys.path.insert(0, str(Path(__file__).parent))
from verify_mied_v4 import read_v4

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass


def reading_size(reading, has_pp):
    # klen(1) + kbytes(klen) + (phoneme_pos byte if pp) + tone(1) + freq(2)
    klen = len(reading[0])
    return 1 + klen + (1 if has_pp else 0) + 1 + 2


def char_record_size(ch_utf8, readings, has_pp):
    # utf8_len(1) + utf8 bytes + reading_count(1) + sum(reading_size)
    return 1 + len(ch_utf8) + 1 + sum(reading_size(r, has_pp) for r in readings)


def main():
    blob = Path('firmware/mie/data/dict_mie_v4.bin').read_bytes()
    d = read_v4(blob)
    has_pp = d.get('has_phoneme_pos', False)

    # Build buckets keyed by (kbytes, tone) → list of (char_id, reading_idx, freq)
    buckets = defaultdict(list)
    for cid, (ch, readings) in enumerate(d['char_table']):
        for ridx, r in enumerate(readings):
            kb, _pos, tone, freq = r
            buckets[(kb, tone)].append((cid, ridx, freq))

    # For each bucket, sort by freq desc and tag readings beyond rank 100
    # as "unreachable".
    unreachable = set()  # set of (cid, ridx)
    for entries in buckets.values():
        entries.sort(key=lambda x: -x[2])
        for rank, (cid, ridx, _f) in enumerate(entries):
            if rank >= 100:
                unreachable.add((cid, ridx))

    n_unreach_readings = len(unreachable)
    total_readings = sum(len(rs) for _, rs in d['char_table'])
    print(f'Total readings:       {total_readings:>10,}')
    print(f'Unreachable readings: {n_unreach_readings:>10,}  '
          f'({100*n_unreach_readings/total_readings:.1f}%)')

    # Count chars whose EVERY reading is unreachable.
    chars_all_unreach = []
    for cid, (ch, rs) in enumerate(d['char_table']):
        if all((cid, ridx) in unreachable for ridx in range(len(rs))):
            chars_all_unreach.append(cid)
    n_dead_chars = len(chars_all_unreach)
    print(f'Chars with no reachable reading: '
          f'{n_dead_chars:,} / {len(d["char_table"]):,}  '
          f'({100*n_dead_chars/len(d["char_table"]):.1f}%)')
    print()

    # ── Scenario 1: drop unreachable readings only (chars stay) ────────
    bytes_saved_readings = 0
    for (cid, ridx) in unreachable:
        r = d['char_table'][cid][1][ridx]
        bytes_saved_readings += reading_size(r, has_pp)

    print(f'== Scenario 1: drop {n_unreach_readings:,} unreachable readings only ==')
    print(f'   char_table bytes saved:        {bytes_saved_readings:>10,}')
    print()

    # ── Scenario 2: drop all chars whose every reading is unreachable ──
    # Include their char_table records, char_offsets entries (4 B each),
    # key_to_char_idx entries (2 B each, possibly multiple per char if
    # readings span different first-bytes), first_char_idx entries (4 B
    # per word reference), and any words that referenced them.
    dead_set = set(chars_all_unreach)

    # char_table: full record for each dead char
    bytes_chartab = sum(
        char_record_size(d['char_table'][cid][0].encode('utf-8'),
                         d['char_table'][cid][1], has_pp)
        for cid in dead_set)

    # char_offsets: 4 B per char_id (we'd reduce char_count)
    bytes_choff = 4 * n_dead_chars

    # key_to_char_idx: each char contributes one u16 entry per unique
    # first-byte across its readings. Count.
    key_idx_entries = 0
    for cid in dead_set:
        firsts = set()
        for r in d['char_table'][cid][1]:
            kb = r[0]
            if kb: firsts.add(kb[0])
        key_idx_entries += len(firsts)
    bytes_keyidx = 2 * key_idx_entries

    # word_table + first_char_idx + word_offsets: count words that would
    # be removed because they reference at least one dead char_id.
    dead_word_ids = []
    for wid, (char_ids, ridx, freq) in enumerate(d['word_table']):
        if any(cid in dead_set for cid in char_ids):
            dead_word_ids.append(wid)

    # word_table per-record size: 1 (n) + 1 (flags) + 2 (freq) + 2*n
    # (+ n if reading_idxs present)
    bytes_wordtab = 0
    for wid in dead_word_ids:
        char_ids, ridxs, _f = d['word_table'][wid]
        n = len(char_ids)
        has_overrides = any(r != 0 for r in ridxs)
        bytes_wordtab += 1 + 1 + 2 + 2 * n + (n if has_overrides else 0)

    # first_char_idx: 4 B per word_id reference (each dead word adds one
    # ref under its first char_id).
    bytes_firstidx = 4 * len(dead_word_ids)
    # word_offsets: 4 B per word
    bytes_wordoff = 4 * len(dead_word_ids)

    total_s2 = (bytes_chartab + bytes_choff + bytes_keyidx
                + bytes_wordtab + bytes_firstidx + bytes_wordoff)

    print(f'== Scenario 2: drop {n_dead_chars:,} chars whose every reading is unreachable ==')
    print(f'   chars dropped:                 {n_dead_chars:>10,}')
    print(f'   words dropped (cascade):       {len(dead_word_ids):>10,}  '
          f'({100*len(dead_word_ids)/len(d["word_table"]):.1f}% of word_table)')
    print(f'   char_table bytes saved:        {bytes_chartab:>10,}')
    print(f'   char_offsets bytes saved:      {bytes_choff:>10,}')
    print(f'   key_to_char_idx bytes saved:   {bytes_keyidx:>10,}')
    print(f'   word_table bytes saved:        {bytes_wordtab:>10,}')
    print(f'   word_offsets bytes saved:      {bytes_wordoff:>10,}')
    print(f'   first_char_idx bytes saved:    {bytes_firstidx:>10,}')
    print(f'   ─── total saved:               {total_s2:>10,}  '
          f'({100*total_s2/len(blob):.1f}% of {len(blob):,} dict)')
    print()

    # Extra info: what fraction of CJK chars are these dead chars from?
    # Show a sample.
    print(f'   Sample of 20 dropped chars (lowest-freq readings ones):')
    sample = sorted(dead_set, key=lambda cid: max(
        r[3] for r in d['char_table'][cid][1]))[:20]
    print(f'   {"".join(d["char_table"][cid][0] for cid in sample)}')


if __name__ == '__main__':
    main()
