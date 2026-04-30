"""D-4 waypoint_detail_view UI test.

End-to-end:
  1. Inject a synthetic Waypoint cascade frame (re-uses D-3 decode test
     fixture). Cache now has one entry.
  2. Navigate BOOT_HOME → MAP → TAB → WAYPOINTS (D-3).
  3. Press OK → expect VIEW_ID_WAYPOINT_DETAIL.
  4. Press BACK → expect VIEW_ID_WAYPOINTS.
  5. Press BACK → expect VIEW_ID_MAP.
"""
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from mokya_rtt import build_frame, TYPE_KEY_EVENT  # type: ignore
from test_d3_waypoint_decode import build_cascade_frame, inject_serial_bytes  # type: ignore

KEYMAP = {}
src = Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME       = 0
VIEW_ID_MAP             = 14
VIEW_ID_WAYPOINTS       = 33
VIEW_ID_WAYPOINT_DETAIL = 34


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
    if read_view(swd) == VIEW_ID_BOOT_HOME: return
    swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
    for _ in range(6):
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        if read_view(swd) == VIEW_ID_BOOT_HOME: return
    raise RuntimeError(f"could not return to BOOT_HOME (view={read_view(swd)})")


def main():
    fails = 0

    # Step 1: inject cascade waypoint frame (so D-3 has something to show).
    print("[step 1] injecting synthetic Waypoint cascade frame")
    frame = build_cascade_frame()
    new_head = inject_serial_bytes(0x33, frame)
    print(f"  c0_to_c1 head -> {new_head}; sleeping 1.5s for decode")
    time.sleep(1.5)

    with MokyaSwd() as swd:
        # confirm cache populated
        a_total = swd.symbol("g_d3_total")
        total = swd.read_u32(a_total)
        print(f"  g_d3_total = {total} (need >=1)")
        if total < 1:
            print("  [FAIL] cascade decoder didn't fire — D-4 will be empty")
            sys.exit(1)

        reset_to_boot_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        print("[start]  view=0 (BOOT_HOME)")

        # BOOT_HOME -> LAUNCHER -> Map (slot 1,0) -> MAP
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=300)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        v = read_view(swd)
        if not expect("nav: BOOT_HOME -> MAP", v, VIEW_ID_MAP): fails += 1

        # MAP -> TAB -> WAYPOINTS (D-3)
        send_press_release(swd, KEYMAP['MOKYA_KEY_TAB'], gap_ms=300)
        v = read_view(swd)
        if not expect("MAP -> TAB -> WAYPOINTS", v, VIEW_ID_WAYPOINTS):
            fails += 1

        # WAYPOINTS -> OK -> WAYPOINT_DETAIL (D-4)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = read_view(swd)
        if not expect("WAYPOINTS -> OK -> WAYPOINT_DETAIL",
                      v, VIEW_ID_WAYPOINT_DETAIL):
            fails += 1

        # Verify the stash captured the right id
        a_target = swd.symbol("s_active_id")
        target_id = swd.read_u32(a_target)
        if not expect("D-3 stashed active_id (TEST_WP_ID)",
                      target_id, 0xCAFEBABE):
            fails += 1

        # WAYPOINT_DETAIL -> BACK -> WAYPOINTS
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("WAYPOINT_DETAIL -> BACK -> WAYPOINTS",
                      v, VIEW_ID_WAYPOINTS):
            fails += 1

        # WAYPOINTS -> BACK -> MAP
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("WAYPOINTS -> BACK -> MAP", v, VIEW_ID_MAP):
            fails += 1

        # cleanup
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)

    print()
    if fails == 0:
        print("==> D-4 view navigation PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} failures)")
        sys.exit(1)


if __name__ == "__main__":
    main()
