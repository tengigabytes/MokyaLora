# MIE v4 Composition Architecture — Status

Last updated: 2026-04-24

## Overview

The MokyaInput Engine has been refactored to **v4 Composition Architecture** to
solve three independent problems found via SWD+RTT latency profiling:

1. **Performance**: v2 `TrieSearcher::search` did O(words × 50) dedup linear
   scan over PSRAM — 580 ms worst case for common Bopomofo initials (ㄅ ㄉ).
2. **Coverage**: v2 dict had only 1,479 single chars reachable as 1-syllable
   IME input; users couldn't type many CJK chars.
3. **Ranking**: Same dict held all word lengths competing by freq, so 4-char
   成語 were perpetually ranked below higher-freq 2-char compounds.

## Architecture (one-paragraph summary)

A single `dict_mie.bin` (MIE4 magic) holds:
- `char_table`: 18,722 unique chars × per-char readings (key bytes + tone + freq)
- `word_table`: 128,855 multi-char words as `char_id[N]` sequences with
  per-position `reading_idx[N]` overrides for 多音字
- `first_char_idx`: per char_id → freq-sorted word_id list
- `key_to_char_idx`: per phoneme-key byte → candidate char_ids
- `char_offsets` / `word_offsets`: pre-computed u32 offsets into char_table /
  word_table (zero-heap embedded loader)

`CompositionSearcher::search(user_keys, n, target_char_count, ...)` walks
`key_to_char_idx[user_keys[0]]` → candidate chars → `first_char_idx[cid]` →
word_ids → `composition_matches` recursive backtracker over each char's
designated reading. **No abbreviation pre-computation in the dict** — search-
time composition derives all valid prefix patterns from char readings.

`ImeLogic::run_search_v4()` dispatches by `count_positions(key_seq_)`:
- 1-position → `search_chars` (char_table, not word_table group 1)
- 2-4 position → `target_char_count = positions` + adjacent-bucket fallback
- 5+ position → **char-by-char mode**: 1-char candidates for first initial
  (commit consumes 1 byte) + appended 5+ phrase matches as bonus

## Completed (Phase 0-5, Phase 1.4 A/B/C)

| Phase | Item | Status |
|---|---|---|
| 1.4 A | Long-press multitap cycling (short=fuzzy, long×N = primary/secondary/tertiary, mod phoneme_count) | ✅ 2026-04-24 |
| 1.4 A | Dict phoneme_pos byte (+1 % size) + `CompositionSearcher` hint filter | ✅ |
| 1.4 A | Engine `lp_cycle_` FSM + KeyEvent.flags + keypad_scan deferred press + 2-byte inject ring | ✅ |
| 1.4 B | SYM1 long-press 4×4 symbol picker (「」『』（）【】，。、；：？！…) | ✅ 2026-04-24 |
| 1.4 C | Idle OK = newline (mirrors idle SPACE = " ") | ✅ 2026-04-24 |
| 1.4   | 150 host unit tests green | ✅ |
| 1.4   | 6-passage SWD regression: 1 416 CJK / 100 % top-100 / 0 miss + 136/136 picker punct + 21/21 newlines + 48/48 spaces | ✅ |
| 0 | PoC simulation script (`scripts/poc_mie_v4_simulation.py`) | ✅ |
| 0 | Validation: dict ≤ 2.5 MB, ref-search p50 ≤ 5 ms | ✅ |
| 1 | `gen_dict.py` MIED v4 builder (parallel `--v4-output` flag) | ✅ |
| 1 | v4 binary format with embedded offset sections | ✅ |
| 1 | `verify_mied_v4.py` round-trip parser | ✅ |
| 2 | `CompositionSearcher` C++ class (header + impl + 8 tests) | ✅ |
| 2 | Backtracking `composition_matches` for abbreviation derivation | ✅ |
| 2 | `search_chars` for 1-char results from char_table | ✅ |
| 2 | Heap-free embedded loader (offsets read from file, not built) | ✅ |
| 3 | Position counter (H2 heuristic) in `ime_keys.cpp` | ✅ |
| 3 | `ImeLogic::run_search_v4` with bucket dispatch + fallback chains | ✅ |
| 3 | 5+ char-by-char mode | ✅ |
| 3 | `attach_composition_searcher` API (back-compat with v2 path) | ✅ |
| 4 | `test_position_counter.cpp` (16 tests) | ✅ |
| 4 | `test_composition_searcher.cpp` (8 tests) | ✅ |
| 4 | `test_ime_v4_dispatch.cpp` (4 tests incl. real-dict smoke) | ✅ |
| 4 | All 123 PC tests green | ✅ |
| 5 | `mie_dict_loader.c` v4 magic auto-detection + load path | ✅ |
| 5 | `ime_task.cpp` conditional CompositionSearcher attach | ✅ |
| 5 | `scripts/build_and_flash.sh --v4` flag | ✅ |
| 5 | Hardware boot + RTT measurement (initial run) | ✅ |

