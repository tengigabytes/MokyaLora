#!/usr/bin/env python3
"""Diagnose why typing 'ㄎㄨㄞㄌ' does not surface '快樂' in MIED v4."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from verify_mied_v4 import read_v4  # type: ignore

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass

# ㄎ=11, ㄨ=13, ㄞ=4, ㄌ=15  (slot + 0x21)
USER_KEYS = bytes([11 + 0x21, 13 + 0x21, 4 + 0x21, 15 + 0x21])
TARGET_WORD = "快樂"

def composition_recurse(word_chars, char_idx, key_idx, user_keys,
                        char_table, reading_idxs):
    if key_idx == len(user_keys):
        return True
    if char_idx >= len(word_chars):
        return False
    cid = word_chars[char_idx]
    ridx = reading_idxs[char_idx]
    ch, readings = char_table[cid]
    if ridx >= len(readings):
        return False
    keyseq, tone, freq = readings[ridx]
    remaining = len(user_keys) - key_idx
    # Try prefix lengths: full, 2, 1 (in that order, descending)
    for plen in (len(keyseq), 2, 1):
        if plen == 0 or plen > len(keyseq):
            continue
        if plen > remaining:
            continue
        if bytes(keyseq[:plen]) == user_keys[key_idx:key_idx+plen]:
            if composition_recurse(word_chars, char_idx+1, key_idx+plen,
                                   user_keys, char_table, reading_idxs):
                return True
    return False

def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1
                else 'firmware/mie/data/dict_mie_v4.bin')
    blob = path.read_bytes()
    d = read_v4(blob)
    char_table = d['char_table']
    word_table = d['word_table']
    first_char_idx = d['first_char_idx']
    key_to_char_idx = d['key_to_char_idx']

    ch_to_cid = {ch: cid for cid, (ch, _) in enumerate(char_table)}

    print(f"=== Diagnosing {TARGET_WORD!r} against {path.name} ===")
    print(f"  user_keys = {USER_KEYS.hex()} (ㄎㄨㄞㄌ)")

    # Step 1: are 快 and 樂 in char_table?
    for ch in TARGET_WORD:
        if ch not in ch_to_cid:
            print(f"  MISS: char {ch!r} not in char_table")
            return
        cid = ch_to_cid[ch]
        readings = char_table[cid][1]
        print(f"  cid[{ch}] = {cid}  readings: "
              + "; ".join(f"{r[0].hex()}/t{r[1]}/f{r[2]}"
                          for r in readings))

    kuai_cid = ch_to_cid['快']
    le_cid = ch_to_cid['樂']

    # Step 2: is 快 in key_to_char_idx[ㄎ]?
    k_byte = 11 + 0x21
    k_list = key_to_char_idx.get(k_byte, [])
    print(f"\n  key_to_char_idx[0x{k_byte:02X}] has {len(k_list)} cids; "
          f"快 present? {kuai_cid in k_list}")
    if kuai_cid not in k_list:
        print("    -> 快's primary reading does not start with ㄎ; "
              "search_words won't visit it.")

    # Step 3: is 快樂 in word_table?
    found_wids = []
    for wid, (char_ids, ridxs, freq) in enumerate(word_table):
        if char_ids == [kuai_cid, le_cid]:
            found_wids.append((wid, ridxs, freq))
    print(f"\n  word 快樂 entries: {len(found_wids)}")
    for wid, ridxs, freq in found_wids:
        print(f"    wid={wid}  ridx={ridxs}  freq={freq}")
        readings_kuai = char_table[kuai_cid][1]
        readings_le = char_table[le_cid][1]
        if ridxs[0] < len(readings_kuai):
            r = readings_kuai[ridxs[0]]
            print(f"      快 reading[{ridxs[0]}] = {r[0].hex()}/t{r[1]}")
        if ridxs[1] < len(readings_le):
            r = readings_le[ridxs[1]]
            print(f"      樂 reading[{ridxs[1]}] = {r[0].hex()}/t{r[1]}")

    # Step 4: first_char_idx[kuai_cid]
    fc = first_char_idx.get(kuai_cid, [])
    print(f"\n  first_char_idx[快({kuai_cid})] has {len(fc)} words")
    found_in_fc = [(w, word_table[w][1], word_table[w][2])
                   for w in fc if word_table[w][0] == [kuai_cid, le_cid]]
    print(f"    快樂 present in first_char_idx[快]? {len(found_in_fc) > 0}")

    # Step 5: simulate composition match for each 快樂 word entry
    print(f"\n  Composition match simulation:")
    for wid, ridxs, freq in found_wids:
        ok = composition_recurse([kuai_cid, le_cid], 0, 0,
                                 USER_KEYS, char_table, ridxs)
        print(f"    wid={wid}  ridx={ridxs}  matches user_keys? {ok}")

    # Step 6: walk key_to_char_idx[ㄎ] and for each 快-cid, show
    # whether word lookup would find 快樂 (need 快's cid in the bucket).
    if found_wids and kuai_cid not in k_list:
        # What reading does 快's reading[0] actually have?
        r0 = char_table[kuai_cid][1][0]
        print(f"\n  BUG ROOT: 快.readings[0] = {r0[0].hex()} starts with "
              f"0x{r0[0][0]:02X} not 0x{k_byte:02X}")
        print("    Search's outer walk via key_to_char_idx skips 快.")


if __name__ == '__main__':
    main()
