# Core 1 Memory Budget

Single source of truth for RAM allocations on the Core 1 application image
(`m1_bridge`, loaded at `0x10200000`). Any new task, queue, or large static
buffer MUST be added to the appropriate table below **before** the code is
merged. The build-time log line (see §5) cross-checks this at boot.

Rationale: M3.4.5d uncovered the pattern of growing `configTOTAL_HEAP_SIZE`
reactively after each new task HardFaulted on `vTaskStartScheduler`'s
IDLE-task allocation. Planning upfront is cheaper than SWD archaeology.

---

## 1. RP2350B SRAM

Physical SRAM on RP2350B is **520 KB** at `0x20000000 – 0x20082000`.

| Region | Size | Owner | Notes |
|---|---|---|---|
| Shared IPC window | 24 KB | Core 0 ↔ Core 1 | Fixed, reserved at link time — see firmware-architecture.md §5. |
| Core 1 .text/.rodata/.data/.bss | ~300 KB | m1_bridge image | Driver/LVGL/FreeRTOS code + statics. |
| FreeRTOS heap (heap_4) | **56 KB** | `configTOTAL_HEAP_SIZE` | Task stacks, TCBs, kernel objects. See §3. Bumped from 48 → 56 KB on 2026-04-27 after PSRAM relocation freed 15 KB of MSP-guard slack. **Measured `g_core1_boot_heap_free = 15,488 B (27.0 %)`** at boot, unchanged after 60-char IME stress (heap settles at the per-task allocation baseline). |
| LVGL heap | **56 KB** | `LV_MEM_SIZE` in `lv_conf.h` | Separate from FreeRTOS. Widgets, styles, fonts, draw buffers. **Post 2026-04-27 view-system refactor (lazy create + LRU cache)**: pool usage decoupled from total view count. `view_router_init(screen, lru_capacity=3)` keeps at most 1 active + 3 cached widget trees alive; cycling past triggers `destroy()` on the oldest. **Measured after 60-char IME stress** (RTT `lvgl_mem,stats`): `total=51128, max_used=45420, free=6360, used_pct=88 %, frag_pct=1 %` — modest in-test reduction from the pre-refactor 47.5 KB peak (test only touches keypad + ime). The architectural win is for production: adding the planned 30+ views grows the registry table only, not the LVGL pool. Each view now defines `static const view_descriptor_t XXX_DESC` with `create()` / `destroy()` callbacks and registers via `g_view_registry[]` in `view_registry.c`. |
| MCU main stack (MSP) | ~9 KB de facto (post 2026-04-27 heap bump) | linker `__StackTop` | Pre-scheduler + exception handlers. Effective MSP region = the slot between `__end__` and `__StackTop = 0x2007A000`. **Trajectory:** 2 KB (original) → 17 KB (after PSRAM relocation) → **9 KB** after `configTOTAL_HEAP_SIZE` 48 → 56 KB which absorbed 8 KB of the MSP slack. Linker assertion `__StackTop - __end__ >= 0x800` (in `memmap_core1_bridge.ld`) enforces a hard 2 KB minimum at link-time. Tracked at runtime by `msp_canary` (`firmware/core1/src/debug/msp_canary.{c,h}`): boot fills the slot with `0xDEADBEEF`, `wd_task` refreshes peak every 200 ms into `g_msp_peak_used` / `g_msp_low_water_addr`. **Measured 2026-04-27** under USB+IPC + 60-char IME stress: peak = **436 B (~5 % of available)** — comfortable ~20× headroom. |
| LVGL framebuffer | 150 KB | `s_framebuffer` in `display/lvgl_glue.c`, dedicated `.framebuffer` NOLOAD section | DIRECT mode 240×320 RGB565. Largest single SRAM consumer (~48 % of Core 1 carve-out). |
| Slack | — | — | Whatever's left. Goal: keep ≥ 32 KB unused for growth. |

### 1.1 PSRAM .psram_bss region (added 2026-04-27)

