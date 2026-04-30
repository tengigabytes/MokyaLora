"""Phase 3 verification — T-2 Range Test navigation.

Drives navigation via RTT key inject:
  BOOT_HOME -> launcher -> Tools tile -> tools_view T-2 row -> RANGE_TEST

Expectations:
  - view becomes VIEW_ID_RANGE_TEST (=19 in current enum)
  - BACK returns to VIEW_ID_TOOLS

Live RANGE_TEST_APP packet decode is exercised by a peer with the
RangeTest module enabled (S-7.3 module setting `enabled=true` +
`sender=true`). Without that, range_test_log stays empty and the
T-2 view shows "total=0 mod:?" (or `OFF` once cascade has delivered
ModuleConfig.RangeTest for self).
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

KEYMAP = {}
src = Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME = 0
VIEW_ID_LAUNCHER  = 1
VIEW_ID_TOOLS     = 18
VIEW_ID_RANGE_TEST = 20


def send_press_release(swd, kc, hold_ms=30, gap_ms=200):
    pb = (kc & 0x7F) | 0x80
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([pb, 0])))
    time.sleep(hold_ms / 1000.0)
    rb = (kc & 0x7F)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([rb, 0])))
    time.sleep(gap_ms / 1000.0)


def read_view(swd):
    return swd.read_u32(swd.symbol('s_view_router_active'))


def back_home(swd):
    for _ in range(8):
        if read_view(swd) == VIEW_ID_BOOT_HOME:
            return True
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
    return read_view(swd) == VIEW_ID_BOOT_HOME


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual:<3} expected={expected}")
    return ok


def main():
    fails = 0
    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! could not reach BOOT_HOME"); sys.exit(1)
        print(f"[start]  view=0 (BOOT_HOME)  mode=RTT")

        # Navigate to launcher and anchor cursor to (0,0).
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)

        # Tools tile = (1, 2)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'],  gap_ms=80)
        for _ in range(2):
            send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],    gap_ms=400)
        v = read_view(swd)
        if not expect("nav: launcher Tools tile -> TOOLS", v, VIEW_ID_TOOLS):
            fails += 1

        # tools_view rows: T-1 / T-2 / T-3 / ... — anchor cur_row=0 first
        # because the static cursor survives LRU re-entry, then DOWN once
        # for T-2.
        for _ in range(11):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'], gap_ms=40)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        v = read_view(swd)
        if not expect("OK on T-2 row -> RANGE_TEST", v, VIEW_ID_RANGE_TEST):
            fails += 1

        # BACK should return to TOOLS.
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("BACK -> TOOLS", v, VIEW_ID_TOOLS):
            fails += 1

        # Cleanup back to BOOT_HOME.
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if fails == 0:
        print("==> Phase 3 navigation PASS")
        sys.exit(0)
    else:
        print(f"==> Phase 3 FAIL ({fails} step(s))")
        sys.exit(1)


if __name__ == "__main__":
    main()