## Quantified Results (RP2350 hardware, RTT trace)

```
                    v2 baseline    v4 result      improvement
─────────────────────────────────────────────────────────────
ime_proc p50        3.50 ms        6.71 ms        ~2x slower
ime_proc p90        233 ms         17.8 ms        13x faster
ime_proc max        391 ms         17.8 ms        22x faster
ime_proc kc=0x01    582 ms         17.6 ms        33x faster (worst case)

Pipeline TOTAL p50  202 ms         104 ms         49% reduction
Pipeline TOTAL p90  590 ms         130 ms         78% reduction
Pipeline TOTAL max  594 ms         227 ms         62% reduction

Dict size           4.9 MB         2.7 MB         45% reduction
Single-char cover   1,479          18,722         12.6x more chars
Multi-char words    78K            128,855        1.7x more words
```

## Production-validation campaign (2026-04-23)

End-to-end driven by SWD virtual key injection (no human at the keypad).
Three real-world passages were typed character by character — each char's
primary Bopomofo reading is converted to physical keycodes, injected
through the inject ring, and the engine's full 100-candidate buffer is
read back over SWD to find the target's rank. Pages auto-scroll
(`RIGHT × rank + OK`) and the committed `g_text` is verified to grow by
the expected UTF-8 byte count.

| Passage | Chars | rank-0 | top-8 | top-100 (reachable) | Committed |
|---|---|---|---|---|---|
| Lazy-Friday casual chat (441 chars) | 441 | 44.0 % | 93.7 % | **100 %** | 100 % |
| Echeneis fish (122 chars, mixed CJK + Latin + space) | 122 | 33.8 % | 88.3 % | 100 % CJK | 100 % CJK + 40/40 ASCII + 5/5 space |
| 鮣魚 wikipedia-style (369 chars, technical terms) | 369 | 36.9 % | 85.9 % | 99.7 % | 99.7 % |

The single-char miss across all three passages is **腭** (libchewing
freq = 1, effectively absent). Every other CJK target — including 鮣 /
隻 / 鰩 / 瓣 / 鋤 — is reachable within the engine's 100-candidate cap.

### Bugs found and fixed during the campaign

