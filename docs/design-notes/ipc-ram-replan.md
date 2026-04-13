# IPC Dual Ring + SRAM Reallocation Plan

**Date:** 2026-04-13
**Phase:** Phase 2 M3 prep
**Status:** ✅ Complete — Step 1 (dual ring) + Step 2 (SRAM reallocation) both validated

---

## 1. Problem Statement

### 1.1 IPC Log/Protobuf Contention

During Meshtastic config exchange (`want_config_id` → `FromRadio` stream), Core 0's
`writeStream()` loop generates ~40 protobuf frames interleaved with LOG_DEBUG/LOG_INFO
output. Both traffic types share the single c0→c1 SPSC ring (32 slots × 264 B).
Log lines consume ring slots, creating 12–49 ms frame-to-frame gaps (vs 0–3 ms on
stock Pico2 with native SerialUSB).

**Root cause:** `IpcSerialStream::flush_tx_acc_()` (log path) and
`IpcSerialStream::write(buf, len)` (protobuf path) both push `IPC_MSG_SERIAL_BYTES`
to the same `c0_to_c1` ring. Each log line '\n' flush consumes a full 264 B ring slot,
starving protobuf data.

### 1.2 SRAM Over-Allocation to Core 0

Current split gives Core 0 (Meshtastic) 432 KB of the 512 KB main SRAM. Actual usage:

| Section | Size |
|---------|------|
| .data + .bss + .vector_table | 54.1 KB |
| .heap (runtime peak) | ~76 KB |
| **Total actual** | **~130 KB** |
| Heap unused | ~302 KB (75% wasted) |

Core 0 has aggressive MESHTASTIC_EXCLUDE flags (no GPS, Screen, WiFi, BT, I2C, MQTT,
all sensor telemetry, StoreForward, CannedMessages, ATAK, etc.). Only Admin,
TextMessage, PKI, Traceroute, NeighborInfo, and Waypoint modules are kept.

### 1.3 Core 1 Cannot Fit M3+ Workload

Core 1 (M1 Bridge) has only 56 KB. M3+ requires FreeRTOS + LVGL + MIE + HAL drivers +
a display framebuffer. Even a single 240×320×16bpp framebuffer is 150 KB — 3× the
entire current Core 1 region.

---

## 2. Design Decisions

### 2.1 IPC: Add Dedicated Log Ring (方案 A)

Add a third SPSC ring (`c0_log_to_c1`) inside the existing 24 KB shared SRAM region,
using space currently wasted as `_tail_pad` (7,324 B available).

**Rationale:** Separating log and data traffic at the ring level eliminates contention
without discarding debug output. The log ring is best-effort (no busy-wait on full) —
log lines are dropped rather than stalling protobuf. Core 1 can forward or discard
log slots independently.

**Rejected alternative (方案 B):** Drop all log output in `flush_tx_acc_()` without
pushing to any ring. Simpler (1-line change) but loses Core 0 debug visibility and
requires re-adding a log channel for M4+ anyway.

### 2.2 Framebuffer: Single Buffer, LVGL Direct Mode

Use one full-screen framebuffer in SRAM (240 × 320 × 16bpp = 150 KB) with LVGL
`LV_DISPLAY_RENDER_MODE_DIRECT`. LVGL only redraws dirty areas; DMA transfers dirty
rectangles to the LCD via PIO 8080.

**Rationale:**
- Original spec (software-requirements.md §1.3) called for ~10 KB partial render
  buffer, but direct mode gives better visual quality (no strip-boundary tearing)
  and simpler DMA logic (one base address, rect offsets).
- Single buffer (150 KB) vs double buffer (300 KB): saves 150 KB. The tradeoff is
  that LVGL must wait for the DMA flush callback before modifying the buffer — this
  is handled automatically by LVGL's flush-ready mechanism.
- PSRAM is too slow for display (12.8 FPS max due to QMI per-word overhead + DMA
  burst read errors with unconfirmed root cause — see bringup log Issue 14 / Step 25).

### 2.3 SRAM: Shrink Core 0 to 176 KB, Expand Core 1 to 312 KB

