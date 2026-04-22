#!/usr/bin/env python3
"""Look up a word in the MIED v4 dict and report all related state."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from verify_mied_v4 import read_v4  # type: ignore

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass

def main():
    if len(sys.argv) < 2:
        print("Usage: dict_find_word.py <word> [dict_path]", file=sys.stderr)
        sys.exit(1)
    word = sys.argv[1]
    path = Path(sys.argv[2] if len(sys.argv) > 2
                else 'firmware/mie/data/dict_mie_v4.bin')
    blob = path.read_bytes()
    d = read_v4(blob)
    char_table = d['char_table']
    word_table = d['word_table']

    ch_to_cid = {ch: cid for cid, (ch, _) in enumerate(char_table)}
    missing = [ch for ch in word if ch not in ch_to_cid]
    print(f"=== Looking up {word!r} in {path.name} ===")
    print(f"  chars:")
    for ch in word:
        if ch not in ch_to_cid:
            print(f"    {ch}: NOT in char_table")
            continue
        cid = ch_to_cid[ch]
        readings = char_table[cid][1]
        rdesc = "; ".join(f"{r[0].hex()}/t{r[1]}/f{r[2]}" for r in readings)
        print(f"    {ch}: cid={cid}  readings=[{rdesc}]")
    if missing:
        print(f"  -> missing chars: {missing}")
        return

    target_cids = [ch_to_cid[ch] for ch in word]

    # Look for word entry
    hits = []
    for wid, (char_ids, ridxs, freq) in enumerate(word_table):
        if char_ids == target_cids:
            hits.append((wid, ridxs, freq))
    print(f"\n  word_table matches: {len(hits)}")
    for wid, ridxs, freq in hits:
        print(f"    wid={wid}  ridx={ridxs}  freq={freq}")
        for i, (cid, r) in enumerate(zip(target_cids, ridxs)):
            ch = char_table[cid][0]
            readings = char_table[cid][1]
            if r < len(readings):
                rb, tone, f = readings[r]
                print(f"      [{i}] {ch} reading[{r}] = {rb.hex()}/t{tone}/f{f}")

if __name__ == '__main__':
    main()