PSRAM (APS6404L, 8 MB at `0x11000000`) is initialised by `psram_init()` in
`main()` line 573 — runs before any task starts. The linker carves
**1 MB at `0x11700000`** (offset 7 MB) into `PSRAM_BSS` (see
`memmap_core1_bridge.ld`); buffers opt in via
`__attribute__((section(".psram_bss")))`. Crt0 does NOT zero this
region, so `main()` clears it manually right after `psram_init()`
returns. **Placement at 7 MB offset is intentional** — the MIE dict
loader writes the v4 blob (up to ~5 MB) starting at offset 0; an
earlier draft placed `.psram_bss` at `0x11000000` and the dict copy
silently overwrote `lru_tmp` / `tmp` etc, manifesting as flaky search
results on long passages.

Currently relocated to `.psram_bss` (~15 KB total, 2026-04-27):

| Symbol | Size | Why PSRAM is OK |
|---|---:|---|
| `mie::ImeLogic::run_search_v4::tmp` | 3.6 KB | Bursty (per-keystroke), data only consumed by Core 1 itself, fully cached. |
| `mie::ImeLogic::prepend_lru_candidates::lru_tmp` | 3.6 KB | Same. |
| `s_text_buf` (ime_view) | 2.0 KB | Snapshot copy, read sequentially during render — cached PSRAM is fast for this. |
| `s_combined` (ime_view) | 2.2 KB | Render scratch, single-writer single-reader. |
| `s_cand_buf` (ime_view) | 1.9 KB | Candidate cell text snapshot. |
| `s_last_cell_text` (ime_view) | 1.9 KB | Per-cell dirty-tracking cache. |

**Cache-coherence caveat — DO NOT relocate SWD-readable buffers.**
PSRAM at `0x11000000` is **WRITE-BACK cached** by the RP2350 XIP cache.
SWD memory accesses through AHB-AP bypass the L1 cache, so dirty
cache lines are invisible to a SWD reader. `g_ime_cand_full` was
moved to PSRAM in a first attempt and immediately broke
`scripts/ime_text_test.py` with "cand_full seq unstable" — its
seq-lock invariant requires the SWD reader to see writes in order.
Anything inspected over SWD MUST stay in regular `.bss`, OR write
via the uncached alias `0x15xxxxxx`, OR call
`xip_cache_clean_range()` after each write.

`scripts/build_and_flash.sh` prints the section sizes after each Core 1
link; compare against this table when the numbers drift.

---

## 2. Design rules

1. **Upfront budget.** Adding a new task or any `pvPortMalloc`-caller
   (queue, semaphore, event group, timer) requires updating §3 **in the
   same commit**. No silent growth.
2. **No silent failures.** Every `xTaskCreate` return value and every
   `*_task_start` boolean MUST be checked. Main aborts with a clear log
   line if anything fails — see `main_core1_bridge.c`. No more
   `(void)rc_xxx;`.
3. **Stack sizing method.** Start from an empirical peak:
   - enable `configCHECK_FOR_STACK_OVERFLOW = 2` (already set)
   - run the task under its worst-case path for at least 10 minutes
   - read `uxTaskGetStackHighWaterMark()` via SWD or a runtime CLI
   - round up to high-water + 25 % (not +1 %).
4. **Prefer static allocation for kernel objects.** FreeRTOS supports
   `xQueueCreateStatic` / `xSemaphoreCreateMutexStatic`. We currently use
   dynamic allocation because `configSUPPORT_STATIC_ALLOCATION = 0`.
   Flip only when we actually need to diagnose heap fragmentation.
5. **LVGL heap stays separate.** Widgets, styles, lv_malloc — all from
   `LV_MEM_SIZE`. Never split widgets across FreeRTOS heap.
