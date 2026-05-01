"""GNSS Cfg page (HW Diag, page 2) verification.

Drives launcher → HWDiag → GNSS Cfg, exercises:
  1. Fix rate cycle: OFF → 1 → 2 → 5 → 10 Hz, observed via
     teseo_get_fix_rate (SWD reads s_fix_rate global).
  2. Hot start: triggers $PSTMHOTSTART; engine continues serving NMEA.
  3. Verifies the page doesn't crash after a sequence of operations,
     by checking the gps_task drain counter keeps advancing.

Cold/warm start are NOT exercised by default — they invalidate
ephemeris and slow TTFF significantly. Pass --include-cold to enable.

Restore defaults is also skipped (NVM destructive).
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

KEY_FUNC=0x06; KEY_BACK=0x12; KEY_UP=0x1F; KEY_DOWN=0x20
KEY_LEFT=0x21; KEY_RIGHT=0x22; KEY_OK=0x23

KEYI_MAGIC = 0x4B45494A
RING = 32
ES = 2
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
    elif op == "in": ok = actual in expected
    elif op == "range": ok = expected[0] <= actual <= expected[1]
    else: ok = False
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={str(actual):<14} {op} {expected!r}")
    return ok


def goto_page(swd, a_inj, a_diag_p, target):
    """Cycle RIGHT until cur_page == target. Bounded by DIAG_PAGE_COUNT (9)."""
    for _ in range(9):
        page = swd.read_mem(a_diag_p, 1)[0]
        if page == target: return
        pr(swd, a_inj, KEY_RIGHT, 200)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--include-cold", action="store_true",
                    help="also test cold/warm start (slow TTFF)")
    args = ap.parse_args()

    fails = 0
    with MokyaSwd() as swd:
        a_inj    = swd.symbol("g_key_inject_buf")
        a_active = swd.symbol("s_view_router_active")
        a_diag_p = swd.symbol("g_hw_diag_cur_page")
        a_drain  = swd.symbol("g_teseo_drain_calls")
        a_sentence = None  # parse from teseo s_state

        if swd.read_u32(a_inj) != KEYI_MAGIC:
            print("  [FAIL] inject magic wrong"); sys.exit(2)

        # Resolve teseo s_state addr from teseo_get_state's PC-relative load.
        import subprocess
        OBJDUMP = (r"C:/Program Files/Arm/GNU Toolchain "
                   r"mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-objdump.exe")
        ELF = "build/core1_bridge/core1_bridge.elf"
        out = subprocess.check_output([OBJDUMP, "-d", ELF],
                                      text=True, errors="replace")
        idx = out.find("<teseo_get_state>:")
        teseo_state_addr = None
        if idx >= 0:
            sec = out[idx:idx+200]
            import re
            m = re.search(r"\.word\s+0x([0-9a-fA-F]+)", sec)
            if m: teseo_state_addr = int(m.group(1), 16)
        print(f"  teseo s_state @ 0x{teseo_state_addr:08X}" if teseo_state_addr
              else "  [WARN] couldn't resolve teseo s_state addr")

        # Resolve s_fix_rate (driver state).
        s_fix_rate_addr = None
        try:
            s_fix_rate_addr = swd.symbol("s_fix_rate")
        except Exception:
            pass

        # ── Phase 1: navigate to GNSS Cfg ───────────────────────────
        print("\n=== Phase 1: navigate launcher → HWDiag → GNSS Cfg (page 2) ===")
        if swd.read_u32(a_active) != VIEW_LAUNCHER:
            pr(swd, a_inj, KEY_FUNC, 300)
        for _ in range(4): pr(swd, a_inj, KEY_UP)
        for _ in range(4): pr(swd, a_inj, KEY_LEFT)
        for _ in range(3): pr(swd, a_inj, KEY_DOWN)
        pr(swd, a_inj, KEY_OK, 400)   # enter HWDiag
        goto_page(swd, a_inj, a_diag_p, 2)   # GNSS Cfg
        page = swd.read_mem(a_diag_p, 1)[0]
        active = swd.read_u32(a_active)
        if not expect("on GNSS Cfg page", page, 2): fails += 1
        if active == VIEW_LAUNCHER:
            print(f"  [FAIL] still on launcher"); fails += 1
        VIEW_HW_DIAG = active

        # ── Phase 2: fix rate cycle ─────────────────────────────────
        print("\n=== Phase 2: fix rate cycle OFF→1→2→5→10→OFF ===")
        # Widget 0 = Fix rate (already focused on entry).
        # Driver maps GNSS_RATE_OFF=0, 1HZ=1, 2HZ=2, 5HZ=5, 10HZ=10.
        if s_fix_rate_addr is None:
            print("  [SKIP] s_fix_rate symbol unavailable")
        else:
            initial = swd.read_u32(s_fix_rate_addr) & 0xFF
            print(f"  initial fix rate = {initial}")
            cycle_seen = [initial]
            for i in range(5):
                pr(swd, a_inj, KEY_OK, 2200)   # set rate triggers SAVEPAR + SRR
                rate = swd.read_u32(s_fix_rate_addr) & 0xFF
                cycle_seen.append(rate)
                print(f"  after OK #{i+1}: rate={rate}")
            # All 5 distinct rates should appear at least once across the
            # 5 OKs (5-cycle returns us through OFF/1/2/5/10).
            unique = set(cycle_seen)
            if not expect("5 distinct rates seen", len(unique) >= 5, True): fails += 1

        # ── Phase 3: hot start (non-destructive) ────────────────────
        print("\n=== Phase 3: hot start (PSTMHOTSTART) ===")
        # Move focus to widget 4 (HOT_START).
        for _ in range(4): pr(swd, a_inj, KEY_DOWN, 150)
        # Sample drain before, fire, sample after.
        d0 = swd.read_u32(a_drain)
        pr(swd, a_inj, KEY_OK, 500)
        time.sleep(2.0)
        d1 = swd.read_u32(a_drain)
        print(f"  drain {d0} -> {d1}  (delta {d1 - d0})")
        if not expect("teseo_poll continues post-hot-start",
                       (d1 - d0) >= 5, True): fails += 1

        # ── Phase 4 (optional): cold start ──────────────────────────
        if args.include_cold:
            print("\n=== Phase 4 (opt-in): cold start ===")
            # focus moves UP twice to reach COLD_START (widget 2).
            for _ in range(2): pr(swd, a_inj, KEY_UP, 150)
            d0 = swd.read_u32(a_drain)
            pr(swd, a_inj, KEY_OK, 500)
            time.sleep(3.0)
            d1 = swd.read_u32(a_drain)
            print(f"  drain {d0} -> {d1}")
            if not expect("teseo_poll continues post-cold-start",
                           (d1 - d0) >= 5, True): fails += 1

        # ── Phase 5: still on GNSS Cfg, no crash ────────────────────
        active = swd.read_u32(a_active)
        page = swd.read_mem(a_diag_p, 1)[0]
        if not expect("still on HW_DIAG (no crash)", active, VIEW_HW_DIAG): fails += 1
        if not expect("still on GNSS Cfg page",      page, 2): fails += 1

    print()
    if fails == 0:
        print("==> GNSS Cfg PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
