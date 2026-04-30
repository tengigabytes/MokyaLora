"""Phase 4 verification — B-3 加入頻道 entry/exit + slot routing.

channels_view splits OK by slot occupancy:
  - slot occupied  -> VIEW_ID_CHANNEL_EDIT (17)
  - slot empty     -> VIEW_ID_CHANNEL_ADD  (25)

Verifies both branches via RTT key inject:
  1. anchor channels_view cursor at 0 (primary, occupied)
  2. OK -> expect CHANNEL_EDIT
  3. BACK -> CHANNELS
  4. DOWN -> cursor 1 (empty)
  5. OK -> expect CHANNEL_ADD

Live AdminMessage.set_channel TX path is exercised by manually pressing
Save in B-3 on hardware (or another script that drives the ROW_SAVE OK
+ reads `meshtastic --info` for new channel). Phase 4 nav test is just
the routing + view-id wiring.
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

VIEW_ID_BOOT_HOME    = 0
VIEW_ID_LAUNCHER     = 1
VIEW_ID_CHANNELS     = 16
VIEW_ID_CHANNEL_EDIT = 17
VIEW_ID_CHANNEL_ADD  = 25


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

        # FUNC + anchor cursor (0,0) + RIGHT to (0,1) Chan tile
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],    gap_ms=400)
        v = read_view(swd)
        if not expect("nav: launcher Chan -> CHANNELS", v, VIEW_ID_CHANNELS):
            fails += 1

        # Anchor channels_view cursor at row 0 (primary, occupied).
        for _ in range(8):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'], gap_ms=40)

        # Test branch 1: OK on row 0 (primary, occupied) -> CHANNEL_EDIT
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = read_view(swd)
        if not expect("OK on slot 0 (occupied) -> CHANNEL_EDIT",
                      v, VIEW_ID_CHANNEL_EDIT):
            fails += 1
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("BACK -> CHANNELS", v, VIEW_ID_CHANNELS):
            fails += 1

        # Test branch 2: DOWN to slot 1 (empty by default), OK -> CHANNEL_ADD
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        v = read_view(swd)
        if not expect("OK on slot 1 (empty) -> CHANNEL_ADD",
                      v, VIEW_ID_CHANNEL_ADD):
            fails += 1
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("BACK -> CHANNELS", v, VIEW_ID_CHANNELS):
            fails += 1

        # Cleanup back to BOOT_HOME.
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if fails == 0:
        print("==> Phase 4 navigation PASS")
        sys.exit(0)
    else:
        print(f"==> Phase 4 FAIL ({fails} step(s))")
        sys.exit(1)


if __name__ == "__main__":
    main()
