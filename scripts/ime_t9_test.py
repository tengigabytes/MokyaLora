#!/usr/bin/env python3
"""ime_t9_test.py — SmartEn (T9) stress test for MIE English input.

Tokenises an English passage into [word | whitespace | punctuation].
For each word:
  1. Primary path — SmartEn T9: one key press per letter, look for the
     target word case-insensitively in the candidate list. Commit at
     its rank if found.
  2. Fallback — Direct multitap: if T9 misses (proper nouns, tech
     initialisms, numbers-in-words), switch to Direct mode and type
     each letter via its key-multitap index.
Whitespace becomes SPACE. Punctuation goes through Direct mode when
the char is in the Direct key table, otherwise is reported as MISS.

Output is a per-word ledger plus stats: T9 hits at which ranks,
fallbacks, misses, total keystrokes (T9 + fallback) vs the
theoretical minimum (1 per letter + 1 OK).

Reuses MokyaSwd + ImeDriver from ime_text_test; inherits the
--transport swd|rtt flag and the wait_until fast-poll helpers.

Usage:
    python scripts/ime_t9_test.py PATH [--limit N] [--transport swd|rtt]

SPDX-License-Identifier: MIT
"""
import argparse
import re
import string
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ime_text_test import (                                # type: ignore
    ImeDriver, MokyaSwd, CHAR_TO_DIRECT, DIRECT_KEY_SLOTS,
    MODE_SMART_EN, MODE_DIRECT, IME_VIEW_INDEX,
    load_keycode_map,
)

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass


# ── T9 key mapping (letter → key name). Taken from
# firmware/mie/src/ime_keys.cpp kKeyTable row 1-3 letter slots.
# Both lower- and upper-case letters share the same key — SmartEn
# dict matches case-insensitively. ────────────────────────────────────
T9_LETTER_TO_KEY = {}
for key_name, slots in DIRECT_KEY_SLOTS:
    if not key_name.startswith('KEY_') or any(c and c.isdigit() for c in slots):
        continue
    for c in slots:
        if c is None: continue
        T9_LETTER_TO_KEY.setdefault(c.lower(), key_name)

# ── Tokeniser. Words are [A-Za-z0-9]+; everything else is a
# single-char token (space, punctuation). Keeps the stream 1:1 with
# the input so the reconstructed text can be diffed. ────────────────
_TOKEN_RE = re.compile(r"[A-Za-z0-9]+|\s|[^A-Za-z0-9\s]")
def tokenise(text):
    return _TOKEN_RE.findall(text)


def word_key_sequence(word):
    """Return the list of MOKYA_KEY_X names to press for this word
    under T9, or None if any letter isn't T9-reachable."""
    seq = []
    for c in word.lower():
        if c.isdigit():
            # Digits aren't in the English dict at all — T9 path
            # cannot succeed; defer to multitap fallback.
            return None
        key = T9_LETTER_TO_KEY.get(c)
        if key is None: return None
        seq.append(key)
    return seq


def find_candidate(drv, target):
    """Case-insensitive search of the full 100-candidate list for
    target. Returns rank or None."""
    _, words = drv.read_all_candidates()
    t_lower = target.lower()
    for i, w in enumerate(words):
        if w and w.lower() == t_lower:
            return i
    return None


def type_word_t9(drv, word, stats):
    """Attempt one T9 word. Returns True on commit (via any path),
    False if the word was fundamentally unreachable (digits not
    supported yet). Updates stats in-place."""
    keys = word_key_sequence(word)
    if keys is None:
        # Digits path — multitap can type digits via the number-row
        # keys. Route directly to multitap fallback.
        return type_word_multitap(drv, word, stats, from_reason='digits')

    drv.ensure_mode(MODE_SMART_EN)
    kc_list = [drv.km[k] for k in keys]
    # One tap per letter.
    drv.inject_keycodes(kc_list)
    # Give the engine a tick to fold the keystrokes into candidates.
    # pending_len is non-zero while SmartEn holds an open word; the
    # wait is bounded by the usual 0.4 s deadline.
    drv.wait_until(drv.read_pend_len, lambda v: v > 0, timeout_s=0.4)
    rank = find_candidate(drv, word)
    if rank is None:
        # T9 miss — abort this attempt so multitap can take over.
        # DEL each letter to clear pending (SmartEn DEL pops one byte).
        drv.inject_keycodes([drv.KC_DEL] * len(kc_list))
        drv.wait_until(drv.read_pend_len, lambda v: v == 0, timeout_s=0.4)
        stats['t9_miss_words'].append(word)
        return type_word_multitap(drv, word, stats, from_reason='t9-miss')

    # Commit at rank.
    pre_len = drv.read_text_len()
    drv.commit_rank(rank)
    drv.wait_until(drv.read_text_len,
                   lambda v: v != pre_len, timeout_s=0.6)
    # Count keystrokes: one per letter + (rank) RIGHT + 1 OK.
    ks = len(kc_list) + rank + 1
    stats['t9_hit'] += 1
    stats['t9_ranks'].append(rank)
    stats['keystrokes'] += ks
    stats['ideal_keystrokes'] += len(kc_list) + 1
    return True


def type_word_multitap(drv, word, stats, from_reason):
    """Type the word one character at a time in Direct mode via
    multitap. Returns True on commit, False if any char isn't in
    the Direct table (e.g. non-ASCII)."""
    drv.ensure_mode(MODE_DIRECT)
    ks_this = 0
    for ch in word:
        plan = CHAR_TO_DIRECT.get(ch)
        if plan is None:
            stats['unreachable_chars'].append((word, ch))
            return False
        key_name, idx = plan
        kc = drv.km[key_name]
        pre_len = drv.read_text_len()
        drv.inject_keycodes([kc] * (idx + 1) + [drv.KC_OK])
        drv.wait_until(drv.read_text_len,
                       lambda v: v != pre_len, timeout_s=0.5)
        ks_this += (idx + 1) + 1
    stats['multitap'] += 1
    stats['multitap_reasons'][from_reason] = \
        stats['multitap_reasons'].get(from_reason, 0) + 1
    stats['keystrokes'] += ks_this
    stats['ideal_keystrokes'] += len(word) + 1
    return True


