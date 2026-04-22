#!/usr/bin/env python3
"""
PoC: MIE v4 Composition Architecture simulation.

Parses the current MIED v2 dict, simulates v4's char_table + word_table
+ first_char_index layout, measures actual byte sizes, and profiles a
reference composition_matches implementation in pure Python.

Go/No-Go gate for Phase 1:
  - Simulated dict_mie.bin total <= 2.5 MB
  - Reference search p50 (Python) <= 5 ms  (C++ expected ~10x faster -> <500 us)

Usage: python scripts/poc_mie_v4_simulation.py
"""

import random
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path


# -- MIED v2 parser ----------------------------------------------------------─

def parse_v2_dict(dat_path: Path, val_path: Path):
    """Parse MIED v2. Returns list of (key_bytes, word_bytes, freq, tone)."""
    dat = dat_path.read_bytes()
    val = val_path.read_bytes()
    if dat[:4] != b"MIED":
        raise ValueError(f"{dat_path}: bad magic")
    version, _flags, key_count, keys_data_off = struct.unpack_from("<HHII", dat, 4)
    if version != 2:
        raise ValueError(f"{dat_path}: expected v2, got v{version}")

    out = []
    for i in range(key_count):
        key_off, val_off = struct.unpack_from("<II", dat, 16 + i * 8)
        kpos = keys_data_off + key_off
        if kpos >= len(dat):
            continue
        klen = dat[kpos]
        key_bytes = bytes(dat[kpos + 1 : kpos + 1 + klen])

        if val_off + 2 > len(val):
            continue
        wc = struct.unpack_from("<H", val, val_off)[0]
        vo = val_off + 2
        for _ in range(wc):
            if vo + 4 > len(val):
                break
            freq = struct.unpack_from("<H", val, vo)[0]
            tone = val[vo + 2]
            wlen = val[vo + 3]
            vo += 4
            if vo + wlen > len(val):
                break
            word_bytes = bytes(val[vo : vo + wlen])
            vo += wlen
            out.append((key_bytes, word_bytes, freq, tone))
    return out


# -- Reading derivation ------------------------------------------------------─

def cjk_codepoint(text: str) -> bool:
    if len(text) != 1:
        return False
    cp = ord(text)
    return 0x3400 <= cp <= 0x9FFF or 0x20000 <= cp <= 0x2FFFF


def is_cjk_char(ch: str) -> bool:
    cp = ord(ch)
    return 0x3400 <= cp <= 0x9FFF or 0x20000 <= cp <= 0x2FFFF


def derive_char_readings_from_1char_entries(records):
    """
    From entries whose word is exactly 1 CJK char, collect (key_bytes, tone,
    freq) tuples per char. Returns {char_utf8: [(key, tone, freq), ...]}.

    Readings are deduped by (key, tone) pair; freq is max across occurrences.
    """
    char_readings = defaultdict(dict)  # {char: {(key, tone): best_freq}}
    for key, word, freq, tone in records:
        try:
            text = word.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if len(text) != 1 or not is_cjk_char(text):
            continue
        bucket = char_readings[text]
        existing = bucket.get((key, tone), 0)
        if freq > existing:
            bucket[(key, tone)] = freq
    # Flatten
    flat = {
        ch: [(k, t, f) for (k, t), f in sorted(d.items(), key=lambda kv: -kv[1])]
        for ch, d in char_readings.items()
    }
    return flat


def derive_char_readings_from_phrases(records, existing_char_readings):
    """
    For chars that appear in phrases but lack a 1-char entry, infer the
    reading by pattern-matching against phrase keys. For PoC simplicity we
    skip this — production pipeline will use Unihan.

    Returns char_readings augmented with a best-effort reading for missing
    chars (currently marked with an empty reading list, meaning they will
    be excluded from word_table).
    """
    # For PoC: identify but don't infer. Report the gap.
    return existing_char_readings


