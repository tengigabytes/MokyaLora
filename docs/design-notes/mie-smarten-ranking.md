# SmartEn Ranking — known issues & TODO

Status: **analysed, not implemented**. Captured 2026-04-24 after T9
stress test with the full 50k English dict surfaced a counter-
intuitive ordering.

## Observed symptom

Typing the T9 sequence for `app` (keys A · O · O) returns:

    1.  appreciate   (freq 51 258)
    2.  applause     (freq 40 194)
    3.  apparently   (freq 38 705)
    4.  appear       (freq 21 449)
    5.  appointment  (freq 20 930)
    ...
    8.  apple        (freq 16 192)
    ...

Users expect `apple` to be at or near rank 0 — "apple" feels like the
most common "app"-prefix word in everyday typing.

## Why the current ranking is technically correct

- **SmartEn search = prefix scan + top-N by frequency**
  `firmware/mie/src/trie_searcher.cpp::TrieSearcher::search` does a
  binary search to the first key ≥ query, then scans forward while
  `memcmp(kbuf, query, qlen) == 0`, inserting (word, freq) into a
  size-capped output buffer sorted by `freq` descending
  (lines 199-280). The bucket in `en_values.bin` is already
  freq-sorted per key. Whatever is at rank 0 is the highest-freq
  dict entry whose T9 key-seq starts with the user's typed prefix.

- **Prefix prediction is already wired.** Every additional keystroke
  narrows the candidate set — typing more letters of a word filters
  down to matches. No separate "predict next word" model exists, but
  the prefix-scan behaviour is functionally the same for within-word
  completion.

So the search engine is honouring the corpus faithfully — `apple`
ranks below `applause` because `applause` has a higher count in the
source wordlist.

## Root cause: corpus is optimised for the wrong thing

`firmware/mie/data_sources/en_50k.txt` is derived from hermitdave's
FrequencyWords project, which is built from movie/TV subtitle
transcripts. Subtitles contain non-typed artefacts that would never
appear in a user's own writing:

- Stage directions and sound cues: `[applause]`, `(laughs)`,
  `[gasps]`, `[music]`. These inflate `applause`, `laughs`,
  `gasps`, etc. well past their real-world typing frequency.
- Filler words: `uh`, `um`, `yeah`, `okay` are over-represented.
- Scripted exclamations: `appreciate that`, `apparently`,
  `excellent` etc. are spoken far more often on screen than they
  are typed.

For an input method we want a corpus that reflects **what users
actually type**, not what they hear. Options:

1. Replace `en_50k.txt` with a typing-oriented corpus. Candidates:
   - Google Web Trillion Word Corpus (heavy typing bias, CC-BY-SA).
   - Google N-grams (Wikipedia-filtered subset) — neutral written
     English.
   - Reddit/Twitter corpora (over-indexed on slang but reflects
     casual typing).
   - Keep `google-10000-english-no-swears.txt` (our old source) but
     extend with a per-word manual curated list — small but more
     predictable.
2. Keep `en_50k.txt` but post-process: drop obvious subtitle
   artefacts (`applause`, `laughs`, stage-direction words) via a
   blacklist, then re-rank.
3. Blend: seed ranks from a typing corpus, fall back to subtitle
   corpus for long-tail coverage not present in the typing one.

## Secondary issue: u16 freq saturation

`build_value_record` clamps `freq` to `min(freq, 65535)` before
storing it in `en_values.bin`. `en_50k.txt` has many top words above
this ceiling:

    you  28 787 591    →  65535
    i    27 086 011    →  65535
    the  22 761 659    →  65535
    to   17 099 834    →  65535
    ...

So roughly the top 2-3k most common words **all tie at 65535** and
are ordered among themselves by whatever secondary sort the builder
applies (probably dict-insertion order). That's fine for correctness
but means the ranking within the first few keystrokes' candidate
sets is essentially arbitrary.

Fixes:
- Rescale freq to fit u16 cleanly before quantisation (e.g. `log2`
  + linear map into 0..65535). Preserves ordering across the whole
  range at the cost of some precision per unit of freq.
- Widen the freq field to u32 in the on-disk format. +2 bytes/word
  for 50k entries = +100 kB en_val size, 20 % growth. Affordable.
- Demand a corpus whose top freq ≤ 65535 to begin with (rare —
  most real corpora spike far higher).

The rescale approach is cheapest and loses the least information.

## Corpus vs key-seq ambiguity (not a bug)

Typing A-O-O also matches non-`app-` prefixes like `soo`, `apo`,
`sop`, `sp-`. These interpretations are folded into the same
candidate list sorted by freq, so a high-freq `so` or `soon` can
outrank an `app-` word at very short prefix queries. This is
fundamental to T9 — ambiguity resolves as the user types more keys
— and is not something the ranking code should try to fix.

## TODO (post P1.6)

In priority order:

1. **Replace / augment English corpus**. Source a typing-weighted
   wordlist and re-run the dict build. Keep `en_50k.txt` in tree
   as a fallback; add `en_typing.txt` as the new default, gated by
   CMake flag for easy A/B comparison.
   - Acceptance: on a T9 stress pass of the "Hardware Engineer's
     Dilemma" passage, `apple` / common typing words land at rank 0.
2. **Log-quantise `freq` on dict build**. Apply a monotonic rescale
   so no more than ~1 000 words tie at u16 max. Keep field width;
   only the mapping changes.
   - Acceptance: no more than 5 % of en_values entries share the
     same freq at the top of the distribution.
3. **Personalised frequency (deferred — needs LittleFS / P1.6.1
   extension)**. Track committed-word counts locally over time, add
   a learned-bonus component to the ranking. Same shape as Phase 1.6
   LRU, but for SmartEn instead of SmartZh. Would make the initial
   corpus choice much less critical.
4. **Optional stage-direction blacklist** layered on top of (1) as a
   cheap correctness filter. 200 words would cover 90 % of the
   subtitle-specific artefacts.

## Non-goals for this ticket

- Changing `TrieSearcher::search` itself. The algorithm is correct.
- Adding a grammar / POS model. Way too heavy for a device of this
  class.
- Cross-session sentence-aware prediction (Phase 2+).
