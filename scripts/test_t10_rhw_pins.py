"""S-7.10 Phase 2 — RemoteHardware available_pins[] editor end-to-end.

Drives rhw_pins_view + rhw_pin_edit_view via RTT key inject; checks
view-id transitions then commits an edit and verifies via
meshtastic --info JSON.

Sequence:
  1. SWD/IPC reset of available_pins to count=0 (clean slate).
  2. Boot → launcher → settings → MODULES_INDEX (key path).
  3. DOWN × 9 → S-7.10 row → OK → RHW_PINS view.
  4. DOWN × 3 → slot 0 → OK → RHW_PIN_EDIT view.
  5. RIGHT × 9 in edit view → gpio_pin=9.
  6. DOWN × 2 → Type row, RIGHT × 2 → type=DIGITAL_WRITE (2).
  7. DOWN → Save row, OK → returns to pins view.
  8. DOWN × 4 → Apply, OK → COMMIT_CONFIG, returns to MODULES_INDEX.
  9. meshtastic --info: assert availablePins has gpio=9, type=DIGITAL_WRITE.
 10. Cleanup: SWD inject pin_count=0 + COMMIT_CONFIG.
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
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME       = 0
VIEW_ID_LAUNCHER        = 1
VIEW_ID_SETTINGS        = 11
VIEW_ID_MODULES_INDEX   = 12
VIEW_ID_T10_RHW_PINS    = 31
VIEW_ID_T10_RHW_PIN_EDIT = 32

PORT = "COM16"

PASS = "[PASS]"
FAIL = "[FAIL]"


def k(swd, kc, gap=180):
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([(kc & 0x7F) | 0x80, 0])))
    time.sleep(0.03)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([kc & 0x7F, 0])))
    time.sleep(gap / 1000)


def back_home(swd):
    for _ in range(8):
        if swd.read_u32(swd.symbol('s_view_router_active')) == VIEW_ID_BOOT_HOME:
            return True
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 250)
    return False


def view_id(swd):
    return swd.read_u32(swd.symbol('s_view_router_active'))


def reset_pins_via_t245():
    """Run t245 — it ends with pin_count=0 + COMMIT regardless of pass/fail,
    so the side effect (clean slate) is what we want."""
    subprocess.run(
        ['bash', '-c',
         f'PORT={PORT} bash scripts/test_ipc_config.sh t245 >/dev/null 2>&1'],
        timeout=120)


def info_json():
    rc = subprocess.run(
        ['python', '-m', 'meshtastic', '--port', PORT, '--info'],
        capture_output=True, text=True, timeout=30)
    return rc.stdout


def main():
    fails = 0
    print("[setup] reset available_pins via t245 helper")
    reset_pins_via_t245()

    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! BOOT_HOME unreachable"); sys.exit(1)

        print("[A] launcher -> settings -> modules_index")
        # FUNC short press: BOOT_HOME -> LAUNCHER. Use long gap to be safe.
        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 500)
        v = view_id(swd)
        if v != VIEW_ID_LAUNCHER:
            print(f"  {FAIL} FUNC didn't reach LAUNCHER; v={v}")
            sys.exit(1)
        # Normalize cursor to (0,0) — UP×3 + LEFT×3 saturates at top-left.
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'], 60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        # Settings tile is at (row=2, col=0) per launcher_view.c s_tiles[].
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_DOWN'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = view_id(swd)
        if v != VIEW_ID_SETTINGS:
            print(f"  {FAIL} expected SETTINGS ({VIEW_ID_SETTINGS}), got {v}")
            sys.exit(1)
        print(f"  {PASS} reached SETTINGS (v={v})")

        # In settings_app_view, RIGHT at ST_NODE_ROOT opens MODULES_INDEX
        # (see settings_app_view.c:475). Cursor starts at root after a
        # fresh activation, so a single RIGHT does it.
        k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 350)
        v = view_id(swd)
        if v != VIEW_ID_MODULES_INDEX:
            print(f"  {FAIL} settings RIGHT -> modules failed; v={v}")
            sys.exit(1)
        print(f"  {PASS} reached MODULES_INDEX (v={v})")

        print("[B] modules -> S-7.10 (row 9) -> rhw_pins_view")
        for _ in range(9): k(swd, KEYMAP['MOKYA_KEY_DOWN'], 50)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 350)
        v = view_id(swd)
        if v != VIEW_ID_T10_RHW_PINS:
            print(f"  {FAIL} S-7.10 -> rhw_pins failed; v={v}")
            sys.exit(1)
        print(f"  {PASS} v={v} (T10_RHW_PINS)")

        print("[C] cursor to slot 0, OK -> edit view")
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_DOWN'], 50)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 350)
        v = view_id(swd)
        if v != VIEW_ID_T10_RHW_PIN_EDIT:
            print(f"  {FAIL} slot 0 -> edit failed; v={v}")
            sys.exit(1)
        print(f"  {PASS} v={v} (T10_RHW_PIN_EDIT)")

        print("[D] edit gpio=9, type=DIGITAL_WRITE, Save")
        for _ in range(9): k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 30)
        k(swd, KEYMAP['MOKYA_KEY_DOWN'], 50)   # row 1 = name (skip)
        k(swd, KEYMAP['MOKYA_KEY_DOWN'], 50)   # row 2 = type
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 30)
        k(swd, KEYMAP['MOKYA_KEY_DOWN'], 50)   # row 3 = Save
        k(swd, KEYMAP['MOKYA_KEY_OK'], 350)
        v = view_id(swd)
        if v != VIEW_ID_T10_RHW_PINS:
            print(f"  {FAIL} Save -> pins failed; v={v}")
            sys.exit(1)
        print(f"  {PASS} returned to pins view")

        print("[E] cursor to Apply (DOWN x4), commit")
        for _ in range(4): k(swd, KEYMAP['MOKYA_KEY_DOWN'], 50)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 600)
        v = view_id(swd)
        if v != VIEW_ID_MODULES_INDEX:
            print(f"  {FAIL} Apply -> modules failed; v={v}")
            fails += 1
        else:
            print(f"  {PASS} Apply -> MODULES_INDEX (v={v})")

        time.sleep(4)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print("[F] verify availablePins via meshtastic --info")
    info = info_json()
    pin_block = re.search(r'"availablePins":\s*\[([^\]]*)\]', info, re.S)
    if not pin_block:
        print(f"  {FAIL} availablePins missing in --info"); fails += 1
    else:
        body = pin_block.group(1)
        if re.search(r'"gpioPin":\s*9\b', body):
            print(f"  {PASS} slot[0].gpio_pin = 9")
        else:
            print(f"  {FAIL} slot[0].gpio_pin missing in {body[:200]!r}")
            fails += 1
        if re.search(r'"type":\s*"DIGITAL_WRITE"', body):
            print(f"  {PASS} slot[0].type = DIGITAL_WRITE")
        else:
            print(f"  {FAIL} slot[0].type != DIGITAL_WRITE in {body[:200]!r}")
            fails += 1

    print("[cleanup] reset available_pins")
    reset_pins_via_t245()

    print()
    if fails == 0:
        print("==> S-7.10 Phase 2 PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} issues)")
        sys.exit(1)


if __name__ == "__main__":
    main()