def collect_all_cjk_chars_from_phrases(records):
    seen = set()
    for _key, word, _freq, _tone in records:
        try:
            text = word.decode("utf-8")
        except UnicodeDecodeError:
            continue
        for ch in text:
            if is_cjk_char(ch):
                seen.add(ch)
    return seen


# -- Word table construction --------------------------------------------------

def build_word_table(records, char_readings):
    """
    Build word entries: one per unique word whose chars are all present in
    char_readings. For PoC, reading_idx defaults to 0 (first reading).

    Returns (word_entries, char_to_id):
      word_entries: list of dicts {word, char_ids, reading_idxs, freq, tone}
      char_to_id:   {char_utf8: char_id}
    """
    char_to_id = {ch: idx for idx, ch in enumerate(sorted(char_readings.keys()))}

    # Dedup by word bytes; keep max freq
    best = {}
    for _key, word, freq, tone in records:
        try:
            text = word.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if len(text) < 2:
            continue  # only multi-char words for word_table
        if not all(is_cjk_char(c) for c in text):
            continue
        if not all(c in char_to_id for c in text):
            continue
        existing = best.get(word, (0, None))[0]
        if freq > existing:
            best[word] = (freq, tone, text)

    word_entries = []
    for word, (freq, tone, text) in best.items():
        char_ids = [char_to_id[c] for c in text]
        reading_idxs = [0] * len(char_ids)  # PoC default; real impl derives from keys
        word_entries.append({
            "word": word,
            "char_ids": char_ids,
            "reading_idxs": reading_idxs,
            "freq": freq,
            "tone": tone,
        })
    return word_entries, char_to_id


# -- Size accounting ----------------------------------------------------------

def size_header() -> int:
    return 0x30  # per MIED v4 spec


def size_char_table(char_readings) -> int:
    """Per char: utf8_len(1) + utf8_bytes(3) + reading_count(1) +
       per reading: key_len(1) + key_bytes(<=6) + tone(1) + base_freq(2)."""
    total = 0
    for ch, readings in char_readings.items():
        utf8 = ch.encode("utf-8")
        total += 1  # utf8_len
        total += len(utf8)
        total += 1  # reading_count
        for key, tone, freq in readings:
            total += 1  # key_len
            total += len(key)
            total += 1  # tone
            total += 2  # base_freq
    return total


def size_word_table(word_entries) -> int:
    """Per word: char_count(1) + flags(1) + freq(2) + char_id(2N) + reading_idx(N)."""
    total = 0
    # Group headers: 8 per group (count + start_id), 8 groups (char_count 1..8)
    total += 8 * 8
    for w in word_entries:
        n = len(w["char_ids"])
        total += 1  # char_count
        total += 1  # flags
        total += 2  # freq
        total += 2 * n  # char_id × N
        total += n   # reading_idx × N
    return total


def size_first_char_index(word_entries, char_count) -> int:
    """offsets[char_count+1] (u32 each) + flat[sum_of_lists] (u32 word_id)."""
    total = 4 * (char_count + 1)
    total += 4 * len(word_entries)  # each word contributes 1 entry for its first char
    return total


def size_key_to_char_index(char_readings) -> int:
    """offsets[23] (u32) + char_id entries (u16 each).
    Each char contributes 1 entry per unique first-byte of its readings."""
    total = 4 * 23
    for ch, readings in char_readings.items():
        unique_first_bytes = {r[0][0] for r in readings if r[0]}
        total += 2 * len(unique_first_bytes)
    return total


# -- Reference composition_matches (pure Python) ------------------------------

