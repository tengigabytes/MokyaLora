"""L-1 launcher placeholder UX test.

Press OK on the Power tile (placeholder, target=VIEW_ID_COUNT) and
verify the launcher does NOT silently exit — instead the toast label
gets populated with the placeholder reason. Real targets still
navigate normally.
"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from mokya_rtt import build_frame, TYPE_KEY_EVENT  # type: ignore

KEYMAP = {}
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME    = 0
VIEW_ID_LAUNCHER     = 1
VIEW_ID_SETTINGS     = 11
VIEW_ID_TOOLS        = 18

PASS = "[PASS]"
FAIL = "[FAIL]"


def k(swd, kc, gap=180):
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([(kc & 0x7F) | 0x80, 0])))
    time.sleep(0.03)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([kc & 0x7F, 0])))
    time.sleep(gap / 1000)


def view(swd):
    return swd.read_u32(swd.symbol("s_view_router_active"))


def back_home(swd):
    for _ in range(8):
        if view(swd) == VIEW_ID_BOOT_HOME:
            return True
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 250)
    return False


def main():
    fails = 0
    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! BOOT_HOME unreachable"); sys.exit(1)

        # FUNC -> launcher; normalize cursor to (0,0) Msg.
        print("[A] open launcher")
        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 500)
        v = view(swd)
        if v != VIEW_ID_LAUNCHER:
            print(f"  {FAIL} expected LAUNCHER ({VIEW_ID_LAUNCHER}), got {v}")
            sys.exit(1)
        print(f"  {PASS} v={v}")
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'], 60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        # Cursor at (0,0) Msg.

        # Navigate to Power tile (row 2, col 2).
        print("[B] cursor to Power tile (2,2)")
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_DOWN'], 80)
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        # Still in launcher.
        v = view(swd)
        if v != VIEW_ID_LAUNCHER:
            print(f"  {FAIL} expected still LAUNCHER, got {v}")
            fails += 1
        else:
            print(f"  {PASS} still in LAUNCHER while moving cursor")

        # OK on Power placeholder — should NOT exit launcher.
        print("[C] OK on Power placeholder")
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = view(swd)
        if v != VIEW_ID_LAUNCHER:
            print(f"  {FAIL} OK on placeholder exited launcher (v={v})")
            fails += 1
        else:
            print(f"  {PASS} launcher still active after OK on placeholder")

        # Move cursor — toast should clear (we don't have an SWD path
        # to read the toast text, but the cursor-move resets it; verify
        # by NOT crashing + still in launcher).
        print("[D] arrow key clears toast (still in launcher)")
        k(swd, KEYMAP['MOKYA_KEY_LEFT'], 80)
        v = view(swd)
        if v != VIEW_ID_LAUNCHER:
            print(f"  {FAIL} arrow key exited launcher (v={v})")
            fails += 1
        else:
            print(f"  {PASS} arrow keeps launcher active")

        # Real target — Tools (1,2). Press OK should navigate.
        print("[E] OK on real tile (Tools) → navigate")
        k(swd, KEYMAP['MOKYA_KEY_UP'], 80)
        # Now at row 1, col 1 (Tele). Want Tools at (1,2). Right once.
        k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = view(swd)
        if v != VIEW_ID_TOOLS:
            print(f"  {FAIL} expected TOOLS ({VIEW_ID_TOOLS}), got {v}")
            fails += 1
        else:
            print(f"  {PASS} navigated to TOOLS (v={v})")

        # Cleanup: back home, swd inject mode reset.
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if fails == 0:
        print("==> Launcher placeholder UX PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