| # | Bug | Root cause | Fix |
|---|---|---|---|
| 1 | High-freq chars (科技, 機 …) ranked surprisingly low or absent in top-50 | `CompositionSearcher::search()` early-return on `collected >= max_results` — first 50 hits in iteration order locked the slate, later high-freq matches dropped | Top-K min-slot replacement: keep iterating, swap in any new match whose freq beats the current min |
| 2 | Same problem for single-char `search_chars` (e.g. 雞 missing from ㄐㄧ tone-1 list) | `pool_n < kPoolCap = 256` early stop on outer loop | Same min-slot pattern over the full iteration |
| 3 | Test runner saw garbage UTF-8 / wrong bytes in candidate words | Race read of LVGL-task struct while it was being rewritten | Lamport seq-lock (odd = write in progress, even = stable); host re-reads on odd |
| 4 | Snapshot fields contained random bytes after long sessions | 512-B debug struct overlapped the IPC counters at 0x2007FFC4+ | Trim to 400 B (ends at 0x2007FFC0); bumped magic to v0003 |
| 5 | `text_buf.endswith(target)` returned false even though commit succeeded | Debug buffer mirrors only the first 96 B of `g_text`; long passages overflow | Verify by `text_len` growth instead of substring match |
| 6 | LVGL hung / TLSF heap corruption when `CAND_MAX` raised to 100 | 100 pre-allocated lv_obj cells × ~440 B exhausted the 48 KB LVGL pool | `LV_MEM_SIZE` 48 → 56 KB; `CAND_MAX` 50 → 40 (rest reachable via engine, see below) |
| 7 | `Candidate tmp[kMaxCandidates]` blew the IME task's 8 KB stack at `kMaxCandidates=100` | 100 × 36 B local in `run_search_v4` adj loop | Moved to `static` (IME task is the only caller) |
| 8 | `cycle_view_to` sometimes ended on a random page | Two `static int s_active` symbols (i2c_bus.c + view_router.c) — `nm` lookup picked the I²C selector | Renamed view_router's variable to `s_view_router_active`; per-step `s_active` verification after every FUNC press |
| 9 | Pagination test scrolled to wrong cell on `rank > 31` | Inject ring batch (`RIGHT × rank + OK`) overwrote earlier slots when total events exceeded the 64-byte ring | `queue_bytes` chunks at `RING_SIZE-2 = 62` events, waits for consumer drain between chunks |
| 10 | Tone-1 chars whose reading ends in ㄓ / ㄚ (隻 / 之 / 八 / 家 / 發) missed top-100 | Test heuristic flagged byte 0x23 / 0x24 as "trailing tone byte" because slots 2/3 share phonemes (ㄓ/ˊ, ㄚ/˙); SPACE filter for tone-1 was skipped | Always trust the dict's `tone` field — append SPACE iff `tone == 1`, never guess from the byte |
| 11 | `count_positions` mis-classified `0x23` after a vowel (e.g. ㄆㄧㄥˊ → pos 2 instead of 1) | Slot 2 byte is both ㄓ (Initial) and ˊ (Tone 2) | `resolve_role(b, st)` — `0x23` is a tone marker iff state ∈ {kAfterM, kAfterF}; otherwise an Initial |
| 12 | Multi-bucket merge missed valid 4-char-bucket matches when position counter under-counted (ㄍㄓㄩㄓ → pos 3, missed 高瞻遠矚) | run_search only fell back to adjacent buckets when primary returned 0 hits | Always merge ±1 / ±2 buckets after primary; dedup by word string |

### Architectural follow-ups (still open)

| Item | Severity | Notes |
|---|---|---|
| Personalised LRU cache for recently-committed chars / words | **Done (Phase 1.6, 2026-04-23)** | Engine + flash persistence live. Fills the "repeat-typer" gap identified after Phase 1.4. See "Phase 1.6 delivered" section below. |
| Bucket 1 (`word_table` 1-char) is empty by design — chars come via `char_table`. Document or unify. | Low | Cosmetic; current behaviour is intentional. |
| 5+ phrase append after 1-char list pushes proverbs far down the candidate ranking | Low | Could promote 5+ phrase into top 10 if matched. |
| Legacy v2 (`TrieSearcher`) path cleanup once v4 is the only flashed format | Low | Keep until production-stable. |
| LVGL render + flush still ~70 ms p50 | Low | Pre-allocated cells + 16 ms refresh done; batch dirty rects next. |
| `ime_task` panic if dict load fails | Low | Currently panics with descriptive message; v2 fallback would help. |
| 多音字 reading_idx coverage — inferred only from same-syllable-count words | Med | ~15 K overrides recorded; spot-check gaps. |
| `LVGL CAND_MAX = 40` (down from desired 100) — rank 41-99 only reachable via engine state, not visible in pre-allocated cells | Low | Either bump LVGL pool again (current 56 KB) or paginate cells dynamically. |

### Decision: Phase 1.5 (Unihan / MoE dict expansion) — dropped (2026-04-24)

Earlier table listed P1.5 as adding ~3 657 Unihan kHanyuPinlu chars + MoE
成語典 + CC-CEDICT compound words. Post-Phase-1.4 validation (1 416 CJK
chars across 6 passages, 0 miss, top-100 100 %) shows this is **not
needed** and in fact counter-productive for three reasons:

1. **Bucket pressure already saturating.** The mid-range half-keyboard
   buckets (ㄧˋ = 215 chars, ㄒㄧ¹ = 158, …) are already dense enough
   that Phase 1.4 long-press cycling can't filter 35 % of synthetic
   rank > 100 targets into top-100. Adding 3 657 even-rarer Unihan
   chars enlarges the same buckets and **makes UX worse for everyone**.
2. **Real Unihan-only demand is vanishing.** None of the five user
   passages (casual / technical / classical-Chinese) hit a dict miss.
   The only documented miss (`腭`) would still be rank 100+ even after
   dict insertion because its freq=1 sinks below high-freq competitors.