def composition_matches(char_ids, reading_idxs, char_table_list, user_keys):
    """
    Backtracking reading matcher: for each char position, the char has one
    designated reading (reading_idxs[i]). Try matching that reading's key
    prefix against user_keys starting at current offset, with prefix lengths
    { full, first 2 bytes, first 1 byte }. Recurse on success. Returns True
    when user_keys is fully consumed by a valid composition, including
    partial prefix of the word.
    """
    def recurse(char_idx, key_idx):
        if key_idx >= len(user_keys):
            return True  # user input fully consumed — prefix match ok
        if char_idx >= len(char_ids):
            return False  # no more chars but user keys remain
        readings = char_table_list[char_ids[char_idx]]
        if not readings:
            return False
        reading_key = readings[reading_idxs[char_idx]][0]
        rlen = len(reading_key)
        remaining = len(user_keys) - key_idx
        # Try full / first-2 / first-1 prefix of this reading's key sequence.
        tried = set()
        for plen in (rlen, min(2, rlen), min(1, rlen)):
            if plen <= 0 or plen in tried:
                continue
            tried.add(plen)
            if plen > remaining:
                # User input ends mid-reading: check if user_keys tail is a
                # prefix of this reading's first-plen bytes.
                if reading_key[:remaining] == user_keys[key_idx:]:
                    return True
            elif reading_key[:plen] == user_keys[key_idx : key_idx + plen]:
                if recurse(char_idx + 1, key_idx + plen):
                    return True
        return False

    return recurse(0, 0)


# -- Projection model for expansion sources ----------------------------------─

def project_with_expansion(char_table_bytes, word_table_bytes, first_idx_bytes,
                           word_count, char_count,
                           add_chars=0, add_words=0, avg_word_len=4):
    """
    Project dict size after adding N new chars (from Unihan) and N new
    multi-char entries (from idiom corpus / MoE). Uses averages from current
    stats to estimate incremental bytes.
    """
    per_char_bytes = char_table_bytes / max(1, char_count)
    new_char_table = char_table_bytes + add_chars * per_char_bytes

    # Per word: 4 header + avg (2N + N) = 4 + 3N
    per_word_bytes = 4 + 3 * avg_word_len
    new_word_table = word_table_bytes + add_words * per_word_bytes

    # first_char_index grows by the new words
    new_first_idx = first_idx_bytes + 4 * add_words

    return int(new_char_table + new_word_table + new_first_idx)


# -- Report generation ------------------------------------------------------─

