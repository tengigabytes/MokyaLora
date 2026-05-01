"""SysDiag (launcher tile 11) deep verification.

Drives launcher → SysDiag, navigates 3 pages, deep-verifies each page's
underlying state via SWD diag mirrors:

  Resources page  → heap_free packed in g_sys_diag_status[16..31]; LFS
                    stats in g_c1_storage_blocks_used / blocks_total
                    (existing globals).
  CPU page        → task_count in g_sys_diag_status[8..15]; cpu_load
                    instrumentation alive (g_cpu_load_windows advances).
  Screen page     → screen state in g_sys_diag_status[8..15] —
                      bit 0  fps_overlay_on
                      bit 1  mode (0=normal, 1=pixtest)
                      bits 2..4  pixtest_idx (0..4)

Drives all interactions via key_inject ring (no human in the loop).
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

KEY_FUNC  = 0x06
KEY_BACK  = 0x12
KEY_UP    = 0x1F
KEY_DOWN  = 0x20
KEY_LEFT  = 0x21
KEY_RIGHT = 0x22
KEY_OK    = 0x23

KEYI_MAGIC  = 0x4B45494A
RING_EVENTS = 32
EVENT_SIZE  = 2

VIEW_LAUNCHER = 1


def inject_event(swd, base, kb):
    producer = swd.read_u32(base + 4)
    slot = producer % RING_EVENTS
    addr = base + 20 + slot * EVENT_SIZE
    swd.write_u8_many([(addr, kb & 0xFF), (addr + 1, 0)])
    swd.write_u32(base + 4, producer + 1)


def press_release(swd, base, key, settle_ms=80):
    inject_event(swd, base, 0x80 | (key & 0x7F))
    time.sleep(0.030)
    inject_event(swd, base, 0x00 | (key & 0x7F))
    time.sleep(settle_ms / 1000.0)


def expect(label, actual, expected, op="=="):
    if op == "==":   ok = actual == expected
    elif op == ">=": ok = actual >= expected
    elif op == "<=": ok = actual <= expected
    elif op == ">":  ok = actual > expected
    elif op == "in": ok = actual in expected
    elif op == "range": ok = expected[0] <= actual <= expected[1]
    else: ok = False
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={str(actual):<14} {op} {expected!r}")
    return ok


def goto_page(swd, a_inject, a_status, target):
    """Cycle RIGHT until cur_page (low byte of g_sys_diag_status) == target."""
    for _ in range(4):
        page = swd.read_u32(a_status) & 0xFF
        if page == target: return
        press_release(swd, a_inject, KEY_RIGHT, settle_ms=200)


def main():
    fails = 0
    with MokyaSwd() as swd:
        a_inject     = swd.symbol("g_key_inject_buf")
        a_active     = swd.symbol("s_view_router_active")
        a_status     = swd.symbol("g_sys_diag_status")
        a_windows    = swd.symbol("g_cpu_load_windows")
        a_idle       = swd.symbol("g_cpu_idle_count")
        a_baseline   = swd.symbol("g_cpu_idle_baseline")
        a_lfs_used   = swd.symbol("g_c1_storage_blocks_used")
        a_lfs_total  = swd.symbol("g_c1_storage_blocks_total")

        if swd.read_u32(a_inject) != KEYI_MAGIC:
            print("  [FAIL] inject magic wrong"); sys.exit(2)

        # ── Phase 1: navigate launcher → SysDiag tile ───────────────
        print("=== Phase 1: navigate to SysDiag tile (row 3 col 1) ===")
        if swd.read_u32(a_active) != VIEW_LAUNCHER:
            press_release(swd, a_inject, KEY_FUNC, settle_ms=300)
        for _ in range(4): press_release(swd, a_inject, KEY_UP)
        for _ in range(4): press_release(swd, a_inject, KEY_LEFT)
        for _ in range(3): press_release(swd, a_inject, KEY_DOWN)
        press_release(swd, a_inject, KEY_RIGHT)   # col 1
        press_release(swd, a_inject, KEY_OK, settle_ms=400)

        active = swd.read_u32(a_active)
        if active == VIEW_LAUNCHER:
            print(f"  [FAIL] tile didn't fire — still on launcher"); fails += 1
        else:
            print(f"  [PASS] entered SysDiag (view={active})")
        VIEW_SYS_DIAG = active

        # ── Phase 2: Resources page deep ────────────────────────────
        print("\n=== Phase 2: Resources page — heap + LFS verification ===")
        goto_page(swd, a_inject, a_status, 0)
        time.sleep(1.5)   # let resources_refresh tick fire

        status = swd.read_u32(a_status)
        page = status & 0xFF
        heap_words = (status >> 16) & 0xFFFF
        heap_free = heap_words * 4
        lfs_used = swd.read_u32(a_lfs_used)
        lfs_total = swd.read_u32(a_lfs_total)

        print(f"  status=0x{status:08X}  page={page}  heap_free={heap_free}")
        print(f"  LFS used={lfs_used} / total={lfs_total}")
        if not expect("on Resources page", page, 0): fails += 1
        # heap_free should be 0 < x < 52KB - 256 = 53,008
        if not expect("heap_free > 0",                heap_free, 0, op=">"): fails += 1
        if not expect("heap_free < total (53008)",    heap_free, 53008, op="<="): fails += 1
        if not expect("LFS total > 0",                lfs_total, 0, op=">"): fails += 1
        if not expect("LFS used <= total",            lfs_used <= lfs_total, True): fails += 1

        # ── Phase 3: CPU page — task_count + cpu_load liveness ─────
        print("\n=== Phase 3: CPU page — task_count + load liveness ===")
        goto_page(swd, a_inject, a_status, 1)
        time.sleep(1.5)

        status = swd.read_u32(a_status)
        page = status & 0xFF
        task_count = (status >> 8) & 0xFF
        print(f"  status=0x{status:08X}  page={page}  task_count={task_count}")
        if not expect("on CPU page", page, 1): fails += 1
        if not expect("task_count in [5, 30]", task_count, (5, 30), op="range"): fails += 1

        # Sample cpu_load over 4 s.
        win0 = swd.read_u32(a_windows)
        idle0 = swd.read_u32(a_idle)
        time.sleep(4.0)
        win1 = swd.read_u32(a_windows)
        idle1 = swd.read_u32(a_idle)
        baseline = swd.read_u32(a_baseline)
        print(f"  cpu_load windows {win0} -> {win1}  idle {idle0} -> {idle1}")
        if not expect("cpu_load >= 3 windows in 4s", (win1 - win0) >= 3, True): fails += 1
        if (idle1 - idle0) == 0:
            print(f"  [WARN] idle counter stuck — expected (usb_device_task tight loop)")

        # ── Phase 4: Screen page — toggle FPS, pixtest cycle ───────
        print("\n=== Phase 4: Screen page — FPS toggle + pixtest cycle ===")
        goto_page(swd, a_inject, a_status, 2)
        time.sleep(0.3)

        # Initial state byte (low bit fps_on=0, mode=0, pixtest_idx=0).
        status = swd.read_u32(a_status)
        state = (status >> 8) & 0xFF
        page = status & 0xFF
        print(f"  initial: status=0x{status:08X} page={page} state=0x{state:02X}")
        if not expect("on Screen page",     page, 2): fails += 1
        if not expect("fps_on initially 0", state & 0x1, 0): fails += 1
        if not expect("mode initially 0",   (state >> 1) & 0x1, 0): fails += 1

        # OK toggles fps_on
        press_release(swd, a_inject, KEY_OK, settle_ms=200)
        state = (swd.read_u32(a_status) >> 8) & 0xFF
        if not expect("after OK: fps_on=1", state & 0x1, 1): fails += 1
        # OK again toggles back
        press_release(swd, a_inject, KEY_OK, settle_ms=200)
        state = (swd.read_u32(a_status) >> 8) & 0xFF
        if not expect("after OK×2: fps_on=0", state & 0x1, 0): fails += 1

        # UP enters pixtest mode
        press_release(swd, a_inject, KEY_UP, settle_ms=200)
        state = (swd.read_u32(a_status) >> 8) & 0xFF
        if not expect("after UP: mode=1 (pixtest)", (state >> 1) & 0x1, 1): fails += 1
        if not expect("after UP: pixtest_idx=0",    (state >> 2) & 0x7, 0): fails += 1

        # OK cycles pixtest_idx 0 → 1 → 2 → 3 → 4 → 0
        expected_indices = [1, 2, 3, 4, 0]
        actual_indices = []
        for _ in range(5):
            press_release(swd, a_inject, KEY_OK, settle_ms=150)
            state = (swd.read_u32(a_status) >> 8) & 0xFF
            actual_indices.append((state >> 2) & 0x7)
        if not expect("pixtest cycle 1..4,0", actual_indices, expected_indices): fails += 1

        # UP exits pixtest
        press_release(swd, a_inject, KEY_UP, settle_ms=200)
        state = (swd.read_u32(a_status) >> 8) & 0xFF
        if not expect("after UP exit: mode=0",     (state >> 1) & 0x1, 0): fails += 1

        # FPS counter: bits 16..31 of status hold fps × 10 while Screen
        # page is active. Sample 3 times over 3 s; expect non-zero and
        # roughly steady (LVGL refresh rate is ~30-60 Hz on this stack).
        time.sleep(1.5)   # let one fps window land
        fps_samples = []
        for _ in range(3):
            time.sleep(1.0)
            fps_x10 = (swd.read_u32(a_status) >> 16) & 0xFFFF
            fps_samples.append(fps_x10)
        print(f"  fps_x10 samples: {fps_samples}  (FPS = x10 / 10)")
        if not expect("fps > 0",                  fps_samples[-1], 0, op=">"): fails += 1
        if not expect("fps in plausible range",   fps_samples[-1], (50, 1500), op="range"): fails += 1
        # All 3 samples should be close (within ±20 of each other ≈ ±2 FPS).
        spread = max(fps_samples) - min(fps_samples)
        if not expect("fps stable across 3 s (spread <= 50)", spread, 50, op="<="): fails += 1

        # ── Phase 5: ←/→ page cycle (existing nav check) ───────────
        print("\n=== Phase 5: ←/→ page cycle (full wrap) ===")
        page0 = swd.read_u32(a_status) & 0xFF
        seq_fwd = []
        for _ in range(4):
            press_release(swd, a_inject, KEY_RIGHT, settle_ms=200)
            seq_fwd.append(swd.read_u32(a_status) & 0xFF)
        expected_fwd = [(page0 + 1 + i) % 4 for i in range(4)]
        if not expect("forward 4 cycles", seq_fwd, expected_fwd): fails += 1

        # ── Phase 6: still on SysDiag, no crash ────────────────────
        active = swd.read_u32(a_active)
        if not expect("still on SysDiag (no crash)", active, VIEW_SYS_DIAG): fails += 1

    print()
    if fails == 0:
        print("==> SysDiag deep verification PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
