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
| FreeRTOS heap (heap_4) | **48 KB** | `configTOTAL_HEAP_SIZE` | Task stacks, TCBs, kernel objects. See §3. |
| LVGL heap | **48 KB** | `LV_MEM_SIZE` in `lv_conf.h` | Separate from FreeRTOS. Widgets, styles, fonts, draw buffers. |
| MCU main stack (MSP) | 4 KB | linker `__StackTop` | Pre-scheduler + exception handlers. |
| LVGL framebuffer(s) | see §4 | `display/lvgl_glue.c` | Currently one partial buffer. |
| Slack | — | — | Whatever's left. Goal: keep ≥ 32 KB unused for growth. |

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
6. **Reserve ≥ 20 % of `configTOTAL_HEAP_SIZE`** at all times. With the
   current 48 KB heap, that is ≥ 9.6 KB free after all tasks are
   running. The boot-time log prints the figure — set a lower bound and
   panic if we cross it.

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
| **App subtotal** |  | | **32,960 B (32.2 KB)** | | |

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
| **Misc subtotal** | | | | **~1.6 KB** |

### 3.4 Totals

| | Bytes (current) | Bytes (post-M4 IME) |
|---|---:|---:|
| §3.1 App tasks | 28,864 | 32,960 |
| §3.2 Kernel tasks | 4,096 | 4,096 |
| §3.3 Queues / TCBs / overhead | ~1,420 | ~1,700 |
| **Estimated** | **~34.4 KB** | **~38.8 KB** |
| `configTOTAL_HEAP_SIZE` | 49,152 | 49,152 |
| **Reserve (target ≥ 20 %)** | **~14.7 KB (30 %)** ✓ | **~10.5 KB (21 %)** ✓ |

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