3. **Cost doesn't match benefit.** Sub-B (MoE 成語典 + CC-CEDICT) adds
   4-char phrases but users type those character-by-character anyway —
   they don't know whether a compound is pre-stored. Measured: 0 of
   136 picker-punct + 0 of 48 spaces + 0 of 21 newlines lost.

**Phase 1.6 solves the real pain** (repeat-typer of the same rare char
within a session — "吸" hit rank 21 four times in one passage, "鮣" hit
rank 16 twice) with <5 KB runtime state and zero dict bloat.

### Phase 1.6 plan — Personalised LRU cache

**Goal.** When the user commits a candidate that did NOT land in the
top 8 on its first reading of a session, later identical inputs should
promote that commit to the top of the candidate list. Works across all
slot-byte buckets and across session boundaries (persisted to LittleFS).

**Scope.** Chars AND multi-char words. Any committed `Candidate` qualifies.

**Data model.**

    struct LruEntry {
        uint8_t  kbytes[8];     // reading keyseq, up to 8 bytes
        uint8_t  klen;          // active bytes in kbytes
        uint8_t  utf8_len;      // bytes in utf8[]
        char     utf8[24];      // committed word (same width as Candidate)
        uint32_t last_used;     // monotonic ms — eviction key
        uint16_t use_count;     // optional ranking weight
        uint8_t  pos_packed;    // phoneme-position hint (matches dict)
        uint8_t  tone;          // tone-1..5, 0 = unspecified
    };
    // sizeof(LruEntry) ≈ 48 B; at 128 entries ≈ 6 KB RAM + 6 KB flash.

**Lookup integration.** New stage in `ImeLogic::run_search_v4`:

    1. Consult LRU: find entries whose kbytes is a prefix of user_keys
       (and whose phoneme_pos matches any user_phoneme_hints ≠ ANY).
    2. Emit those at the FRONT of candidates_, tagged "recently used".
    3. Run the existing v4 search for the rest; dedup against LRU hits.

**Commit integration.** When `did_commit(utf8)` fires with the current
`key_seq_` + `phoneme_hint_`, upsert into the LRU:
  - If an entry with matching (kbytes, pos_packed, utf8) exists, bump
    `last_used` + `use_count`.
  - Otherwise evict the least-recently-used entry and insert.

**Persistence.** Serialise the whole table into a 6 KB LittleFS file
(`/mie/lru.bin`) on shutdown / periodic tick; reload on boot. Skip on
first boot or schema-mismatch. LFS writes are expensive (P2-11 flash-
safety wrap), so throttle to once per N commits + once per session end.

**Size budget.** 128 entries × 48 B = 6 KB RAM (static, not heap) + 6 KB
LittleFS footprint. Fits well inside Core 1's remaining 14 KB free heap.

**Risk items.**
- Entries whose kbytes are short prefixes of common sequences could
  dominate ranking (e.g. a single-ㄅ entry prepending to every ㄅ-word).
  Mitigation: only surface when the user's key_seq_ EXACTLY matches the
  entry's kbytes, OR when kbytes is a prefix AND user's length >= entry
  klen.
- LittleFS wear from frequent writes. Mitigation: commit-count throttle
  (write after every 50 commits) + opportunistic write on mode cycle.

**Test plan.**
- Host unit tests for LRU data structure (upsert, evict, prefix lookup).
- Hardware regression: re-run the five user passages; expect the second-
  occurrence of "吸" / "鮣" to land at rank 0–3 instead of 16–21.
- Power-cycle test: persist → reboot → verify cache restored.

### Phase 1.6 delivered (2026-04-23)

Three commits on branch `dev-Sblzm`:

- `9d05ae1` **Step 1** — `firmware/mie/{include/mie,src}/lru_cache.{h,cpp}`
  + 16 GoogleTest cases. Heap-free 64-entry LruCache (kCap 128 → 64 at
  Step 3 to fit Core 1 .bss budget), 48 B per entry, `LRU1` serialise
  format with magic / version / count validation.
- `3c4c6c8` **Step 2** — wire into `ImeLogic`: `prepend_lru_candidates()`
  splices LRU hits to rank 0 in SmartZh search (per-entry `klen`
  threaded through `candidates_prefix_keys_` so prefix hits partial-
  commit correctly), `commit_partial` upserts `(matched-prefix,
  packed-hint, utf8, tone)` before the key_seq_ strip, public
  `load_lru` / `serialize_lru` API for the Core 1 bridge. 5 new
  integration tests against an in-memory MIE4 blob (171/171 suite).
