#!/usr/bin/env python3
"""Inspect MIED dict_dat.bin + dict_values.bin to quantify the word
duplication that current TrieSearcher::search dedup compensates for.

Outputs:
  - total entries / unique words
  - word duplication histogram (how many keys reference each word)
  - byte breakdown: header overhead vs word string bytes
  - projected size if values used word_id indirection (proposed v3 format)
  - top-K most duplicated words

Format reference (per gen_dict.py header + trie_searcher.cpp):

dict_dat.bin
  Header  16 B  magic="MIED" (4) | version u16 | flags u16 | key_count u32
                | keys_data_off u32
  Index   key_count * 8 B  (key_data_off u32, val_data_off u32)
  Keys    variable  (key_len u8, key_bytes...)  — sorted ascending

dict_values.bin (v2)
  ValueRecord  2 B word_count
              followed by word_count * (freq u16, tone u8, word_len u8, word_utf8)
"""

import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path


def parse_header(buf):
    if buf[:4] != b"MIED":
        raise ValueError("dict_dat.bin: bad magic")
    version, flags, key_count, keys_data_off = struct.unpack_from(
        "<HHII", buf, 4)
    return version, flags, key_count, keys_data_off


def main(dat_path, val_path):
    dat = Path(dat_path).read_bytes()
    val = Path(val_path).read_bytes()

    version, flags, key_count, keys_data_off = parse_header(dat)
    print(f"dict_dat.bin = {len(dat):>10,} bytes  ({len(dat)/1024:.1f} KB)")
    print(f"dict_values.bin = {len(val):>10,} bytes  ({len(val)/1024:.1f} KB)")
    print(f"  version={version}  flags={flags}  key_count={key_count:,}  "
          f"keys_data_off={keys_data_off}")
    print()

    has_tone  = (version == 2)
    hdr_bytes = 4 if has_tone else 3

    # Walk every (key, value_record) pair.
    word_counter   = Counter()              # word_bytes -> total occurrences
    word_first_key = {}                     # word_bytes -> first key it appeared under
    word_total_bytes_in_values = 0          # sum of all word_len bytes across all entries
    header_total_bytes_in_values = 0        # sum of all per-record header bytes
    record_count_total = 0                  # sum of word_count across all keys
    keys_with_no_records = 0
    sample_dup_keys = []                    # for the most duplicated word

    for i in range(key_count):
        entry_off = 16 + i * 8
        key_off, val_off = struct.unpack_from("<II", dat, entry_off)

        # Decode the key bytes (sanity / used for sample_dup_keys)
        kpos_len = keys_data_off + key_off
        klen = dat[kpos_len]
        key_bytes = bytes(dat[kpos_len + 1 : kpos_len + 1 + klen])

        # Decode the value record at val_off in dict_values.bin
        if val_off + 2 > len(val):
            keys_with_no_records += 1
            continue
        word_count, = struct.unpack_from("<H", val, val_off)
        vo = val_off + 2
        header_total_bytes_in_values += 2  # the word_count u16 itself

        for w in range(word_count):
            if vo + hdr_bytes > len(val):
                break
            freq = struct.unpack_from("<H", val, vo)[0]
            tone = val[vo + 2] if has_tone else 0
            wlen = val[vo + (3 if has_tone else 2)]
            vo += hdr_bytes
            if wlen == 0 or vo + wlen > len(val):
                continue
            word_bytes = bytes(val[vo : vo + wlen])
            vo += wlen

            word_counter[word_bytes] += 1
            if word_bytes not in word_first_key:
                word_first_key[word_bytes] = key_bytes
            word_total_bytes_in_values  += wlen
            header_total_bytes_in_values += hdr_bytes
            record_count_total += 1

    print("--- raw counts ---")
    print(f"  total value records (key, word) pairs:  {record_count_total:>10,}")
    print(f"  unique word strings:                     {len(word_counter):>10,}")
    print(f"  duplication factor (records / unique):   "
          f"{record_count_total/max(1,len(word_counter)):>10.2f}×")
    if keys_with_no_records:
        print(f"  keys with no value records (skipped):    {keys_with_no_records}")
    print()

    print("--- byte breakdown of dict_values.bin ---")
    overhead = header_total_bytes_in_values
    other = len(val) - word_total_bytes_in_values - overhead
    print(f"  word UTF-8 bytes (the duplicated stuff): "
          f"{word_total_bytes_in_values:>10,}  "
          f"({100*word_total_bytes_in_values/len(val):.1f} %)")
    print(f"  per-record headers (freq/tone/wlen/cnt):  "
          f"{overhead:>10,}  ({100*overhead/len(val):.1f} %)")
    print(f"  unaccounted (alignment / format slack):   "
          f"{other:>10,}  ({100*other/len(val):.1f} %)")
    print()

    print("--- word duplication histogram ---")
    dup_hist = Counter()
    for w, c in word_counter.items():
        # Bucket: 1, 2-4, 5-9, 10-19, 20-49, 50-99, 100-199, 200-499, 500+
        if   c == 1:   bucket = "1"
        elif c < 5:    bucket = "2-4"
        elif c < 10:   bucket = "5-9"
        elif c < 20:   bucket = "10-19"
        elif c < 50:   bucket = "20-49"
        elif c < 100:  bucket = "50-99"
        elif c < 200:  bucket = "100-199"
        elif c < 500:  bucket = "200-499"
        else:          bucket = "500+"
        dup_hist[bucket] += 1
    order = ["1","2-4","5-9","10-19","20-49","50-99","100-199","200-499","500+"]
    for b in order:
        n = dup_hist.get(b, 0)
        if n:
            print(f"  duplicated {b:>9}× : {n:>6,} unique words "
                  f"({100*n/len(word_counter):.1f}%)")
    print()

    print("--- top 15 most-duplicated words ---")
    for w, c in word_counter.most_common(15):
        try:
            text = w.decode("utf-8")
        except UnicodeDecodeError:
            text = repr(w)
        print(f"  {c:>5,}×  {text}")
    print()

    # ── Projection: v3 word-id indirection ─────────────────────────────────
    print("--- projection: v3 word-id indirection format ---")
    # Proposed v3 format:
    #   word_table: each unique word stored once as (word_len u8, word_bytes).
    #   value record: word_count u16, then word_count * (freq u16, tone u8,
    #                 word_id u24)  — 6 bytes per per-key word reference.
    word_table_bytes = sum(1 + len(w) for w in word_counter.keys())
    v3_per_word_ref  = 2 + 1 + 3   # freq u16 + tone u8 + word_id u24
    v3_records_bytes = record_count_total * v3_per_word_ref
    v3_word_count_hdrs = key_count * 2  # the word_count u16 per key (kept)
    v3_values_bytes    = word_table_bytes + v3_records_bytes + v3_word_count_hdrs

    print(f"  v3 word table (unique words once):       "
          f"{word_table_bytes:>10,}  ({word_table_bytes/1024:.1f} KB)")
    print(f"  v3 value records (key→word_id refs):     "
          f"{v3_records_bytes:>10,}  ({v3_records_bytes/1024:.1f} KB)")
    print(f"  v3 word_count headers (one u16 per key): "
          f"{v3_word_count_hdrs:>10,}  ({v3_word_count_hdrs/1024:.1f} KB)")
    print(f"  v3 dict_values.bin total (estimated):    "
          f"{v3_values_bytes:>10,}  ({v3_values_bytes/1024:.1f} KB)")
    saved = len(val) - v3_values_bytes
    print(f"  vs current v2 ({len(val):,} B): "
          f"{'+' if saved < 0 else '-'}{abs(saved):>10,} bytes  "
          f"({100*saved/len(val):+.1f} %)")
    print()

    # Worst-case 1-byte prefix dedup work:
    print("--- worst-case 1-byte prefix dedup load ---")
    # Find which 1-byte key-prefix has the most word records.
    by_prefix = defaultdict(lambda: [0, 0])  # prefix_byte -> [n_keys, n_words]
    for i in range(key_count):
        entry_off = 16 + i * 8
        key_off, val_off = struct.unpack_from("<II", dat, entry_off)
        kpos_len = keys_data_off + key_off
        klen = dat[kpos_len]
        if klen == 0: continue
        first = dat[kpos_len + 1]
        by_prefix[first][0] += 1
        if val_off + 2 <= len(val):
            wc, = struct.unpack_from("<H", val, val_off)
            by_prefix[first][1] += wc
    rows = sorted(by_prefix.items(), key=lambda kv: -kv[1][1])[:10]
    print(f"  {'prefix_byte':>11} {'(slot)':>7}  {'keys':>8}  "
          f"{'word_recs':>10}  {'v2 dedup_cmp@50':>16}  {'v3 dedup_cmp@50':>16}")
    for first, (n_keys, n_words) in rows:
        slot = first - 0x21
        v2_cmp = n_words * 50      # current O(words × max_results)
        v3_cmp = 0                 # bitset = O(1) per word
        print(f"  0x{first:02x}={chr(first)!r:>5}  {slot:>5}    "
              f"{n_keys:>8,}  {n_words:>10,}  {v2_cmp:>16,}  {v3_cmp:>16}")


if __name__ == "__main__":
    dat_path = sys.argv[1] if len(sys.argv) > 1 else "firmware/mie/data/dict_dat.bin"
    val_path = sys.argv[2] if len(sys.argv) > 2 else "firmware/mie/data/dict_values.bin"
    main(dat_path, val_path)