6. **Reserve ≥ 15 % of `configTOTAL_HEAP_SIZE`** at all times. With the
   current 56 KB heap (post 2026-04-27 bump), that is ≥ 8,602 B free
   after all tasks are running. Currently at 27 % (15.5 KB free).
   Original 2024-04 wording (kept below for historical context) was
   based on the 48 KB heap and the 14 % crisis threshold; both are
   now superseded.
   current 48 KB heap, that is ≥ 7.2 KB free after all tasks are
   running. The boot-time log prints the figure (and stashes it in
   `g_core1_boot_heap_free` for SWD readback) — `main` panics if we
   cross it. **Threshold was loosened from 20 % to 15 % on 2026-04-24**
   when the Phase 1.6 RTT key-inject task (1 KB TCB/stack) pushed free
   from 9.6 KB down to ~9 KB; see `phase2-log.md` "P1.6 follow-up"
   entry. The original 20 % target was a forward-looking budget for
   "future driver growth", not a hard requirement — 15 % still leaves
   1.6 KB buffer above transient FreeRTOS needs.

---

## 3. FreeRTOS heap budget (`configTOTAL_HEAP_SIZE = 48 KB`)

Heap is consumed by task stacks (4 bytes per word), TCBs (~88 B each on
32-bit V11), and kernel objects (queues/mutexes/semaphores ~80-100 B
each + their storage). `heap_4` adds ~16 B overhead per allocation.

### 3.1 Application tasks

| Task name | File | Stack (words) | Stack (B) | Priority | Purpose |
|---|---|---:|---:|---|---|
| `usb` | `main_core1_bridge.c` | 1024 | 4096 | tskIDLE+2 | TinyUSB device; `tud_task()` + taskYIELD loop. |
| `bridge` | `main_core1_bridge.c` | 1024 | 4096 | tskIDLE+2 | Core 0 ↔ Core 1 IPC pump; HW-FIFO doorbell listener. |
| `lvgl` | `src/display/lvgl_glue.c` | 3072 | 12288 | tskIDLE+2 | `lv_timer_handler()` + draw buffer flush; largest stack because LVGL draw paths nest deeply. |
| `kpad` | `src/keypad/keypad_scan.c` | 512 | 2048 | tskIDLE+2 | PIO+DMA keypad scan, 20 ms debounce, enqueue `key_event_t`. |
| `chg` | `src/power/bq25622.c` | 512 | 2048 | tskIDLE+2 | 1 Hz charger poll + watchdog kick. |
| `sens` | `src/sensor/sensor_task.c` | 512 | 2048 | tskIDLE+2 | Shared tick for LPS22HH / LIS2MDL / LSM6DSV16X. |
| `gps` | `src/sensor/gps_task.c` | 512 | 2048 | tskIDLE+2 | Teseo-LIV3FL NMEA drain + parser. |
| `ime` | `src/ime/ime_task.cpp` | 1024 | 4096 | tskIDLE+3 | Drain `key_event_t` queue → `ImeLogic::process_key`; 20 ms `tick()` for multi-tap / long-press; owns the `ImeLogic` instance + both `TrieSearcher` instances as statics (~3 KB, see §4). Primary consumer of the KeyEvent queue (FA §4.4); the listener callbacks are re-entrancy-free. |
| `key_inject` | `src/keypad/key_inject.c` | 256 | 1024 | tskIDLE+2 | Drain `g_key_inject_buf` ring (SWD transport) → `key_event_push_inject_flags`. Skip-loop when `g_key_inject_mode != SWD`. |
| `key_inj_rtt` | `src/keypad/key_inject_rtt.c` | 256 | 1024 | tskIDLE+2 | Drain SEGGER RTT down-channel 1 → parse binary wire frame → `key_event_push_inject_flags`. Skip-loop when `g_key_inject_mode != RTT`. Stack sized for TRACE()'s 128 B vsnprintf scratch. |
| `wd` | `src/power/watchdog_task.c` | 192 | 768 | tskIDLE+3 | HW watchdog kicker. Polls `g_ipc_shared.c0_heartbeat` every 200 ms; kicks on advance, stops kicking after 4 s of silence so the 3 s HW watchdog resets the chip. Honours `wd_pause` (atomic counter — flash_safety_wrap, Power::reboot wrap their long ops). 192 words is plenty: only atomic loads, integer math, register writes — no printf or callbacks. |
| **App subtotal** |  | | **35,712 B (34.9 KB)** | | |

### 3.2 FreeRTOS internal tasks