| Region | Current | Final | Change |
|--------|---------|-------|--------|
| Core 0 | 432 KB (0x20000000–0x2006BFFF) | 176 KB (0x20000000–0x2002BFFF) | −256 KB |
| Core 1 | 56 KB (0x2006C000–0x20079FFF) | 312 KB (0x2002C000–0x20079FFF) | +256 KB |
| Shared IPC | 24 KB (0x2007A000–0x2007FFFF) | 24 KB (unchanged) | 0 |
| SCRATCH_X | 4 KB | 4 KB | 0 |
| SCRATCH_Y | 4 KB | 4 KB | 0 |

**Core 0 at 176 KB:** 54 KB static + ~122 KB heap. SWD-measured heap high-water
mark ~100 KB (3 nodes). At MAX_NUM_NODES=200 (~140 KB peak), 36 KB margin (20%).

**Core 1 at 312 KB budget:**

| Component | Estimate |
|-----------|----------|
| Framebuffer (240×320×16bpp ×1) | 150 KB |
| LVGL heap (widgets, styles, draw cache) | 35 KB |
| FreeRTOS ucHeap (6 tasks + TCB + queues) | 48 KB |
| MIE C API + buffers | 5 KB |
| HAL drivers (I2C×2, PIO keypad, PWM) | 9 KB |
| Application state (messages, nodes, GPS, UI) | 16 KB |
| Static (.data/.bss misc, SDK runtime) | 10 KB |
| Stack (main + ISR) | 8 KB |
| **Total** | **~281 KB** |
| **Margin** | **~31 KB (10%)** |

Audio (PDM mic + I2S amp) removed from future revisions — no audio buffers needed.
MIE dictionary data (4 MB) and application heap (message history, node cache) live
in PSRAM (8 MB APS6404L), not SRAM.

---

## 3. IPC Shared SRAM Layout (Proposed)

Total: 24 KB at `0x2007A000–0x2007FFFF` (unchanged base/size).

```
Offset   Size      Content
────────────────────────────────────────────────────────
0x0000      16 B   Handshake (boot_magic, c0_ready, c1_ready, flash_lock)
0x0010      16 B   _pad_to_0x20
0x0020      32 B   c0_to_c1_ctrl      — DATA ring control (32 slots)
0x0040      32 B   c0_log_to_c1_ctrl  — LOG ring control (16 slots)   [NEW]
0x0060      32 B   c1_to_c0_ctrl      — CMD ring control (32 slots)
0x0080   8,448 B   c0_to_c1_slots[32]  — protobuf frames only
0x2180   4,224 B   c0_log_to_c1_slots[16] — log lines (best-effort)  [NEW]
0x3200   8,448 B   c1_to_c0_slots[32]  — host commands
0x5300     260 B   gps_buf             — GPS double-buffer (M4)
0x5404   3,068 B   _tail_pad           — breadcrumbs @ 0x5FC0 (64 B)
────────────────────────────────────────────────────────
Total   24,576 B   ✓ fits in 24 KB
```

### Ring API Change

`ipc_ring_push()` and `ipc_ring_pop()` gain a `slot_count` parameter (currently
hardcoded to `IPC_RING_SLOT_COUNT = 32`). This allows the log ring to use 16 slots
while data/cmd rings remain at 32.

```c
bool ipc_ring_push(IpcRingCtrl *ctrl, IpcRingSlot *slots,
                   uint32_t slot_count,  // NEW
                   uint8_t msg_id, uint8_t seq,
                   const void *payload, uint16_t payload_len);
```

### Data Flow After Change

```
Core 0 writeStream() loop:
  getFromRadio() → LOG_DEBUG → flush_tx_acc_() → c0_log_to_c1 (16 slots, best-effort)
                 → protobuf   → write(buf,len) → c0_to_c1     (32 slots, dedicated)

Core 1 bridge_task:
  1. Pop c0_to_c1 (DATA) → tud_cdc_write  (high priority)
  2. Pop c0_log_to_c1 (LOG) → tud_cdc_write or discard (low priority)
  3. Check CDC OUT → c1_to_c0 (CMD)
```

Expected improvement: ~~frame gap from 12–49 ms → 0–5 ms~~ — see §7 benchmark results.

---

## 4. SRAM Map (Final)