- `c4dd34c` **Step 3** — Core 1 flash persistence + symmetric P2-11
  park. New partition at `0x10C00000` (64 KB reserved, 8 KB active
  slot). `flash_safety_wrap.c` on Core 1 wraps `flash_range_erase /
  program` to park Core 0 before XIP toggles off; Core 0's new
  `flash_park_listener.c` (in the Meshtastic variant dir) claims
  `SIO_IRQ_BELL` to ack park requests. Shared-SRAM `flash_lock_c0`
  state word + `IPC_FLASH_DOORBELL_C0` doorbell bit are the
  independent reverse-direction lock. Save throttled by
  `ime_task.cpp`: 50 commits / MODE cycle / 30 s idle. Scratch is
  static .bss (3328 B) to sidestep heap fragmentation that otherwise
  starves the 6.4 KB request hours into runtime.

Hardware checks (SWD):
- `0x10C00000` reads `0x3155524C` ("LRU1") + version 1 after first
  save; 0xFF tail remains (empty blob).
- Reset cycles preserve the header — persist path reaches flash.
- MODE inject triggers the mode_tripwire → flash save completes
  without HardFault on either core.

Hardware regression — `scripts/test_lru_regression.py` across the
reference passages (erased partition, full passages, cold → warm
within one boot session; user1 also validated `--reboot` round-trip).
Two passes are reported: cold (Pass 1, post-erase) and warm (Pass 2
on the same passage in the same boot). 2026-04-27 re-run on the
**rewritten fictional content** with **kCap = 128** (Phase 1.6.1).

| Passage     | Chars | Pass 1 rank-0 | Pass 2 rank-0 | Δrank-0 | Δrank≥8 | Commit | Accept |
|-------------|-------|---------------|---------------|---------|---------|--------|--------|
| user1-30 †  |  24   |  5  (21 %)    | 23  (96 %)    | **+18** | n/a     | 100 %  | PASS   |
| user1 †     | 217   | 110 (51 %)    | 122 (56 %)    |  +12    | n/a     | 100 %  | PASS   |
| user2       | 218   | 109 (50.0 %)  | 118 (54.1 %)  |  +9     |  -3     | 100 %  | FAIL*  |
| user3       | 246+48| 132 (53.7 %)  | 138 (56.1 %)  |  +6     |  -4     | 100 %  | FAIL*  |
| user4       | 259   | 141 (54.4 %)  | 146 (56.4 %)  |  +5     |  -1     | 100 %  | FAIL*  |
| user5       | 162   |  68 (42.0 %)  | 115 (71.0 %)  | **+47** | -25     | 100 %  | PASS   |
| lazy_friday | 327   | 181 (55.4 %)  | 184 (56.3 %)  |  +3     |  -2     | 100 %  | FAIL*  |
| echeneis †  |  77   |  46 (60 %)    | 69  (90 %)    | **+23** | n/a     | 100 %  | PASS   |

†2026-04-22 numbers, content unchanged on disk so kCap = 128 is
expected to be at least as strong; not re-captured this round.

*FAIL = script's three-part acceptance (rank≤3 must not regress; ≥10
chars must promote out of rank≥8 tail) not met — not a crash or
persistence failure.

Cross-reboot persistence (user1 30-char, `--erase --reboot --limit 30`):
Pass 1 rank-0 = 5 → Pass 2 rank-0 = 23, delta +18. Flash `LRU1` magic
survives the SYSRESETREQ and Core 1 relaunch, cache re-hydrates
without any warm pass since power-on.

#### 2026-04-27 single-pass dual-mode snapshot (`--precise-hints` vs default HINT_ANY)

Hardware re-run on the rewritten fictional passages. `--precise-hints`
forces every Bopomofo press to use the dict-encoded long-press
position (engine-internal regression mode — tighter candidate filter,
higher rank-0, but reports false MISSes against the real-UX path).
Default HINT_ANY short-taps every byte and matches what a user types
on the half-keyboard. Trade-off: precise-hints is the right yardstick
for engine-side ranking changes; HINT_ANY is the right yardstick for
end-user UX. Pass = `ime_text_test.py PASSAGE` warm-LRU baseline.

