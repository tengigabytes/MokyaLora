# MIE v4 Composition Architecture — Status

Last updated: 2026-04-23

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

## Completed (Phase 0-5)

| Phase | Item | Status |
|---|---|---|
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
| `腭` (and ~600 similar Unihan-only chars) absent from libchewing tsi | High for completeness | Phase 1.5: integrate Unihan kHanyuPinlu (~3 657 fillable chars) |
| Compound-word coverage (跑步機 / 線上會議 / 行銷策略 / etc.) | Med | Phase 1.5: MoE 成語典 + CC-CEDICT modern compound lift |
| Bucket 1 (`word_table` 1-char) is empty by design — chars come via `char_table`. Document or unify. | Low | Cosmetic; current behaviour is intentional. |
| 5+ phrase append after 1-char list pushes proverbs far down the candidate ranking | Low | Could promote 5+ phrase into top 10 if matched. |
| Legacy v2 (`TrieSearcher`) path cleanup once v4 is the only flashed format | Low | Keep until production-stable. |
| LVGL render + flush still ~70 ms p50 | Low | Pre-allocated cells + 16 ms refresh done; batch dirty rects next. |
| `ime_task` panic if dict load fails | Low | Currently panics with descriptive message; v2 fallback would help. |
| 多音字 reading_idx coverage — inferred only from same-syllable-count words | Med | ~15 K overrides recorded; spot-check gaps. |
| `LVGL CAND_MAX = 40` (down from desired 100) — rank 41-99 only reachable via engine state, not visible in pre-allocated cells | Low | Either bump LVGL pool again (current 56 KB) or paginate cells dynamically. |

### Process improvements

- **SWD virtual key injection** is now a first-class capability:
  `firmware/core1/src/keypad/key_inject.{c,h}` exposes `g_key_inject_buf`,
  a 64-event ring polled by a dedicated FreeRTOS task that forwards into
  `key_event_push_inject()`. The host writes events with `pylink`
  (`scripts/mokya_swd.py`) — persistent SWD connection, ~0.3 ms per
  memory op vs the 500 ms launch cost of one-shot `J-Link Commander`
  scripts. Used for end-to-end IME testing without keypad operator.
- **Text-driven IME regression** lives in `scripts/ime_text_test.py` —
  takes a UTF-8 passage, types it char-by-char (CJK via SmartZh, ASCII
  via Direct mode, space directly), reads back `g_text`, reports per-
  rank histogram + reachability stats. Three reference passages under
  `scripts/ime_passage_*.txt`.
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

### SWD virtual key injection + text-driven IME tests
- `firmware/core1/src/keypad/key_inject.{h,c}` — 64-event ring polled by
  a FreeRTOS task (5 ms cadence), forwards into `key_event_push_inject`
- `firmware/core1/src/ui/ime_view.c` — `ime_view_debug_t` v0003 (seq-
  locked snapshot of cand/text/pending/mode) + `g_ime_cand_full` (full
  100-candidate mirror in regular .bss for SWD readback beyond the 8-cell
  on-screen window)
- `scripts/mokya_swd.py` — pylink-based persistent SWD connection,
  ~0.3 ms per memory op + transient-error retry
- `scripts/inject_keys.py` — CLI for injection + view cycling + reset
- `scripts/ime_text_test.py` — text passage → per-char Bopomofo / Direct
  / mode-switching pipeline; verifies via `g_text` byte growth
- `scripts/ime_passage_*.txt` — three reference passages (chat / fish /
  technical wiki)

## Recovery (if v4 is broken)

If a v4 flash leaves Core 1 unbootable, roll back to v2 by re-flashing the
MDBL pack:

```sh
bash scripts/build_and_flash.sh --core1
# (no --v4 flag → MDBL v2 dict gets repackaged + flashed)
```

The auto-detection in `mie_dict_loader.c` chooses v2 when the partition holds
MDBL magic and v4 when it holds MIE4. Same Core 1 firmware works with either.