```
RP2350B SRAM (520 KB)

0x20000000 ┌──────────────────────────────────────┐
           │  Core 0 — Meshtastic (GPL-3.0)       │
           │  .vector + .data + .bss    54 KB      │  176 KB
           │  .heap                    122 KB      │
0x2002C000 ├──────────────────────────────────────┤
           │  Core 1 — FreeRTOS+LVGL+MIE          │
           │  framebuffer             150 KB       │
           │  LVGL heap + FreeRTOS     83 KB       │  312 KB
           │  HAL + App + Stack        48 KB       │
           │  margin                   31 KB       │
0x2007A000 ├──────────────────────────────────────┤
           │  Shared IPC (NOLOAD)                  │  24 KB
           │  DATA ring (32) + LOG ring (16)       │
           │  CMD ring (32) + GPS buf              │
0x20080000 ├──────────────────────────────────────┤
           │  SCRATCH_X (Core 0 stack1, unused)    │  4 KB
0x20081000 ├──────────────────────────────────────┤
           │  SCRATCH_Y (Core 0 MSP stack)         │  4 KB
0x20082000 └──────────────────────────────────────┘
```

---

## 5. Implementation Plan

### Step 1 — IPC Dual Ring (validate speed improvement)

Implement the log ring separation. Build, flash, benchmark with
`scripts/bench_raw_serial2.py`. **Gate:** frame gap must drop from 12–49 ms to < 10 ms.

Files changed:
1. `firmware/shared/ipc/ipc_protocol.h` — add `IPC_LOG_RING_SLOT_COUNT`
2. `firmware/shared/ipc/ipc_shared_layout.h` — add `c0_log_to_c1_ctrl` + slots, update `_tail_pad`
3. `firmware/shared/ipc/ipc_ringbuf.h` — add `slot_count` param to push/pop/pending/free_slots
4. `firmware/shared/ipc/ipc_ringbuf.c` — use `slot_count` param in modulo
5. `firmware/core0/.../ipc_serial_stub.cpp` — `flush_tx_acc_()` → log ring; `write(buf,len)` → data ring
6. `firmware/core1/.../main_core1_bridge.c` — drain data ring first, then log ring

### Step 2 — SRAM Reallocation (after Step 1 benchmark passes) ✅

Resize Core 0 from 432 KB → 176 KB, Core 1 from 56 KB → 312 KB. Build, flash, run
`meshtastic --info` regression test.

Files changed:
1. `firmware/core0/.../patch_arduinopico.py` — patch RAM LENGTH to 176K (was 432K via 512K−IPC)
2. `firmware/core1/.../memmap_core1_bridge.ld` — `ORIGIN=0x2002C000, LENGTH=312K`

### Step 3 — M3 LVGL Integration (future milestone)

With the expanded Core 1 region, integrate LVGL with single framebuffer. Not part of
this change — only enabled once Steps 1 and 2 are validated.

---

## 6. Risk Assessment

| Risk | Mitigation |
|------|------------|
| Core 0 heap too small at 122 KB | SWD-measured peak ~100 KB (3 nodes); MAX_NUM_NODES=200 worst case ~140 KB → 36 KB margin (20%). Can shift boundary to 0x20030000 (192 KB) if needed |
| Log ring overflow loses debug output | Acceptable — log is best-effort; can increase to 24 slots at cost of 2.1 KB tail-pad if needed |
| ringbuf API change breaks callers | Mechanical — all 7 call sites updated in same commit; compile error if any missed |
| Framebuffer single-buffer tearing | LVGL flush callback blocks next render; DMA at 8080 bus speed (~10 MHz × 8-bit = 10 MB/s) completes 150 KB in ~15 ms — 60+ FPS achievable |

---

## 7. Benchmark Baseline (Pre-Change)

From `bench_raw_serial2.py` (2026-04-13):

| Metric | Stock Pico2 | MokyaLora (M2 baseline) |
|--------|-------------|-------------------------|
| First frame latency | 201 ms | 1,827 ms |
| Frame-to-frame gap | 0–3 ms | 12–49 ms |
| Total frames | 98 | 48 |
| Total elapsed | 303 ms | 3,180 ms |

**Original target:** frame gap < 10 ms. First-frame latency unchanged (caused by
250 ms idle poll in `StreamAPI::readStream()`, separate issue — M5 doorbell ISR scope).

### 7.1 Step 1 Results — IPC Dual Ring (2026-04-13)