def type_separator(drv, ch, stats):
    """Space / newline / punctuation. SmartEn commits on SPACE/OK so
    we have to exit the word boundary cleanly regardless of where
    we came from. Punctuation falls back to Direct."""
    if ch.isspace():
        # SPACE in Direct OR SmartEn both emit a literal space and
        # close any open word. Prefer SmartEn (cheaper, auto-caps).
        drv.ensure_mode(MODE_SMART_EN)
        pre_len = drv.read_text_len()
        drv.inject_keycodes([drv.KC_SPACE])
        drv.wait_until(drv.read_text_len,
                       lambda v: v != pre_len, timeout_s=0.4)
        stats['keystrokes'] += 1
        stats['ideal_keystrokes'] += 1
        return True
    # Punctuation: use Direct multitap if the char is in the table,
    # otherwise declare unreachable (a future pass could route
    # through SYM1 picker for , . ; : etc.).
    plan = CHAR_TO_DIRECT.get(ch)
    if plan is None:
        stats['punct_unreachable'].append(ch)
        return False
    drv.ensure_mode(MODE_DIRECT)
    key_name, idx = plan
    kc = drv.km[key_name]
    pre_len = drv.read_text_len()
    drv.inject_keycodes([kc] * (idx + 1) + [drv.KC_OK])
    drv.wait_until(drv.read_text_len,
                   lambda v: v != pre_len, timeout_s=0.5)
    stats['keystrokes'] += (idx + 1) + 1
    stats['ideal_keystrokes'] += 2
    stats['punct_ok'] += 1
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', type=Path)
    ap.add_argument('--elf', default='build/core1_bridge/core1_bridge.elf')
    ap.add_argument('--limit', type=int, default=0,
                    help='stop after N tokens (0 = all)')
    ap.add_argument('--transport', choices=['swd', 'rtt'], default='swd')
    args = ap.parse_args()

    text = args.path.read_text(encoding='utf-8')
    tokens = tokenise(text)
    if args.limit:
        tokens = tokens[:args.limit]
    words = [t for t in tokens if re.match(r'^[A-Za-z0-9]+$', t)]
    print(f"Passage: {args.path.name}  tokens={len(tokens)}  words={len(words)}")

    keymap = load_keycode_map()

    stats = {
        't9_hit': 0, 't9_ranks': [],
        't9_miss_words': [],
        'multitap': 0, 'multitap_reasons': {},
        'punct_ok': 0, 'punct_unreachable': [],
        'unreachable_chars': [],
        'keystrokes': 0, 'ideal_keystrokes': 0,
    }

    with MokyaSwd(elf=args.elf) as swd:
        drv = ImeDriver(swd, keymap, transport=args.transport)
        drv.check_alive()
        drv.cycle_view_to(IME_VIEW_INDEX)
        drv.reset_text()

        t0 = time.time()
        for idx, tok in enumerate(tokens):
            if re.match(r'^[A-Za-z0-9]+$', tok):
                ok = type_word_t9(drv, tok, stats)
                tag = "OK" if ok else "UNREACH"
                per = (time.time() - t0) / (idx + 1)
                if (idx + 1) % 10 == 0 or not ok:
                    print(f"  [{idx+1:3d}/{len(tokens)}] word {tok!r:20s}  {tag}  "
                          f"({per*1000:.0f} ms/token avg)")
            else:
                type_separator(drv, tok, stats)

        dt = time.time() - t0
        total_words = stats['t9_hit'] + stats['multitap']
        print(f"\n=== Summary: {len(tokens)} tokens, {total_words} words typed "
              f"in {dt:.1f}s ===")
        print(f"  T9 hits        : {stats['t9_hit']}  ({100*stats['t9_hit']/max(1,len(words)):.1f}% of words)")
        if stats['t9_ranks']:
            ranks = stats['t9_ranks']
            print(f"    avg rank       : {sum(ranks)/len(ranks):.2f}")
            print(f"    rank 0 (1-tap) : {sum(1 for r in ranks if r == 0)}")
            print(f"    rank 1-3       : {sum(1 for r in ranks if 1 <= r <= 3)}")
            print(f"    rank 4-7       : {sum(1 for r in ranks if 4 <= r <= 7)}")
            print(f"    rank >= 8      : {sum(1 for r in ranks if r >= 8)}")
        print(f"  Multitap falls : {stats['multitap']}")
        for r, c in stats['multitap_reasons'].items():
            print(f"    reason {r:10s}: {c}")
        if stats['t9_miss_words']:
            miss_str = ', '.join(stats['t9_miss_words'][:20])
            more = f" (+{len(stats['t9_miss_words'])-20} more)" if len(stats['t9_miss_words']) > 20 else ''
            print(f"  T9-miss words  : {miss_str}{more}")
        print(f"  Punct OK       : {stats['punct_ok']}")
        if stats['punct_unreachable']:
            uniq = sorted(set(stats['punct_unreachable']))
            print(f"  Punct unreach  : {uniq}")
        if stats['unreachable_chars']:
            print(f"  Unreachable chars: {stats['unreachable_chars'][:10]}")
        ideal = stats['ideal_keystrokes']
        print(f"  Keystrokes     : {stats['keystrokes']} "
              f"(ideal {ideal}; overhead = {stats['keystrokes'] - ideal})")


if __name__ == '__main__':
    main()
