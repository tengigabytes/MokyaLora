# MIE Phase 1.6 — Personalised LRU Cache (design plan)

Status: **planned**, not implemented. Supersedes the dropped P1.5
(Unihan / MoE dict expansion); see phase2-log "P1.5 dropped" entry.

## Problem statement

Post-Phase-1.4 validation shows the remaining IME pain is **not
coverage** (6 passages, 1 416 CJK, 0 miss) but **repeat-typer cost**:

- `吸` (ㄒㄧ¹, rank 21 in its bucket) — hit 4× in a single passage,
  each time requiring DPAD RIGHT × 21 + OK.
- `鮣` (ㄧㄣˋ, rank 16) — hit 2×, same pattern.
- Names of people / places (e.g. `XX`, `YY`) — one-off short
  rank, but the user will type them again next session.

A personalised LRU that learns from the first commit and promotes
subsequent identical-reading commits to rank 0-7 directly eliminates
this class of friction.

## Goals

1. After the first commit of a reading, subsequent identical inputs
   within the session surface that committed word in the top 5.
2. Persist across reboots (LittleFS).
3. Zero dict bloat, no global-freq bias — each device personalises
   independently.
4. Memory budget: ≤ 8 KB RAM, ≤ 8 KB LittleFS.

## Non-goals

