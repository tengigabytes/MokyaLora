"""Phase C — D-6 navigation view (map_nav_view) verification.

Two D-6 entry paths to exercise:

  Path A — D-1 OK on a peer cursor:
    BOOT_HOME -> launcher -> Map -> (no peer in cache yet, OK is no-op,
    just confirms we don't crash on the gated path)

  Path B — C-3 OP_NAVIGATE on a peer:
    BOOT_HOME -> launcher -> Nodes -> peer -> OK (NODE_DETAIL)
    -> OK (NODE_OPS) -> DOWN x7 (OP_NAVIGATE) -> OK -> MAP_NAV (id 15)

Verifies along the way:
  * VIEW_ID_MAP_NAV is reachable (= 15 in the post-Phase-C enum)
  * map_nav_view.target_num picked up the peer's node_num
  * BACK on MAP_NAV returns to MAP (D-1), not to NODE_OPS, per plan
    map-ppi-radar-v1.md §DEC-6 ("avoid 卡循環")
  * BACK on MAP returns to BOOT_HOME

map_nav_view static `s` layout (must match struct):
    +0   header           (lv_obj_t*)
    +4   bearing_big      (lv_obj_t*)
    +8   range_lbl        (lv_obj_t*)
   +12   bearing_lbl      (lv_obj_t*)
   +16   eta_lbl          (lv_obj_t*)
   +20   speed_lbl        (lv_obj_t*)
   +24   target_num       (uint32_t)   <- read this
   +28   last_render_ms   (uint32_t)
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

ELF = "build/core1_bridge/core1_bridge.elf"
ARM_OBJDUMP = (r"C:/Program Files/Arm/GNU Toolchain "
               r"mingw-w64-x86_64-arm-none-eabi/bin/"
               r"arm-none-eabi-objdump.exe")

KEYMAP = {}
src = Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME = 0
VIEW_ID_LAUNCHER  = 1
VIEW_ID_NODES     = 6
VIEW_ID_NODE_DET  = 7
VIEW_ID_NODE_OPS  = 8
VIEW_ID_MAP       = 14
VIEW_ID_MAP_NAV   = 15

NAV_TARGET_OFF = 24


def find_static_s(elf, source_basename):
    out = subprocess.check_output([ARM_OBJDUMP, "-t", elf], text=True)
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and source_basename in line:
            in_block = True
            continue
        if in_block:
            if 'df *ABS*' in line:
                break
            m = re.match(r'^([0-9a-fA-F]+)\s+l\s+O\s+\.bss\s+\S+\s+s$', line)
            if m:
                return int(m.group(1), 16)
    return None


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
    print(f"  [{tag}] {label:<40} actual=0x{actual:x}  expected=0x{expected:x}")
    return ok


def main():
    nav_s_addr = find_static_s(ELF, 'map_nav_view.c')
    if nav_s_addr is None:
        print("!! could not resolve static 's' in map_nav_view.c — abort")
        sys.exit(1)
    print(f"resolved map_nav_view.c  s @ 0x{nav_s_addr:08x}")

    fails = 0

    with MokyaSwd() as swd:
        # ── Pre-flight: reset to BOOT_HOME ─────────────────────────────
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        v = read_view(swd)
        for _ in range(8):
            if v == VIEW_ID_BOOT_HOME: break
            send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
            v = read_view(swd)
        if v != VIEW_ID_BOOT_HOME:
            print(f"!! could not reach BOOT_HOME (v={v})")
            sys.exit(1)
        print(f"[start]  view=0 (BOOT_HOME)  mode=RTT")

        # ── Path B: navigate via OP_NAVIGATE ───────────────────────────
        # Launcher: row 0 col 2 = Nodes  (default focus is row 0 col 0)
        print()
        print("Path B: BOOT_HOME -> Nodes -> peer -> NODE_OPS -> OP_NAVIGATE")
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        v = read_view(swd)
        if not expect("FUNC -> LAUNCHER", v, VIEW_ID_LAUNCHER): fails += 1

        # Anchor cursor at (0,0); launcher caches its cursor.
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        for _ in range(2):
            send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = read_view(swd)
        if not expect("OK on Nodes -> NODES", v, VIEW_ID_NODES): fails += 1

        # Skip self (row 0 is often us); pick row 1.
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=120)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        v = read_view(swd)
        if not expect("OK on peer -> NODE_DETAIL", v, VIEW_ID_NODE_DET): fails += 1

        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = read_view(swd)
        if not expect("OK on NODE_DETAIL -> NODE_OPS", v, VIEW_ID_NODE_OPS): fails += 1

        # NODE_OPS rows (post-Phase-C MAX_ENTRIES=8):
        #   0 DM / 1 Alias / 2 Favorite / 3 Ignore / 4 Traceroute /
        #   5 RequestPos / 6 RemoteAdmin / 7 Navigate
        for _ in range(7):
            send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = read_view(swd)
        if not expect("OP_NAVIGATE -> MAP_NAV", v, VIEW_ID_MAP_NAV): fails += 1

        # Verify map_nav_view picked up the target.
        target = swd.read_u32(nav_s_addr + NAV_TARGET_OFF)
        if target == 0:
            print(f"  [FAIL] map_nav_view target_num         actual=0  expected=non-zero")
            fails += 1
        else:
            print(f"  [PASS] map_nav_view target_num         actual=0x{target:08x}  (peer node_num)")

        # ── BACK chain: MAP_NAV -> MAP -> BOOT_HOME ────────────────────
        print()
        print("BACK chain: MAP_NAV -> MAP -> BOOT_HOME")
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("BACK -> MAP",            v, VIEW_ID_MAP):       fails += 1
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("BACK -> BOOT_HOME",      v, VIEW_ID_BOOT_HOME): fails += 1

        # ── Path A (degraded): D-1 OK with no cursor is a no-op ────────
        print()
        print("Path A: D-1 OK with no peer cursor must be no-op")
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        # Anchor cursor at (0,0) (cached from earlier nav).
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        v = read_view(swd)
        if not expect("launcher -> MAP",        v, VIEW_ID_MAP):       fails += 1
        # Press OK with cursor=-1 → must NOT navigate to MAP_NAV.
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=300)
        v = read_view(swd)
        if not expect("OK with no cursor stays in MAP", v, VIEW_ID_MAP): fails += 1

        # Cleanup BACK to home
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)

        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if fails == 0:
        print("==> Phase C PASS")
        sys.exit(0)
    else:
        print(f"==> Phase C FAIL ({fails} step(s) failed)")
        sys.exit(1)


if __name__ == "__main__":
    main()