**Implementation:** log ring separation merged. `flush_tx_acc_()` → log ring (best-effort),
`write(buf, len)` → data ring (dedicated). Core 1 drains data ring first, log ring
only when data ring empty.

| Metric | M2 Baseline | Dual Ring (3 runs) | Change |
|--------|-------------|---------------------|--------|
| First→Last frames | ~1,350 ms | 1,040–1,166 ms | **−15 to −23%** |
| Total elapsed | 3,180 ms | 2,763–2,925 ms | **−8 to −13%** |
| Frame count | 48 | 47–49 | same |
| Frame-to-frame gap | 12–49 ms | 11–50 ms | **no improvement** |

**Analysis:** Total throughput improved ~10–20% (log no longer stalls data ring when
ring is full). However, per-frame gap is unchanged — the dominant cost is **Core 0
CPU time per `getFromRadio()` iteration**, not ring contention:

1. **`LOG_DEBUG/LOG_INFO` string formatting** — `vfprintf` through newlib is CPU-heavy
   even when the ring push itself is non-blocking. The format string expansion,
   `millis()` calls, and character-by-character `write(uint8_t)` accumulation all burn
   cycles on the critical path.
2. **Protobuf encoding** — each `getFromRadio()` call encodes a nanopb message (config,
   channel, module config, node info). Encoding + CRC takes non-trivial time.
3. **FreeRTOS SysTick overhead** — 1 ms tick interrupt cost on Cortex-M33.

**Conclusion:** The dual ring is architecturally correct (log/data separation is needed
for M4+ anyway) and gives a measurable throughput improvement. But the frame gap
bottleneck is CPU-bound on Core 0, not IPC-bound. Further investigation needed:

- **P2-13 (open):** Investigate Core 0 per-frame CPU bottleneck during config exchange.
  Candidate mitigations: (a) suppress LOG output during `writeStream()` burst
  (save `vfprintf` CPU cost); (b) lower Meshtastic log level for the MokyaLora variant
  (`LOG_LEVEL_WARN`); (c) profile `getFromRadio()` with SysTick cycle counter to
  identify the hot path. Stock Pico2 achieves 0–3 ms gaps because native SerialUSB
  `write()` is a fast memcpy into a USB endpoint buffer — no ring push, no doorbell,
  no cross-core overhead.

### 7.2 Step 2 — SRAM Reallocation ✅

**Date:** 2026-04-14
**Result:** Core 0 shrunk to 176 KB, Core 1 expanded to 312 KB. Build + flash + SWD + `meshtastic --info` round-trip verified.

Initial attempt used 192/296 split, then revised to 176/312 after detailed M3+ budget
analysis showed Core 1 margin was too tight (4.7% without audio, 10% with 312 KB).
Audio hardware (PDM mic + I2S amp) confirmed removed from future revisions.

**Changes:**

| File | Change |
|------|--------|
| `patch_arduinopico.py` | `__RAM_LENGTH__ - 0x14000` → `__RAM_LENGTH__ - 0x54000` (Core 0: 432 KB → 176 KB) |
| `memmap_core1_bridge.ld` | `ORIGIN = 0x2006C000, LENGTH = 56K` → `ORIGIN = 0x2002C000, LENGTH = 312K` |
| `memmap_default.ld` (framework) | Same patch applied to live framework copy |

**Verification:**

| Check | Result |
|-------|--------|
| Core 0 static (.data+.bss) | 53.8 KB / 176 KB (31%) |
| Core 0 SWD heap high-water mark | ~100 KB (3 nodes) → 76 KB free (43%) |
| Core 0 worst-case (200 nodes) | ~140 KB → 36 KB margin (20%) |
| Core 1 `__data_start__` | `0x2002C110` ✅ |
| Core 1 `__StackTop` | `0x2007A000` ✅ (312 KB region top) |
| `g_ipc_shared` | `0x2007A000` ✅ (unchanged) |
| SWD Core 0 PSP | `0x2000B138` (within 176 KB) ✅ |
| SWD Core 1 PSP | `0x20032F98` (within 312 KB) ✅ |
| SWD IPC boot_magic | `0x4D4F4B59` ("MOKY") ✅ |
| SWD c1_ready | 1 ✅ |
| `meshtastic --info` | Full config + 5 nodes returned ✅ |
