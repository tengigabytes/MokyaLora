#!/usr/bin/env python3
"""find_rare_chars.py — Mine the v4 dict for chars that would have been
beyond rank 100 in v2 (half-keyboard, no phoneme-position filter) but
should become reachable under Phase 1.4 long-press disambiguation.

For each char, look at the bucket of "all chars that share the same
key-byte sequence + tone" (the v2 search universe for that input). Sort
by freq desc — that's the order the v2 engine returned them in. Any
char at rank > 100 was unreachable even with the 100-cap scroll.

Phase 1.4 prediction: applying the per-byte phoneme-position filter
removes from the bucket every char whose authored phoneme positions
don't match the target. Chars whose phoneme positions are NOT all
primary tend to benefit most — the filter prunes the (typically
larger) primary-position competitor pool.

Output: a passage file with one char per group selected so we cover
diverse buckets.
"""
import sys
import struct
from pathlib import Path
from collections import defaultdict

sys.path.insert(0, str(Path(__file__).parent))
from verify_mied_v4 import read_v4

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass


def main():
    blob = Path('firmware/mie/data/dict_mie_v4.bin').read_bytes()
    d = read_v4(blob)
    has_pp = d.get('has_phoneme_pos', False)
    if not has_pp:
        print('ERROR: dict has no phoneme_pos — rebuild with Phase 1.4',
              file=sys.stderr)
        sys.exit(1)

    # bucket key: (kbytes, tone)  -> list of (char, pos, freq, char_id)
    buckets = defaultdict(list)
    for cid, (ch, readings) in enumerate(d['char_table']):
        for (kb, pos, tone, freq) in readings:
            buckets[(kb, tone)].append((ch, pos, freq, cid))

    # For each bucket, sort by freq desc and tag rank.
    # Look for chars at rank > 100 whose phoneme positions are NOT all 0
    # (i.e. they would benefit from long-press filtering — typing them
    # with proper hints prunes most of the higher-freq primary-position
    # competitors).
    candidates = []   # (char, bucket_key, rank, pos, freq, bucket_size)
    for key, entries in buckets.items():
        if len(entries) <= 100:
            continue   # whole bucket fits in v2 cap, nothing to recover
        entries.sort(key=lambda r: -r[2])
        for rank, (ch, pos, freq, cid) in enumerate(entries):
            if rank < 100:
                continue
            # Skip chars where every phoneme is primary — long-press
            # wouldn't help (filter keeps everyone with all-primary
            # readings, which is the dominant slice of the bucket).
            if all(p == 0 for p in pos):
                continue
            candidates.append((ch, key, rank, pos, freq, len(entries)))

    # Group candidates by bucket so we can show diversity.
    by_bucket = defaultdict(list)
    for c in candidates:
        by_bucket[c[1]].append(c)

    print(f'== Buckets with >100 chars: {sum(1 for k,e in buckets.items() if len(e)>100)}')
    print(f'== Bucket sizes top-15:')
    biggest = sorted(buckets.items(), key=lambda x: -len(x[1]))[:15]
    for key, entries in biggest:
        kb_hex = ''.join(f'{b:02x}' for b in key[0])
        print(f'   bucket kbytes={kb_hex} tone={key[1]} size={len(entries)}')

    print(f'\n== Total chars at rank > 100 with non-all-primary pos: {len(candidates)}')

    # Pick 30 candidates spanning three rank tiers (100, 150, 200) with
    # 10 chars each. Different ranks stress the hint filter at different
    # depths: chars near the cap may slot back into top-100 after the
    # ~50 % bucket reduction; chars at rank 200 likely won't unless their
    # hint pattern is rare enough to prune a large competitor pool.
    target_ranks = [100, 150, 200]
    per_tier     = 10
    picked = []
    seen_chars = set()

    biggest = sorted(by_bucket.items(), key=lambda x: -len(buckets[x[0]]))
    for target in target_ranks:
        n_added = 0
        for bkey, entries in biggest:
            if n_added >= per_tier: break
            # Find the char with rank closest to target (above only).
            best = None
            for c in entries:
                if c[2] < target: continue
                if c[0] in seen_chars: continue
                if best is None or c[2] < best[2]:
                    best = c
            if best is None: continue
            picked.append(best)
            seen_chars.add(best[0])
            n_added += 1

    print(f'\n== Selected {len(picked)} test chars:\n')
    print(f"{'char':<5} {'rank':>5}  {'bucket':>7}  {'pos':<10} kbytes      tone")
    print('-' * 60)
    for ch, key, rank, pos, freq, bsize in picked:
        kb_hex = ' '.join(f'{b:02x}' for b in key[0])
        pos_str = ''.join(str(p) for p in pos)
        print(f"{ch:<5} {rank:>5}  {bsize:>7}  {pos_str:<10} [{kb_hex}]  t{key[1]}")

    # Write passage file (chars only, one per line for readability).
    out = Path('scripts/ime_passage_rare.txt')
    out.write_text(''.join(c[0] for c in picked) + '\n', encoding='utf-8')
    print(f'\n== Wrote {out} ({len(picked)} chars)')


if __name__ == '__main__':
    main()