- Training on UN-committed candidates (Phase 2+ could explore).
- Cloud sync / sharing across devices.
- Tokenised phrase suggestions (that's Phase 2 "sentence-aware
  completion"; out of 1.6 scope).

## Data structure

```c
// mie/lru_cache.h — Apache-2.0, lives alongside ImeLogic.
typedef struct {
    uint8_t  kbytes[8];     // reading keyseq, LSB-aligned
    uint8_t  klen;          // 1..8 active bytes
    uint8_t  pos_packed;    // 2-bit-per-byte phoneme position (matches
                            // dict's phoneme_pos byte); 0xFF per
                            // nibble = "any"
    uint8_t  tone;          // 0=unspecified, 1..5
    uint8_t  utf8_len;      // bytes in utf8[]
    char     utf8[24];      // committed word, matches Candidate width
    uint32_t last_used_ms;  // eviction key
    uint16_t use_count;     // weak bonus; breaks last-used ties
    uint16_t reserved;      // pad to 48 B
} lru_entry_t;
// sizeof(lru_entry_t) == 48.

#define LRU_CAP 128u
extern lru_entry_t g_lru[LRU_CAP];
extern uint32_t    g_lru_count;   // 0..LRU_CAP
```

Size: 128 × 48 = 6 144 B RAM + same on flash = fits comfortably.

## Integration points

### Lookup (ime_search.cpp)

New first stage in `ImeLogic::run_search_v4`, before the existing
CompositionSearcher call:

```cpp
int lru_n = lru_lookup(key_seq_, key_seq_len_,
                       phoneme_hint_,
                       candidates_, kMaxCandidates);
// lru_n entries filled at head of candidates_[], tagged via
// candidates_prefix_keys_[i] = key_seq_len_ (same convention).
// Then run normal v4 search, dedup against lru hits.
```

Matching rule:
- `entry.klen == key_seq_len_` AND `entry.kbytes[0..klen) == key_seq_[0..klen)`
- For each byte `k`, if `phoneme_hint_[k] != 0xFF`, require
  `entry.pos(k) == phoneme_hint_[k]` (or entry.pos(k) == any).
- Bonus: also match when `entry.klen < key_seq_len_` AND the entry's
  kbytes is a prefix of user_keys (covers "start typing a name →
  name pops up early").

Ordering among LRU hits: `last_used_ms` desc, tiebreak by `use_count`.

### Commit (ime_commit.cpp)

Upsert hook in `did_commit`:

```cpp
void ImeLogic::did_commit(const char* utf8) {
    // ... existing body ...
    if (mode_ == InputMode::SmartZh && key_seq_len_ > 0) {
        lru_upsert(key_seq_, key_seq_len_,
                   phoneme_hint_, /*tone=*/get_commit_tone(),
                   utf8, now_ms());
    }
}
```

Upsert semantics:
1. Scan for entry matching (kbytes, pos_packed, utf8) exactly.
   - If found: `last_used_ms = now`; `use_count++`.
2. Otherwise: evict the LRU entry (min `last_used_ms`), then insert.

Rationale for matching on utf8 too: the same (reading, pos) can
legitimately commit different words (homophones the user intentionally
picked at different times). Two entries for 市/事/視 under the same
ㄕˋ input is correct.

### Persistence (new module)

`firmware/core1/src/ime/lru_persist.c` (Apache-2.0):

- `lru_save(path)` → serialise `{ magic, version, count, entries[count] }`
  to LittleFS.
- `lru_load(path)` → reverse.
- Called:
  - On boot after `ime_task_start` (best-effort; skip on any error).
  - Throttled write: every 50 commits OR on `MODE` cycle OR on
    `abort()` (session-end signal).
  - Flash safety: already handled by the P2-11 `--wrap` wrap — no
    extra care needed.

File layout:
```
  magic       4 B   "LRU1"
  version     2 B   u16 = 1
  count       2 B   u16 <= LRU_CAP
  entries     count * 48 B
```

Expected file size: 48 × 128 + 8 = 6 152 B → one LittleFS block on
the 4 KB filesystem (spills to 2 blocks, fine).

## Tests

### Host unit (firmware/mie/tests/test_lru_cache.cpp — new)

1. Upsert on empty table — entry at index 0.
2. Upsert same key+word twice — no new entry; `use_count` = 2.
3. Eviction when full — LRU (min `last_used_ms`) dropped.
4. Prefix lookup — `entry.klen < user_n` AND matches returns hit.
5. Phoneme-hint filter — short-tap vs long-press cycled commits don't
   collide.
6. Tone-1 marker handling — trailing SPACE in user input strips
   correctly vs stored kbytes.

### Hardware SWD regression

Re-run the five user passages. Expect:
- First pass: identical to current numbers.
- Second pass (same passage, no reset): repeat chars like 吸 / 鮣 /
  個 / 的 drop to rank 0-5 instead of their bucket rank.
- Power cycle test: persist → reboot → second pass still benefits.

Explicit test script: `scripts/test_lru_regression.py` (new) that
runs `ime_text_test.py passage --user-sim` twice with a
`scripts/mokya_swd.py`-driven reboot in between, compares histograms.

## Risk / mitigation

| risk | mitigation |
|---|---|
| Short-kbytes entry (e.g. just ㄅ) dominates ranking | Require `entry.klen == user_n` OR entry is a prefix AND `user_n > entry.klen`; entries for length-1 kbytes get a downweight |
| LittleFS wear | 50-commit + mode-cycle throttle; expected ~200 writes/day ≤ 10⁵/year « rated erase cycles |
| Cross-mode pollution (committing a word in SmartEn accidentally populating SmartZh cache) | Cache keyed by mode; separate `g_lru_zh` / `g_lru_en` tables or a mode-tag field |
| Heap-free constraint on Core 1 | Static 6 KB BSS, no dynamic allocation |
| Corrupted file on boot | Magic + version + length checks; fall back to empty cache on mismatch |

## Rollout

1. Implement `lru_cache.{h,c}` in MIE (MIT-licensed — reusable by
   non-FreeRTOS callers like mie_repl). Host unit tests first.
2. Wire `lru_lookup` into `ime_search.cpp::run_search_v4`; wire
   `lru_upsert` into `ime_commit.cpp::did_commit`.
3. Add persistence module in Core 1 (Apache-2.0) using LittleFS APIs.
4. Measure: SWD regression on all five passages, report second-pass
   rank improvement.
5. Document in phase2-log + mie-v4-status; commit.

Target: 2-3 sessions. Estimated +400 LoC including tests.