| Task | Stack (words) | Stack (B) | Source |
|---|---:|---:|---|
| `IDLE` | 512 (`configMINIMAL_STACK_SIZE`) | 2048 | `tasks.c` creates during `vTaskStartScheduler`. |
| `Tmr Svc` | 512 (`configTIMER_TASK_STACK_DEPTH`) | 2048 | `timers.c`, enabled by `configUSE_TIMERS = 1`. |
| **Kernel subtotal** | | **4096 B (4.0 KB)** | |

### 3.3 Queues, semaphores, misc

| Object | File | Storage | Overhead | Total |
|---|---|---:|---:|---:|
| `i2c_bus_mutex` (mutex) | `src/i2c/i2c_bus.c` | — | ~80 B | ~80 B |
| `KeyEvent queue` (16 × 2 B) | `src/keypad/key_event.c` | 32 B | ~80 B | ~112 B |
| `KeyEvent view queue` (16 × 2 B) | `src/keypad/key_event.c` | 32 B | ~80 B | ~112 B |
| `ime snapshot mutex` | `src/ime/ime_task.cpp` | — | ~80 B | ~80 B |
| Timer queue (10 × ~12 B) | `timers.c` | ~120 B | ~80 B | ~200 B |
| TCBs (9 tasks × ~88 B) | `tasks.c` | — | — | ~792 B |
| Heap_4 per-alloc overhead | `heap_4.c` | — | ~16 B × ~15 blocks | ~240 B |
| `settings reply queue` (8 × ~70 B) | `src/settings/settings_client.c` | ~560 B | ~80 B | ~640 B |
| **Misc subtotal** | | | | **~2.2 KB** |

### 3.4 Totals

| | Bytes (pre-IME) | Post-M4 IME | Post-P1.6 follow-up (RTT) |
|---|---:|---:|---:|
| §3.1 App tasks | 28,864 | 32,960 | 34,944 |
| §3.2 Kernel tasks | 4,096 | 4,096 | 4,096 |
| §3.3 Queues / TCBs / overhead | ~1,420 | ~1,700 | ~1,900 |
| **Estimated** | **~34.4 KB** | **~38.8 KB** | **~40.9 KB** |
| `configTOTAL_HEAP_SIZE` | 49,152 | 49,152 | 57,344 (post 2026-04-27 bump) |
| **Reserve (threshold 15 %)** | **~14.7 KB (30 %)** ✓ | **~10.5 KB (21 %)** ✓ | **~15.5 KB (27 %)** ✓ |

**Measured 2026-04-27 post `configTOTAL_HEAP_SIZE` 48 → 56 KB bump (SWD, `g_core1_boot_heap_free`)**:
`xFreeBytesRemaining = 15,488 B (27.0 %)` — back to a healthy reserve
after the PSRAM relocation (commit `46a7821`) freed enough MSP-guard
slack to absorb 8 KB of `ucHeap` growth. Threshold restored to **15 %**.
MSP guard region remains 8.93 KB (still ~20× measured peak 436 B).
60-char IME stress: heap_free unchanged from boot baseline.

**Pre-bump 2026-04-27 measurements (kept for historical context)**:
`xFreeBytesRemaining = 7,296 B (14.84 %)` post watchdog liveness
chain — 0.4 KB above the 14 % panic threshold. Watchdog task adds
768 B stack + ~88 B TCB ≈ 0.86 KB from heap_4. Threshold had been
lowered 15 %→14 % since RAM region was full and bumping was blocked.
PSRAM offload + heap bump retroactively resolved that constraint.

**Measured 2026-04-27 post B2 Stage 2 settings UI (SWD, `g_core1_boot_heap_free`)**:
`xFreeBytesRemaining = 8,160 B (16.6 %)` — 0.8 KB above the 15 %
panic threshold. Stage 2 added the `settings_client` reply queue
(8 × ~70 B from heap_4) and a ~700 B BSS cache in `settings_view`;
the BSS hit pushed RAM overflow at link-time, resolved by trimming
`VAL_BUF_MAX` 40→16 (owner long_name display truncates at 16 B,
which is fine — Stage 3 IME-driven editing will not use this cache).