| Passage     | Chars (CJK + ASCII) | Mode      | rank-0 | top-8 | commit |
|-------------|---------------------|-----------|--------|-------|--------|
| user2       | 218                 | precise   | 68.8 % | 97.2 %| 100 %  |
| user3       | 246 + 48            | precise   | 70.3 % | 97.2 %| 99.2 % |
| user4       | 259                 | precise   | 69.9 % | 96.9 %| 98.8 % |
| user5       | 162                 | precise   | 51.9 % | 92.0 %| 98.8 % |
| user5       | 162                 | HINT_ANY† | 71.0 % | 100 % | 100 %  |
| t9_stress   | 69 + 519            | precise   | 71.0 % | 100 % | 100 %  |
| lazy_friday | 327                 | HINT_ANY  | 56.9 % | 94.2 %| 100 %  |

†`user5` HINT_ANY captured *after* the dict-regen change that flipped
the default from `--precise-hints` to HINT_ANY (Open Follow-ups #17 /
2026-04-26 dict rebuild) on a warm LRU — same passage where precise
mode reports the lowest rank-0 of the set. The 51.9 % → 71.0 % gap
is the canonical illustration that the precise-hints filter under-
counts user-reachable candidates whenever the dict's encoded long-
press position differs from what the user actually presses; it does
**not** mean the engine got better between runs.

`lazy_friday` was flagged as a picker rank-7 commit cascade in an
earlier draft of this table; the failure does not reproduce on
2026-04-27 (327/327 commit ✓ across one warm and one cold/warm
two-pass run). Bullet retired in `phase2-log.md` Open Follow-ups.

Shape of the data (post-Phase 1.6.1, kCap = 128): user5 jumps from
+0 (kCap = 64 baseline) to **+47** rank-0 — the canonical "kCap was
the bottleneck" passage. user2 / user3 / user4 lift from +1 / +2 / +3
to +9 / +6 / +5 — modest but no longer flat. lazy_friday remains the
hardest case (+3 rank-0 lift on 327 chars), consistent with the long-
passage profile that Phase 1.6.1's plan flagged as still bound by
recency. Short / repetitive passages (user1-30, echeneis) keep their
strong +18 / +23 lift. Every passage now lands 100 % commit on both
passes, and rank≤3 never regresses across cold→warm.

The lingering FAIL* flags are entirely the script's "≥10 chars
promoted out of the rank≥8 tail" gate, not commit or rank≤3
regressions; for user2..4 the cold-pass tail is only 12–21 chars to
begin with, so a tail of 10–18 after warming caps the achievable
promotion well below 10 even when the cache is doing useful work.
Consider relaxing the gate, or making it scale with the cold-pass
tail size, in a future test-tooling pass.

Follow-up work (2026-04-23 / 24):
- `--reboot` round-trip fix: `force_lru_save()` was resetting
  `producer_idx` to 2, which wrapped the key-inject ring past its
  (stale-from-Pass-1) consumer position and re-replayed ~30 garbage
  events into IME, crashing Core 1 mid-flash-write. Commit `3af07f6`
  appends events at `cur_prod & (RING-1)` and bumps `producer_idx`
  by +2 instead — confirmed with live SWD reproduction.
- `build/mie-host/dict.bin` 89 MB regeneration fixed by CMake change
  to route `mie_dict_data_lg` to `${MIE_DATA_DIR}/lg/` so it no
  longer clobbers the `sm` variant outputs (commit `f63f41b`).
- **RTT key-inject transport** added as an alternative to the SWD
  ring. `firmware/core1/src/keypad/key_inject_rtt.{c,h}` +
  `scripts/mokya_rtt.py` + `ime_text_test.py --transport rtt`.
  Both transports coexist under a shared `g_key_inject_mode` byte
  (default SWD) — whichever isn't selected long-sleeps 50 ms so
  ime_task never competes with two hot pollers. Host flips the
  byte for the duration of an RTT burst and flips it back on
  context-manager exit. Commits `7e2443a` / `1a21ee9`. Net effect:
  a binary-framed, ring-wrap-safe second transport with ~35 %
  higher raw throughput at the SWD level but identical per-char
  latency under regression (IME processing dominates), whose main
  value is safety (no producer/consumer competition, CRC-protected
  frames, resync on magic byte).
