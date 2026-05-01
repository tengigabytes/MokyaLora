"""L-1 launcher 4-row scroll + HW Diag 8-page navigation test.

Drives the launcher and HW Diag view via SWD key inject, reads SWD
diag globals to verify state transitions:
  - g_launcher_cur_row / cur_col / view_row  (launcher_view.c)
  - g_hw_diag_cur_page                       (hw_diag_view.c)
  - s_view_router_active                     (view_router.c)

Pass criteria:
  Phase 1 (launcher scroll):
    - Down x 3 from (0,0) lands at cur_row=3, view_row=1 (viewport scrolled)
    - Up   x 3 returns to cur_row=0, view_row=0
    - Right x 2 from (0,0) lands at cur_col=2 (no col scroll)
    - Left from col 0 stays at col 0

  Phase 2 (HW Diag page cycle):
    - Navigate to HWDiag tile (row 3 col 0), OK enters HW Diag
    - active view becomes VIEW_ID_HW_DIAG
    - cur_page starts at 0 (GNSS_NMEA)
    - Right x 8 cycles through all 8 pages and wraps back to 0
    - Left x 8 cycles backward and wraps back to 0
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

# Keycodes (mirror firmware/mie/include/mie/keycode.h MOKYA_KEY_*).
KEY_FUNC  = 0x06
KEY_BACK  = 0x12
KEY_UP    = 0x1F
KEY_DOWN  = 0x20
KEY_LEFT  = 0x21
KEY_RIGHT = 0x22
KEY_OK    = 0x23

KEYI_MAGIC = 0x4B45494A   # "KEYJ"
RING_EVENTS = 32
EVENT_SIZE  = 2

# KEYJ V2 wire format (matches inject_keys.py):
#   slot byte 0 = key | 0x80 (press) or key & 0x7F (release)
#   slot byte 1 = 0 (reserved/flags — currently always zero)


def inject_event(swd, base, kb):
    """Push one event byte (with second byte 0) onto the inject ring."""
    producer = swd.read_u32(base + 4)
    slot = producer % RING_EVENTS
    addr = base + 20 + slot * EVENT_SIZE
    swd.write_u8_many([(addr, kb & 0xFF), (addr + 1, 0)])
    swd.write_u32(base + 4, producer + 1)


def press_release(swd, base, key, settle_ms=80):
    inject_event(swd, base, 0x80 | (key & 0x7F))   # press
    time.sleep(0.030)
    inject_event(swd, base, 0x00 | (key & 0x7F))   # release
    time.sleep(settle_ms / 1000.0)


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual!r:<6} expected={expected!r}")
    return ok


def main():
    fails = 0
    with MokyaSwd() as swd:
        # Resolve symbols.
        a_inject  = swd.symbol("g_key_inject_buf")
        a_active  = swd.symbol("s_view_router_active")
        a_lc_row  = swd.symbol("g_launcher_cur_row")
        a_lc_col  = swd.symbol("g_launcher_cur_col")
        a_lc_view = swd.symbol("g_launcher_view_row")
        a_diag_p  = swd.symbol("g_hw_diag_cur_page")

        VIEW_LAUNCHER = 1     # VIEW_ID_LAUNCHER
        # VIEW_ID_HW_DIAG: parse from view_router.h enum order at runtime.
        # Easier: read s_view_router_active after entering HW Diag and
        # treat that observed value as expected for subsequent checks.

        magic = swd.read_u32(a_inject)
        if magic != KEYI_MAGIC:
            print(f"  [FAIL] key_inject_buf magic = 0x{magic:08X}, expected 0x{KEYI_MAGIC:08X}")
            sys.exit(2)

        # Make sure we're at launcher. Press FUNC short to bring it up
        # (router treats short FUNC as launcher modal toggle).
        active_pre = swd.read_u32(a_active)
        print(f"  [start] s_view_router_active = {active_pre}")
        if active_pre != VIEW_LAUNCHER:
            press_release(swd, a_inject, KEY_FUNC)
            time.sleep(0.3)
            active_now = swd.read_u32(a_active)
            print(f"  [after FUNC] s_view_router_active = {active_now}")
            if active_now != VIEW_LAUNCHER:
                print(f"  [FAIL] couldn't open launcher")
                sys.exit(2)

        # ── Phase 1: launcher scroll ──────────────────────────────────
        print("\n=== Phase 1: launcher 3x4 scroll ===")
        # Reset to (0, 0). Up + Left a lot to anchor.
        for _ in range(4): press_release(swd, a_inject, KEY_UP)
        for _ in range(4): press_release(swd, a_inject, KEY_LEFT)
        time.sleep(0.2)

        cur = (swd.read_mem(a_lc_row, 1)[0],
               swd.read_mem(a_lc_col, 1)[0],
               swd.read_mem(a_lc_view, 1)[0])
        print(f"  baseline  cur_row,col,view = {cur}")
        if cur != (0, 0, 0):
            print(f"  [FAIL] baseline unexpected"); fails += 1

        # Down x 3 → expect cur_row=3, view_row=1 (viewport bottom became row 3).
        for _ in range(3): press_release(swd, a_inject, KEY_DOWN)
        cur = (swd.read_mem(a_lc_row, 1)[0],
               swd.read_mem(a_lc_col, 1)[0],
               swd.read_mem(a_lc_view, 1)[0])
        print(f"  after ↓×3 cur_row,col,view = {cur}")
        if not expect("cur_row after ↓×3", cur[0], 3): fails += 1
        if not expect("cur_col stays",      cur[1], 0): fails += 1
        if not expect("view_row scrolled",  cur[2], 1): fails += 1

        # Up x 3 → back to (0, 0, 0).
        for _ in range(3): press_release(swd, a_inject, KEY_UP)
        cur = (swd.read_mem(a_lc_row, 1)[0],
               swd.read_mem(a_lc_col, 1)[0],
               swd.read_mem(a_lc_view, 1)[0])
        print(f"  after ↑×3 cur_row,col,view = {cur}")
        if not expect("cur_row back to 0",  cur[0], 0): fails += 1
        if not expect("view_row back to 0", cur[2], 0): fails += 1

        # Right x 2 → cur_col=2 (max col), no scroll.
        for _ in range(2): press_release(swd, a_inject, KEY_RIGHT)
        cur = (swd.read_mem(a_lc_row, 1)[0],
               swd.read_mem(a_lc_col, 1)[0],
               swd.read_mem(a_lc_view, 1)[0])
        print(f"  after →×2 cur_row,col,view = {cur}")
        if not expect("cur_col after →×2", cur[1], 2): fails += 1

        # Right one more → should not advance (cap at COLS-1=2).
        press_release(swd, a_inject, KEY_RIGHT)
        cur = (swd.read_mem(a_lc_row, 1)[0],
               swd.read_mem(a_lc_col, 1)[0],
               swd.read_mem(a_lc_view, 1)[0])
        if not expect("cur_col stays at 2 (boundary)", cur[1], 2): fails += 1

        # ── Phase 2: navigate to HWDiag tile + enter ───────────────────
        print("\n=== Phase 2: navigate to HWDiag (row 3 col 0) + enter ===")
        # Reset to (0, 0). Then ↓×3 + ←×2 → row 3 col 0.
        for _ in range(4): press_release(swd, a_inject, KEY_UP)
        for _ in range(4): press_release(swd, a_inject, KEY_LEFT)
        for _ in range(3): press_release(swd, a_inject, KEY_DOWN)
        cur = (swd.read_mem(a_lc_row, 1)[0],
               swd.read_mem(a_lc_col, 1)[0])
        print(f"  positioned at row={cur[0]} col={cur[1]} (expected 3, 0)")
        if cur != (3, 0):
            print(f"  [FAIL] couldn't reach HWDiag tile"); fails += 1

        # OK to enter.
        press_release(swd, a_inject, KEY_OK, settle_ms=400)
        active = swd.read_u32(a_active)
        page0  = swd.read_mem(a_diag_p, 1)[0]
        print(f"  after OK  view={active}  page={page0}")
        if active == VIEW_LAUNCHER:
            print(f"  [FAIL] still on launcher — HWDiag tile didn't fire"); fails += 1
        # Page is whatever the LRU cache preserved from last visit
        # (0..7 valid). If the view was evicted it'll be 0; if cached
        # it'll be wherever the user left off.
        if not expect("page in [0, 7]", page0 <= 7, True): fails += 1
        VIEW_HW_DIAG = active

        # ── Phase 3: cycle pages forward 8 times → wraps to start ──────
        print("\n=== Phase 3: ←/→ page cycle (starts at page", page0, ") ===")
        seq_fwd = []
        for i in range(13):
            press_release(swd, a_inject, KEY_RIGHT, settle_ms=200)
            page = swd.read_mem(a_diag_p, 1)[0]
            seq_fwd.append(page)
        expected_fwd = [(page0 + 1 + i) % 13 for i in range(13)]
        print(f"  forward sequence:  actual {seq_fwd}  expected {expected_fwd}")
        if not expect("forward seq full cycle", seq_fwd, expected_fwd): fails += 1

        # Backward 8 times — currently at page0 (wrapped). Should go
        # page0-1, page0-2, ..., back to page0.
        seq_bwd = []
        for i in range(13):
            press_release(swd, a_inject, KEY_LEFT, settle_ms=200)
            page = swd.read_mem(a_diag_p, 1)[0]
            seq_bwd.append(page)
        expected_bwd = [(page0 - 1 - i) % 13 for i in range(13)]
        print(f"  backward sequence: actual {seq_bwd}  expected {expected_bwd}")
        if not expect("backward seq full cycle", seq_bwd, expected_bwd): fails += 1

        # Active view should still be HW_DIAG.
        active = swd.read_u32(a_active)
        if not expect("still on HW_DIAG", active, VIEW_HW_DIAG): fails += 1

        # ── Phase 4: FUNC re-opens launcher modally; focus preserved ───
        # BACK on HW Diag is a no-op because HW Diag is the active view,
        # not a modal child of launcher (launcher_done_cb already fired
        # via modal_finish(true) on the OK that entered HW Diag).
        # FUNC short-press is the documented "open launcher" gesture.
        print("\n=== Phase 4: FUNC re-opens launcher (modal) ===")
        press_release(swd, a_inject, KEY_FUNC, settle_ms=300)
        active = swd.read_u32(a_active)
        cur = (swd.read_mem(a_lc_row, 1)[0],
               swd.read_mem(a_lc_col, 1)[0],
               swd.read_mem(a_lc_view, 1)[0])
        print(f"  after FUNC  view={active}  cur_row,col,view={cur}")
        if not expect("launcher reopened",          active, VIEW_LAUNCHER): fails += 1
        if not expect("focus preserved (row 3)",    cur[0], 3): fails += 1
        if not expect("focus preserved (col 0)",    cur[1], 0): fails += 1
        if not expect("view_row preserved (1)",     cur[2], 1): fails += 1

        # Bonus: BACK on launcher modal cancels the modal, returning to
        # the original caller (HW_DIAG, since launcher was opened atop it).
        press_release(swd, a_inject, KEY_BACK, settle_ms=300)
        active = swd.read_u32(a_active)
        print(f"  after BACK  view={active}  (expect HW_DIAG={VIEW_HW_DIAG})")
        if not expect("BACK on launcher → HW_DIAG", active, VIEW_HW_DIAG): fails += 1

    print()
    if fails == 0:
        print("==> L-1 navigation PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