def run():
    dat_path = Path("firmware/mie/data/dict_dat.bin")
    val_path = Path("firmware/mie/data/dict_values.bin")
    if not dat_path.exists() or not val_path.exists():
        print(f"ERROR: dict files not found at {dat_path.parent}", file=sys.stderr)
        return 2

    v2_total = dat_path.stat().st_size + val_path.stat().st_size

    print("=" * 72)
    print("MIE v4 Composition Architecture — PoC Simulation")
    print("=" * 72)

    print("\n[1/5] Parsing current MIED v2 dict...")
    records = parse_v2_dict(dat_path, val_path)
    print(f"  total (key, word) records:        {len(records):>10,}")
    print(f"  v2 file total size:               {v2_total:>10,} bytes "
          f"({v2_total/1024/1024:.2f} MB)")

    print("\n[2/5] Deriving char_table from 1-char dict entries...")
    char_readings = derive_char_readings_from_1char_entries(records)
    all_chars = collect_all_cjk_chars_from_phrases(records)
    missing = all_chars - set(char_readings.keys())
    print(f"  chars with explicit readings:     {len(char_readings):>10,}")
    print(f"  all CJK chars referenced:         {len(all_chars):>10,}")
    print(f"  gap (phrase-only, no 1-char entry): {len(missing):>8,}")
    reading_count_dist = defaultdict(int)
    for rs in char_readings.values():
        reading_count_dist[len(rs)] += 1
    total_readings = sum(len(rs) for rs in char_readings.values())
    print(f"  total readings across all chars:  {total_readings:>10,}")
    print(f"  avg readings per char:            {total_readings/max(1,len(char_readings)):.2f}")

    print("\n[3/5] Building word_table from multi-char dict entries...")
    word_entries, char_to_id = build_word_table(records, char_readings)
    print(f"  multi-char words in v4 table:     {len(word_entries):>10,}")
    len_dist = defaultdict(int)
    for w in word_entries:
        len_dist[len(w["char_ids"])] += 1
    for n in sorted(len_dist):
        print(f"    {n}-char words:                 {len_dist[n]:>10,}")

    print("\n[4/5] Computing v4 layout byte sizes...")
    h_sz = size_header()
    ct_sz = size_char_table(char_readings)
    wt_sz = size_word_table(word_entries)
    fi_sz = size_first_char_index(word_entries, len(char_to_id))
    ki_sz = size_key_to_char_index(char_readings)
    total = h_sz + ct_sz + wt_sz + fi_sz + ki_sz

    print(f"  header:                           {h_sz:>10,} bytes")
    print(f"  char_table:                       {ct_sz:>10,} bytes  "
          f"({ct_sz/1024:>6.1f} KB)")
    print(f"  word_table:                       {wt_sz:>10,} bytes  "
          f"({wt_sz/1024:>6.1f} KB)")
    print(f"  first_char_index:                 {fi_sz:>10,} bytes  "
          f"({fi_sz/1024:>6.1f} KB)")
    print(f"  key_to_char_index:                {ki_sz:>10,} bytes  "
          f"({ki_sz/1024:>6.1f} KB)")
    print(f"  ------------------------------------------------------")
    print(f"  TOTAL dict_mie.bin (simulated):   {total:>10,} bytes  "
          f"({total/1024/1024:.2f} MB)")
    print(f"  vs current v2 (dat+values):       {v2_total:>10,} bytes  "
          f"({v2_total/1024/1024:.2f} MB)")
    savings = 100 * (1 - total / v2_total)
    print(f"  savings vs v2:                    {savings:>10.1f} %")

    # Expansion projections
    print("\n  Expansion projections (approximate):")
    proj_unihan = project_with_expansion(
        ct_sz, wt_sz, fi_sz, len(word_entries), len(char_to_id),
        add_chars=len(missing), add_words=0)
    proj_unihan_full = project_with_expansion(
        ct_sz, wt_sz, fi_sz, len(word_entries), len(char_to_id),
        add_chars=(20_000 - len(char_readings)), add_words=0)
    proj_idioms = project_with_expansion(
        ct_sz, wt_sz, fi_sz, len(word_entries), len(char_to_id),
        add_chars=0, add_words=30_000, avg_word_len=4)
    proj_all = project_with_expansion(
        ct_sz, wt_sz, fi_sz, len(word_entries), len(char_to_id),
        add_chars=(20_000 - len(char_readings)), add_words=30_000,
        avg_word_len=4)

    print(f"    + fill phrase-char gap (+{len(missing)} chars):   "
          f"{proj_unihan/1024/1024:.2f} MB")
    print(f"    + full Unihan to 20K chars:        "
          f"{proj_unihan_full/1024/1024:.2f} MB")
    print(f"    + 30K idioms to word_table:        "
          f"{proj_idioms/1024/1024:.2f} MB")
    print(f"    + both (20K chars + 30K idioms):   "
          f"{proj_all/1024/1024:.2f} MB")

    print("\n[5/5] Reference composition_matches latency profile...")
    # Build char_table_list indexed by char_id
    id_to_char = {v: k for k, v in char_to_id.items()}
    char_table_list = [char_readings[id_to_char[cid]] for cid in range(len(char_to_id))]

    # Build first_char_index (char_id -> list of word_ids sorted by freq desc)
    first_char_idx = defaultdict(list)
    for wid, w in enumerate(word_entries):
        if w["char_ids"]:
            first_char_idx[w["char_ids"][0]].append((w["freq"], wid))
    for cid in first_char_idx:
        first_char_idx[cid].sort(reverse=True)

    # Build key-first-byte -> candidate char_ids
    key_to_chars = defaultdict(list)
    for cid, readings in enumerate(char_table_list):
        first_bytes = {r[0][0] for r in readings if r[0]}
        for b in first_bytes:
            key_to_chars[b].append(cid)

    # Generate test inputs from word_table itself (so they match reading_idx=0
    # paths). For each sampled word, build candidate inputs:
    #   - first char's first reading, full key sequence (1-syllable input)
    #   - first char's first reading, first byte only (1-key abbreviation)
    #   - first 2 chars' first readings concatenated (2-syllable input)
    # This yields realistic prefixes that should hit the candidate set.
    random.seed(42)
    sampled_words = random.sample(word_entries, min(40, len(word_entries)))
    sample_keys = []
    for w in sampled_words:
        readings = char_table_list[w["char_ids"][0]]
        if not readings or not readings[0][0]:
            continue
        first_key = readings[0][0]
        sample_keys.append(first_key)              # full first syllable
        sample_keys.append(first_key[:1])          # 1-byte abbrev
        if len(w["char_ids"]) >= 2:
            r2 = char_table_list[w["char_ids"][1]]
            if r2 and r2[0][0]:
                sample_keys.append(first_key + r2[0][0])  # 2 syllables
    # Dedup and cap at 100
    seen = set()
    sample_keys = [k for k in sample_keys if not (k in seen or seen.add(k))][:100]

    times_ns = []
    match_counts = []
    MAX_CANDIDATES = 50
    for user_keys in sample_keys:
        t0 = time.perf_counter_ns()
        matched = 0
        seen_wids = set()
        first_byte = user_keys[0]
        for cid in key_to_chars.get(first_byte, []):
            for freq, wid in first_char_idx.get(cid, []):
                if wid in seen_wids:
                    continue
                seen_wids.add(wid)
                w = word_entries[wid]
                if composition_matches(w["char_ids"], w["reading_idxs"],
                                        char_table_list, user_keys):
                    matched += 1
                    if matched >= MAX_CANDIDATES:
                        break
            if matched >= MAX_CANDIDATES:
                break
        t1 = time.perf_counter_ns()
        times_ns.append(t1 - t0)
        match_counts.append(matched)

    times_ns.sort()
    n = len(times_ns)
    p50 = times_ns[n // 2]
    p90 = times_ns[int(n * 0.9)]
    pmax = times_ns[-1]
    avg_matches = sum(match_counts) / max(1, n)
    print(f"  samples:                          {n}")
    print(f"  avg candidates returned:          {avg_matches:.1f}")
    print(f"  search latency p50:               {p50/1000:>8.1f} us  (Python ref)")
    print(f"  search latency p90:               {p90/1000:>8.1f} us")
    print(f"  search latency max:               {pmax/1000:>8.1f} us")
    cpp_p50_est = p50 / 10  # C++ expected ~10x faster than Python
    print(f"  C++ estimate (p50 / 10):          {cpp_p50_est/1000:>8.1f} us")

    # Go/No-Go verdict
    print("\n" + "=" * 72)
    print("Go/No-Go Verdict")
    print("=" * 72)
    size_ok = total <= 2.5 * 1024 * 1024
    size_exp_ok = proj_all <= 2.5 * 1024 * 1024  # with expansion
    lat_py_ok = p50 <= 5_000_000  # 5 ms Python ref
    lat_cpp_ok = cpp_p50_est <= 500_000  # 500 µs C++ estimate

    def check(label, ok, detail):
        print(f"  [{('PASS' if ok else 'FAIL')}] {label:<40} {detail}")

    check("Dict <= 2.5 MB (current corpus):",
          size_ok, f"{total/1024/1024:.2f} MB")
    check("Dict <= 2.5 MB (20K chars + 30K idioms):",
          size_exp_ok, f"{proj_all/1024/1024:.2f} MB")
    check("Python ref search p50 <= 5 ms:",
          lat_py_ok, f"{p50/1000:.0f} us")
    check("C++ search p50 estimate <= 500 us:",
          lat_cpp_ok, f"{cpp_p50_est/1000:.0f} us")

    if size_ok and size_exp_ok and lat_py_ok and lat_cpp_ok:
        print("\n  -> GO for Phase 1")
        return 0
    else:
        print("\n  -> Review gates; consider architecture tuning before Phase 1")
        return 1


if __name__ == "__main__":
    sys.exit(run())