- **Poll refactor** (commit `eb182a3`) replaced the 10 ms full-
  snapshot polls in `ime_text_test.py` wait loops with 3 ms
  single-u32 polls via a new `ImeDriver.wait_until(reader,
  condition, ...)` helper. Per-char steady-state 400 → 310 ms.
- **LFU-weighted LRU eviction** (commit `01af1b9`) was a hypothesis
  test for the long-passage flat-lift problem. Hardware result
  was a null — see `docs/design-notes/mie-p1.6-lru-plan.md`
  "What we learned about long passages". Kept in tree as a unit
  test + honest commit message for future P1.6.1 revisits.
- **`build_and_flash.sh --core1` partition preservation fix**
  (2026-04-24). `--core1` was re-flashing `build/mie-host/dict.bin`
  (MDBL v2) over whatever was already on the board — including any
  `--v4` MIE4 blob the user had flashed earlier. Symptom was IME
  timing out after a "quick Core 1 flash", because v2 dict has
  different candidate rankings than v4. Tightened `--core1` so it
  now flashes *only* the Core 1 image at 0x10200000; callers must
  combine with `--dict`, `--font`, or `--v4` to refresh those
  partitions deliberately.
- Simultaneous flash writes from both cores is still an uncovered
  race (low probability, both sides honour a 5 ms park timeout).
  Document as follow-up — add a global `flash_op_arb` CAS if it
  ever bites.

### Process improvements

- **SWD virtual key injection** is now a first-class capability:
  `firmware/core1/src/keypad/key_inject.{c,h}` exposes `g_key_inject_buf`,
  a 64-event ring polled by a dedicated FreeRTOS task that forwards into
  `key_event_push_inject()`. The host writes events with `pylink`
  (`scripts/mokya_swd.py`) — persistent SWD connection, ~0.3 ms per
  memory op vs the 500 ms launch cost of one-shot `J-Link Commander`
  scripts. Used for end-to-end IME testing without keypad operator.
