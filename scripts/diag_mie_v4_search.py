#!/usr/bin/env python3
"""Simulate CompositionSearcher::search() for a given user keyseq + target
char_count against MIED v4 dict. Pure Python reference mirroring the C++
logic in firmware/mie/src/composition_searcher.cpp so we can diagnose
why a word doesn't surface without hardware RTT."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from verify_mied_v4 import read_v4  # type: ignore

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass

K_MAX_CANDS = 50  # ImeLogic::kMaxCandidates

def composition_recurse(word_chars, ridxs, char_idx, key_idx, user_keys, char_table):
    if key_idx >= len(user_keys):
        return True
    if char_idx >= len(word_chars):
        return False
    cid = word_chars[char_idx]
    ridx = ridxs[char_idx]
    if cid >= len(char_table):
        return False
    readings = char_table[cid][1]
    if ridx >= len(readings):
        return False
    keyseq, tone, freq = readings[ridx]
    klen = len(keyseq)
    remaining = len(user_keys) - key_idx

    tried = []
    for p in [klen, klen - 1, 2, 1]:
        if 1 <= p <= klen and p not in tried:
            tried.append(p)

    for plen in tried:
        if plen > remaining:
            if bytes(keyseq[:remaining]) == bytes(user_keys[key_idx:key_idx+remaining]):
                return True
        elif bytes(keyseq[:plen]) == bytes(user_keys[key_idx:key_idx+plen]):
            consumed = plen
            if (plen == klen and tone == 1 and
                key_idx + plen < len(user_keys) and
                user_keys[key_idx + plen] == 0x20):
                consumed += 1
            if composition_recurse(word_chars, ridxs, char_idx + 1,
                                    key_idx + consumed, user_keys, char_table):
                return True
    return False

def main():
    if len(sys.argv) < 4:
        print("Usage: diag_mie_v4_search.py <user_keys_hex> <target_cc> "
              "<dict_path> [trace_word]", file=sys.stderr)
        print("  user_keys_hex: e.g. 27233323 for ㄍㄓㄩㄓ", file=sys.stderr)
        sys.exit(1)

    user_keys_hex = sys.argv[1]
    target_cc = int(sys.argv[2])
    dict_path = Path(sys.argv[3])
    trace_word = sys.argv[4] if len(sys.argv) > 4 else None

    user_keys = bytes.fromhex(user_keys_hex)
    blob = dict_path.read_bytes()
    d = read_v4(blob)
    char_table = d['char_table']
    word_table = d['word_table']
    first_char_idx = d['first_char_idx']
    key_to_char_idx = d['key_to_char_idx']
    group_headers = d['group_headers']

    print(f"=== Simulated search: user_keys={user_keys.hex()} target={target_cc} ===")

    if target_cc == 0:
        allowed = (0, len(word_table))
    elif 1 <= target_cc <= 7:
        cnt, start = group_headers[target_cc - 1]
        allowed = (start, start + cnt)
    elif target_cc == -1:
        _, start5 = group_headers[4]
        cnt8, start8 = group_headers[7]
        allowed = (start5, start8 + cnt8)
    else:
        print(f"bad target_cc {target_cc}")
        return
    print(f"  bucket allowed wid range: [{allowed[0]}, {allowed[1]}) "
          f"= {allowed[1] - allowed[0]} words")

    # Find the candidate first-chars whose reading[0] starts with user_keys[0]
    fb = user_keys[0]
    cand_cids = key_to_char_idx.get(fb, [])
    print(f"  key_to_char_idx[0x{fb:02X}]: {len(cand_cids)} candidate first-chars")

    # Target word lookup if trace enabled
    target_word_cids = None
    target_wid = None
    if trace_word:
        ch_to_cid = {ch: cid for cid, (ch, _) in enumerate(char_table)}
        target_word_cids = [ch_to_cid[c] for c in trace_word if c in ch_to_cid]
        for wid, (cids, ridxs, freq) in enumerate(word_table):
            if cids == target_word_cids:
                target_wid = wid
                break
        print(f"  trace_word {trace_word!r}: cids={target_word_cids} wid={target_wid}")
        if target_wid is not None:
            in_bucket = allowed[0] <= target_wid < allowed[1]
            first_cid_visited = target_word_cids[0] in cand_cids
            print(f"    in bucket? {in_bucket}  first-cid in key_to_char_idx? "
                  f"{first_cid_visited}")

    # Simulate the search, collecting matches freq-desc.
    collected = []
    trace_visited = False
    for cid in cand_cids:
        wids = first_char_idx.get(cid, [])
        for wid in wids:
            if not (allowed[0] <= wid < allowed[1]):
                continue
            if target_wid is not None and wid == target_wid:
                trace_visited = True
                cids_w, ridxs_w, freq_w = word_table[wid]
                match = composition_recurse(cids_w, ridxs_w, 0, 0,
                                             user_keys, char_table)
                print(f"    TRACE: wid={wid} freq={freq_w} "
                      f"composition_matches={match}")
                # Continue normal flow
            cids_w, ridxs_w, freq_w = word_table[wid]
            if composition_recurse(cids_w, ridxs_w, 0, 0,
                                    user_keys, char_table):
                # Render word str
                w_str = ''.join(char_table[c][0] for c in cids_w)
                collected.append((w_str, freq_w, wid))
                if len(collected) >= K_MAX_CANDS:
                    break
        if len(collected) >= K_MAX_CANDS:
            break

    if trace_word:
        print(f"  TRACE visited? {trace_visited}")

    # Sort as search() does at the end
    collected.sort(key=lambda x: -x[1])
    print(f"\n  matches found: {len(collected)} (capped at {K_MAX_CANDS})")
    for i, (w, freq, wid) in enumerate(collected):
        marker = "  <-- target" if (target_wid is not None and wid == target_wid) else ""
        print(f"    [{i:3d}] {w:<12s}  freq={freq:>6d}  wid={wid}{marker}")

if __name__ == '__main__':
    main()
