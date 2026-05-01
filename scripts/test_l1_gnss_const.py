"""GNSS Const page (HW Diag, page 3) navigation test.

Verifies:
  - Page reachable from launcher → HWDiag → ←→ to page 3
  - Page renders without crash (gps_task drain continues)
  - Up/Down cycling stays within page (does not switch pages)

Does NOT exercise actual preset application (writes Teseo NVM, reboots
engine — destructive). To test preset application manually:
  1. Navigate to GNSS Const page on device
  2. ↑/↓ to select desired preset
  3. Press OK; wait ~3 s for SAVEPAR + SRR

This test is a smoke test for navigation + non-crash, mirroring the
hw_diag/sysdiag patterns.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

KEY_FUNC=0x06; KEY_BACK=0x12; KEY_UP=0x1F; KEY_DOWN=0x20
KEY_LEFT=0x21; KEY_RIGHT=0x22; KEY_OK=0x23
KEYI_MAGIC = 0x4B45494A
RING = 32; ES = 2
VIEW_LAUNCHER = 1


def inj(swd, base, kb):
    p = swd.read_u32(base + 4)
    a = base + 20 + (p % RING) * ES
    swd.write_u8_many([(a, kb & 0xFF), (a + 1, 0)])
    swd.write_u32(base + 4, p + 1)


def pr(swd, base, k, ms=80):
    inj(swd, base, 0x80 | (k & 0x7F)); time.sleep(0.030)
    inj(swd, base, 0x00 | (k & 0x7F)); time.sleep(ms / 1000.0)


def expect(label, actual, expected, op="=="):
    if op == "==": ok = actual == expected
    elif op == ">=": ok = actual >= expected
    elif op == ">": ok = actual > expected
    else: ok = False
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={str(actual):<14} {op} {expected!r}")
    return ok


def main():
    fails = 0
    with MokyaSwd() as swd:
        a_inj    = swd.symbol("g_key_inject_buf")
        a_active = swd.symbol("s_view_router_active")
        a_diag_p = swd.symbol("g_hw_diag_cur_page")
        a_drain  = swd.symbol("g_teseo_drain_calls")

        if swd.read_u32(a_inj) != KEYI_MAGIC:
            print("  [FAIL] inject magic wrong"); sys.exit(2)

        # Navigate to GNSS Const page (index 3).
        if swd.read_u32(a_active) != VIEW_LAUNCHER:
            pr(swd, a_inj, KEY_FUNC, 300)
        for _ in range(4): pr(swd, a_inj, KEY_UP)
        for _ in range(4): pr(swd, a_inj, KEY_LEFT)
        for _ in range(3): pr(swd, a_inj, KEY_DOWN)
        pr(swd, a_inj, KEY_OK, 400)   # enter HWDiag

        # Cycle right until on page 3.
        for _ in range(10):
            page = swd.read_mem(a_diag_p, 1)[0]
            if page == 3: break
            pr(swd, a_inj, KEY_RIGHT, 200)

        page = swd.read_mem(a_diag_p, 1)[0]
        active = swd.read_u32(a_active)
        if not expect("on GNSS Const page", page, 3): fails += 1
        if active == VIEW_LAUNCHER:
            print(f"  [FAIL] still on launcher"); fails += 1
        VIEW_HW_DIAG = active

        # Wait for refresh tick (lazy GETPAR loads current state).
        time.sleep(1.5)

        # ↑/↓ moves focus internally — should NOT switch pages.
        for _ in range(5): pr(swd, a_inj, KEY_DOWN, 100)
        for _ in range(3): pr(swd, a_inj, KEY_UP, 100)
        page_after = swd.read_mem(a_diag_p, 1)[0]
        if not expect("page unchanged after ↑↓", page_after, 3): fails += 1

        # Verify Teseo still alive (not crashed by GETPAR/page render).
        d0 = swd.read_u32(a_drain)
        time.sleep(2.0)
        d1 = swd.read_u32(a_drain)
        if not expect("teseo_poll continues", (d1 - d0) >= 5, True): fails += 1

        # Final sanity: ←/→ leaves the page (cycles to next/prev).
        pr(swd, a_inj, KEY_RIGHT, 200)
        page_next = swd.read_mem(a_diag_p, 1)[0]
        if not expect("→ moves to next page", page_next, 4): fails += 1   # Track page
        pr(swd, a_inj, KEY_LEFT, 200)
        page_back = swd.read_mem(a_diag_p, 1)[0]
        if not expect("← returns to Const", page_back, 3): fails += 1

        # Still on HW Diag (didn't crash out to launcher).
        if not expect("still on HW_DIAG (no crash)",
                       swd.read_u32(a_active), VIEW_HW_DIAG): fails += 1

    print()
    if fails == 0:
        print("==> GNSS Const PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
