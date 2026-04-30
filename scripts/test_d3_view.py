"""D-3 waypoints_view UI navigation test.

Drives via RTT key inject:
  BOOT_HOME -> FUNC -> LAUNCHER -> nav to Map slot -> OK -> MAP
            -> TAB -> WAYPOINTS  (verify view_id == VIEW_ID_WAYPOINTS)
            -> BACK -> MAP       (verify back-to-map round-trip)

Phase 2 contract: D-3 entry from D-1 via TAB; OK on D-3 stashes the
waypoint id but does not yet navigate (D-4 lands in Phase 3).
"""
import re
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from mokya_rtt import build_frame, TYPE_KEY_EVENT  # type: ignore

KEYMAP = {}
src = Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME = 0
VIEW_ID_LAUNCHER  = 1
VIEW_ID_MAP       = 14
VIEW_ID_WAYPOINTS = 33   # 32 (T10_RHW_PIN_EDIT) + 1


def send_press_release(swd, kc, hold_ms=30, gap_ms=200):
    pb = (kc & 0x7F) | 0x80
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([pb, 0])))
    time.sleep(hold_ms / 1000.0)
    rb = (kc & 0x7F)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([rb, 0])))
    time.sleep(gap_ms / 1000.0)


def read_view(swd):
    return swd.read_u32(swd.symbol('s_view_router_active'))


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual!r:<6} expected={expected!r}")
    return ok


def reset_to_boot_home(swd):
    v = read_view(swd)
    if v == VIEW_ID_BOOT_HOME: return
    swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
    for _ in range(5):
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        if read_view(swd) == VIEW_ID_BOOT_HOME: return
    raise RuntimeError(f"could not return to BOOT_HOME (view={read_view(swd)})")


def main():
    fails = 0

    with MokyaSwd() as swd:
        reset_to_boot_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        print("[start]  view=0 (BOOT_HOME)")

        # ── BOOT_HOME -> LAUNCHER -> Map (slot 1,0) -> MAP ──────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        # Anchor launcher cursor at (0,0)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=300)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        v = read_view(swd)
        if not expect("nav: BOOT_HOME -> MAP", v, VIEW_ID_MAP): fails += 1

        # ── MAP -> TAB -> WAYPOINTS ─────────────────────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_TAB'], gap_ms=300)
        v = read_view(swd)
        if not expect("MAP -> TAB -> WAYPOINTS", v, VIEW_ID_WAYPOINTS):
            fails += 1

        # ── WAYPOINTS -> UP/DOWN no-op when empty cache (smoke) ─────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=200)
        send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=200)
        v = read_view(swd)
        if not expect("UP/DOWN don't navigate", v, VIEW_ID_WAYPOINTS):
            fails += 1

        # ── WAYPOINTS -> BACK -> MAP ────────────────────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("WAYPOINTS -> BACK -> MAP", v, VIEW_ID_MAP): fails += 1

        # Cleanup: leave the device on home so re-runs are stable.
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("MAP -> BACK -> BOOT_HOME", v, VIEW_ID_BOOT_HOME):
            fails += 1

    print()
    if fails == 0:
        print("==> D-3 view navigation PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} failures)")
        sys.exit(1)


if __name__ == "__main__":
    main()