- **RTT key-inject transport** (Phase 1.6 followup): alternate path
  at `firmware/core1/src/keypad/key_inject_rtt.{c,h}` using SEGGER
  RTT down-channel 1 with a binary wire frame (magic + type + len +
  payload + crc8; see `key_inject_frame.h`). Same downstream path
  (`key_event_push_inject_flags`) as the SWD ring. `g_key_inject_mode`
  arbitrates — only one transport polls at a time. Host side is
  `MokyaSwd.rtt_send_frame` (memory-write into the SEGGER CB;
  bypasses pylink's `rtt_write` which is unreliable on RP2350) plus
  `ime_text_test.py --transport rtt`.
- **Text-driven IME regression** lives in `scripts/ime_text_test.py` —
  takes a UTF-8 passage, types it char-by-char (CJK via SmartZh, ASCII
  via Direct mode, space directly), reads back `g_text`, reports per-
  rank histogram + reachability stats. Three reference passages under
  `scripts/ime_passage_*.txt`. `--transport {swd,rtt}` picks the
  injection transport; `wait_until(cheap_read, condition, poll_s=3)`
  replaced the 10 ms + 400 B-snapshot polls that used to dominate
  wait-loop cost (per-char 400 → 310 ms post-refactor).
- SWD+RTT debug methodology was promoted to default in `CLAUDE.md`'s
  Hardware Debug section earlier this week. RTT trace is the first
  tool to reach for on Core 1 latency / hang investigations.

## Critical files (reference)

### Build pipeline
- `firmware/mie/tools/gen_dict.py` — v2 builder (current production) + v4 builder (`--v4-output` flag)
- `scripts/poc_mie_v4_simulation.py` — Python reference of v4 architecture
- `scripts/verify_mied_v4.py` — v4 binary round-trip verifier
- `scripts/build_and_flash.sh` — supports `--v4` flag to flash MIE4 dict

### MIE library (host + Core 1)
- `firmware/mie/include/mie/composition_searcher.h`
- `firmware/mie/src/composition_searcher.cpp`
- `firmware/mie/include/mie/ime_logic.h` — `attach_composition_searcher`
- `firmware/mie/src/ime_logic.cpp` — attach + position counter helpers
- `firmware/mie/src/ime_keys.cpp` — `count_positions` + `first_n_positions_bytes`
- `firmware/mie/src/ime_search.cpp` — `run_search` dispatch + `run_search_v4`

### Core 1 firmware
- `firmware/core1/src/ime/mie_dict_loader.h/c` — MIE4 magic detection + v4_blob load
- `firmware/core1/src/ime/mie_dict_partition.h` — `MIE_MIE4_MAGIC` constant
- `firmware/core1/src/ime/ime_task.cpp` — `g_v4_searcher` + conditional attach
- `firmware/core1/m1_bridge/CMakeLists.txt` — `composition_searcher.cpp` in build

### Tests
- `firmware/mie/tests/test_composition_searcher.cpp` (8)
- `firmware/mie/tests/test_position_counter.cpp` (16)
- `firmware/mie/tests/test_ime_v4_dispatch.cpp` (4 incl. real-dict smoke)

### Trace infrastructure (cross-cutting)
- `firmware/core1/src/debug/mokya_trace.h` — RTT TRACE / TRACE_BARE macros
- `firmware/core1/src/debug/mokya_trace.c` — vsnprintf → SEGGER_RTT_Write
- `scripts/analyze_rtt_latency.py` — pipeline latency analyzer

### SWD/RTT virtual key injection + text-driven IME tests
- `firmware/core1/src/keypad/key_inject.{h,c}` — 64-event ring polled by
  a FreeRTOS task (5 ms cadence), forwards into `key_event_push_inject`.
  `g_key_inject_mode` byte arbitrates SWD vs RTT — active task runs at
  poll cadence, inactive long-sleeps 50 ms.
- `firmware/core1/src/keypad/key_inject_rtt.{h,c}` — RTT transport
  counterpart; SEGGER down-channel 1 (buffer `"keyinj"`, 256 B),
  uses `key_inject_frame.h` parser. Task stack 256 words (TRACE()
  vsnprintf needs ~128 B inside).
- `firmware/core1/src/keypad/key_inject_frame.{h,c}` — shared binary
  wire frame (magic + type + len + payload + crc8) + re-sync parser
  state machine. Reused by future USB-CDC transport.
- `firmware/core1/src/ui/ime_view.c` — `ime_view_debug_t` v0003 (seq-
  locked snapshot of cand/text/pending/mode) + `g_ime_cand_full` (full
  100-candidate mirror in regular .bss for SWD readback beyond the 8-cell
  on-screen window)
- `scripts/mokya_swd.py` — pylink-based persistent SWD connection,
  ~0.3 ms per memory op + transient-error retry. Adds `rtt_send_frame`,
  `rtt_ring_empty`, `set_key_inject_mode` for the RTT path.
- `scripts/mokya_rtt.py` — standalone RTT client + `_selftest()` for
  bring-up verification without invoking ime_text_test.
- `scripts/inject_keys.py` — CLI for injection + view cycling + reset
- `scripts/ime_text_test.py` — text passage → per-char Bopomofo / Direct
  / mode-switching pipeline; verifies via `g_text` byte growth.
  `--transport {swd,rtt}` picks the injection transport;
  `ImeDriver.wait_until` polls cheap u32 fields rather than 400 B
  snapshots.
- `scripts/test_lru_regression.py` — Pass 1 / Pass 2 rank-histogram
  regression around Phase 1.6 LRU. `--erase` wipes flash LRU slot,
  `--reboot` exercises cross-reset persistence. `force_lru_save()`
  uses safe `(cur_prod + 2)` ring advancement (the ring-wrap bug
  that wedged QMI in the 2026-04-23 session).
- `scripts/ime_passage_*.txt` — reference passages spanning
  encyclopedic content (Echeneis-family), rare-char stress, household
  prose, embedded-systems prose, classical-Chinese style, café-scene
  narrative, and an English/Chinese mixed engineering passage; see
  Phase 1.6 regression table above. (The user2..5 / lazy_friday /
  t9_stress files were rewritten as fully fictional, neutral text
  in 2026-04-26 — earlier baseline numbers in the regression table
  were captured against the previous content of those filenames and
  should be re-measured against the new text before comparing.)

## Recovery (if v4 is broken)

If a v4 flash leaves Core 1 unbootable, roll back to v2 by re-flashing the
MDBL pack:

```sh
bash scripts/build_and_flash.sh --core1
# (no --v4 flag → MDBL v2 dict gets repackaged + flashed)
```

The auto-detection in `mie_dict_loader.c` chooses v2 when the partition holds
MDBL magic and v4 when it holds MIE4. Same Core 1 firmware works with either.