**Measured 2026-04-24 post RTT task (SWD, `g_core1_boot_heap_free`)**:
`xFreeBytesRemaining = 9,312 B (19 %)` — 1.9 KB above the 15 %
panic threshold; ~0.3 KB below the estimate, consistent with
heap_4 fragmentation overhead settling at ~10 %.

**Measured via SWD (2026-04-18, post-M3.4.5d, before IME task lands)**:
`xFreeBytesRemaining = 15,008 B (14.65 KB, 30.5 %)`,
`xMinimumEverFreeBytesRemaining = 15,008 B` — estimate matches reality.
The post-M4 column is projected; re-measure after `ime` task creation
and revisit the stack sizing if the IME state ends up larger than
~3 KB or run_search's prefix-scan working set pushes over 4 KB stack.

---

## 4. Notable static buffers

Not exhaustive — just the ones large enough (≥ 256 B) to care about.

| Buffer | File | Size | Notes |
|---|---|---:|---|
| `s_drain_buf` | `src/sensor/teseo_liv3fl.c` | 256 B | Teseo I2C burst read buffer. |
| `s_gsv` accumulator × 4 | same | ~400 B | Per-talker satellite cycle builders. |
| `s_sat_view` | same | ~200 B | Published 32-sat snapshot. |
| `s_rf_state` | same | ~440 B | RF/signal diagnostics (noise, ANF, CPU, per-sat). Populated only after `teseo_enable_rf_debug_messages(true)` commissioning. |
| `s_line` | same | 96 B | NMEA line accumulator. |
| `ImeLogic` instance (M4) | `src/ime/ime_task.c` | ~3 KB | 50 × `Candidate` (~40 B) ≈ 2 KB + 256 B display + 64 B key_seq + ~200 B per-candidate prefix + state. Static to keep the `ime` task stack small. |
| LVGL draw buffer | `src/display/lvgl_glue.c` | TBD | One partial buffer for ST7789VI; update when the allocation is promoted from `LV_MEM_SIZE` to explicit static. |

Shared-SRAM IPC buffers (SPSC rings + GPS double-buffer) live in the
24 KB IPC window and are **not** in either heap — see §5 of
`firmware-architecture.md`.

---

## 5. Boot-time invariants (implemented in `main_core1_bridge.c`)

After `vTaskStartScheduler()` returns control to the scheduler and the
first task runs, the bridge task logs:

```
[m1] heap: total=49152  free=NNNNN  minEverFree=NNNNN  (target free ≥ 9830 = 20%)
```

If `free < 20 % of total`, this is flagged via IPC `LOG_LINE` at WARN
level so any regression during CI or commissioning is visible on the
host.

Every task-start helper (`lvgl_glue_start`, `bq25622_start_task`,
`sensor_task_start`, `gps_task_start`, `keypad_scan_task_create`) and
every direct `xTaskCreate` call in `main` is wrapped in:

```c
if (rc_foo != pdPASS) { panic("xTaskCreate(foo) failed"); }
```

No `(void)rc_xxx` casts. A panic prints the task name so the failing
allocation is immediately obvious from the USB CDC log (no more SWD
`mem32 ...ED28` archaeology to figure out which task ran out of heap).

---

## 6. Checklist when adding a new task

1. Decide on a stack size — start from **512 words** unless the task has
   a specific reason to be larger (LVGL → 3072 from empirical draw
   recursion, USB → 1024 because TinyUSB lives in globals).
2. Add a row to §3.1 in this file and update §3.4 Totals.
3. If the new subtotal pushes Reserve below 20 %, bump
   `configTOTAL_HEAP_SIZE` **in the same commit** that adds the task.
4. Wire the `*_task_start` helper into `main_core1_bridge.c` with a
   `panic`-on-fail check. Never `(void)rc_xxx`.
5. After the first successful run, read `uxTaskGetStackHighWaterMark()`
   and update the row if the stack can be shrunk (leave +25 % headroom).
