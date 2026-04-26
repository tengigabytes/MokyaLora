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

## Secondary observation: short-prefix commit not exploited

Every word in the stress test costs `letters + 1` keystrokes even
for very-high-freq words like `you` (= T · U · U · OK = 4 taps).
On a real Nokia T9 phone, `you` would drop to 2 taps:

    Y (= press KEY_T once) → rank 0 "you" shown
    OK                       → commit rank 0

i.e. the user exits the word after just one keystroke by spotting
that rank 0 already holds the intended word, skipping the remaining
two letters. This is what makes T9 predictive.

The firmware already supports this path — `OK` in SmartEn commits
the rank-0 candidate regardless of how many phonemes are in
key_seq_. The issue is **two-layered**:

1. The test script (`scripts/ime_t9_test.py`) does not even try
   short-prefix shortcuts. It always types every letter and then
   finds the word in the candidate list. So the reported
   "keystrokes / word" is a worst-case number, not the best-case
   shape a human typist actually experiences. Adding a "try commit
   at shortest prefix where word is rank 0" loop would realistically
   model typing and likely drop overall cost 20-30 % on common words.

2. Even if the test did this, the u16 freq saturation (above) means
   `you` / `to` / `the` all tie with ~2 000 other top words. Which
   one surfaces at rank 0 after a single keystroke is then a
   function of insertion order rather than actual frequency —
   unreliable. A realistic typist who presses Y expecting `you`
   might instead see `the` or something else surface first,
   defeating the short-prefix commit path.

The two need fixing together. Rank ordering must be meaningful at
every prefix length, AND the measurement harness must model the
shortest-prefix-commit typing path.

## The bigger gap: English has no stroke-saving equivalent to SmartZh

SmartZh delivers a non-trivial **per-word stroke saving** that SmartEn
currently does not:

    ㄍㄌㄉㄒ   → 高樓大廈   (4 initials → 4-char phrase, ~60 % savings)
    ㄒㄧ       → 吸          (2 keys → 1 char, already a direct hit)
    ㄕㄐㄈ     → 生機蓬       (3 initials → 3-char phrase)

This is implemented at two layers in v4:
- `CompositionSearcher::search` composes multi-char candidates from
  single-char readings, so an input of 3-4 *initial-only* keys can
  surface a complete multi-char Chinese phrase.
- The char_table stores per-char phoneme `pos_packed` so the search
  can match the short-form initial (primary phoneme) against a full
  syllable's reading.

For English the question is: what's the equivalent unit of saving?

| language | natural unit | what Chinese saves | what English could save |
|---|---|---|---|
| Chinese  | 1 syllable = 1 char | 4 initials ⇒ 4-char phrase | (not directly comparable) |
| English  | 1 letter             | — | 1-3 letters ⇒ whole word (prefix) |
|          |                     | — | 1 letter per word ⇒ multi-word phrase |

Two mechanisms that would bring English close to Chinese's savings:

### A. Within-word prefix commit (free; needs ranking fix + test harness)

Already supported by firmware: prefix-match + top-N by freq; OK
commits rank 0 at any prefix length. With reliable ranking, common
words drop to 1-3 keystrokes each:

    y + OK              → "you"             (2 taps vs 4)
    t + OK              → "the"             (2 taps vs 4)
    a + p + p + OK      → "apple"           (4 taps vs 6)
    p + r + o + OK      → "project"         (4 taps vs 8)

Blocked today by: (i) u16 freq clamp — every top-2 000 word ties at
65535; (ii) corpus bias; (iii) test harness doesn't try short-prefix
commits.

Solving these is TODO items 1, 2, 5 below. No new data structures
needed.

### B. Multi-word phrase prediction (new work; analog of SmartZh abbr)

The direct analog of `ㄍㄌㄉㄒ → 高樓大廈` is typing the **first
letter of each word in a common phrase**:

    g l d          → "good luck dave"    (3 taps for 14 chars)
    t y            → "thank you"         (2 taps for 9 chars)
    o m w          → "on my way"         (3 taps for 9 chars)
    f y i          → "for your info"     (3 taps for 13 chars)

This needs:
- A phrase/n-gram dict keyed on word-initial sequences, with each
  phrase freq-ranked the same way single words are.
- A builder step in `gen_dict.py` that mines common n-grams from
  the corpus (or a curated list of ~500-2000 idioms / greetings /
  chat phrases) and emits them alongside single-word entries.
- A search-path branch in SmartEn that, when the user's key_seq
  matches both a word and a phrase, surfaces both and lets the
  ranking decide.

Wire format: phrase entries can reuse the same `en_dat` trie with
a flag bit distinguishing "phrase expands to N words with spaces"
from "single word". Storage cost: a few thousand common phrases ≈
100 KB. Still inside the 5 MB v4 partition.

This is real work (probably 2-3 sessions including corpus curation
and UX validation) but it's the answer to "English has no
stroke-saving equivalent to SmartZh". Without it, English maxes
out at "prefix commit of single words" — useful but fundamentally
bounded.

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
5. **Short-prefix commit in the T9 test harness**. Modify
   `ime_t9_test.py::type_word_t9` to walk the word one keystroke
   at a time and commit as soon as the target surfaces at rank 0
   (or ≤ N for a threshold). Report two headline numbers: the
   current worst-case `letters + 1` path AND the predictive
   best-case `prefix + OK` path. Only meaningful once (2) has made
   rank ordering reliable.
   - Acceptance: on the stress passage, high-freq words like `you`,
     `the`, `to`, `and`, `is` cost ≤ 2 keystrokes (1 letter + OK);
     medium-freq 5-6 letter words cost ~3 keystrokes instead of 7.
   - Corollary: the summary report gets a "keys-saved vs letters+1"
     column alongside the current `overhead` column, so the
     predictive value of a given dict + ranking configuration is
     legible at a glance.
6. **Multi-word phrase prediction** (the SmartZh abbr analog).
   Extend `gen_dict.py` to mine word-initial n-grams and emit phrase
   entries into `en_dat` / `en_val` with a "phrase" flag bit. SmartEn
   search branch learns to surface phrases beside single words. Dict
   growth ~100 KB for 1-2 k phrases. Biggest unknown is corpus: need
   either an n-gram dataset or a hand-curated list of common chat
   phrases ("thank you", "on my way", "I don't know", ...).
   - Acceptance: typing `t y` produces `thank you` at rank 0-2, and
     similar for ~50 common phrases. Overall T9 stress passage
     keystroke count drops by another 15-25 % on natural text.
   - Scope: 2-3 sessions (builder + search branch + corpus + UX
     validation). Do NOT attempt before (1) and (2) — need reliable
     single-word ranking first, or the phrase layer just adds noise.

## Non-goals for this ticket

- Changing `TrieSearcher::search` itself. The algorithm is correct.
- Adding a grammar / POS model. Way too heavy for a device of this
  class.
- Cross-session sentence-aware prediction (Phase 2+).
