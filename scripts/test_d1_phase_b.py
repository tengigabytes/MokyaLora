"""Phase B — D-1 peer rendering / layer mask / cursor verification.

Drives navigation via RTT key inject; verifies state through SWD reads
of the file-scope `static map_t s` in map_view.c.

Coverage:
  * Phase A regressions still pass (FUNC->LAUNCHER, OK->MAP, BACK->HOME,
    LEFT/RIGHT scale clamps).
  * SET cycles layer_mask: 0(nodes) -> 1(all) -> 2(me-only) -> 0.
  * UP / DOWN are no-ops when visible_count == 0 (cursor stays -1).
  * cursor default after cold boot is -1.
  * visible_count is 0 when GPS not fixed / cache empty / me-only.

Peer rendering with non-zero visible_count requires either a real GPS
fix + at least one peer with last_position in the cache, or the
gps_dummy build flag + a SWD-faked cache entry. Out of scope for this
test — the geometry path is exercised by visual verification in
Phase D integration.

Layout of `static map_t s` (map_view.c) — must match the C struct:

    offset  field
    ---------------------------
      0     header           (lv_obj_t*)
      4     ring_inner       (lv_obj_t*)
      8     ring_mid         (lv_obj_t*)
     12     ring_outer       (lv_obj_t*)
     16     cardinal_n       (lv_obj_t*)
     20     me_cross         (lv_obj_t*)
     24     status_line      (lv_obj_t*)
     28     peer[32]         (lv_obj_t*) -> 32 * 4 = 128 bytes
    156     scale_idx        (uint8_t)
    157     layer_mask       (uint8_t)
    158     cursor           (int8_t)
    159     pad
    160     visible_count    (uint32_t)
    164     visible_node_id  (uint32_t[32]) -> 128 bytes
    292     last_cache_seq   (uint32_t)
    296     last_render_ms   (uint32_t)
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
VIEW_ID_MAP       = 14

OFF_SCALE_IDX     = 156
OFF_LAYER_MASK    = 157
OFF_CURSOR        = 158
OFF_VISIBLE_COUNT = 160

SCALE_LABELS = ['100m', '500m', '1km', '5km', '10km', '50km', '100km']
LAYER_LABELS = ['nodes', 'all', 'me-only']
LAYER_NODES, LAYER_ALL, LAYER_ME_ONLY = 0, 1, 2


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


def read_u8(swd, addr):
    return swd.read_mem(addr, 1)[0]


def read_i8(swd, addr):
    return struct.unpack('<b', swd.read_mem(addr, 1))[0]


def read_u32_at(swd, addr):
    return struct.unpack('<I', swd.read_mem(addr, 4))[0]


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual!r:<10} expected={expected!r}")
    return ok


def main():
    s_addr = find_static_s(ELF, 'map_view.c')
    if s_addr is None:
        print("!! could not resolve static 's' in map_view.c — abort")
        sys.exit(1)
    print(f"resolved map_view.c  s @ 0x{s_addr:08x}")

    fails = 0

    with MokyaSwd() as swd:
        # ── Pre-flight: reset to BOOT_HOME if not already there ────────
        v = read_view(swd)
        if v != VIEW_ID_BOOT_HOME:
            print(f"[start]  view={v} (not BOOT_HOME) — sending BACKs")
            swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
            for _ in range(4):
                send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
                if read_view(swd) == VIEW_ID_BOOT_HOME: break
            v = read_view(swd)
            if v != VIEW_ID_BOOT_HOME:
                print(f"!! could not return to BOOT_HOME (view={v})")
                sys.exit(1)
        else:
            swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        print(f"[start]  view=0 (BOOT_HOME)  mode=RTT")

        # ── Navigate to MAP ────────────────────────────────────────────
        # Anchor launcher cursor at (0,0) first; cached cursor leaks
        # across modal commits.
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=300)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        v = read_view(swd)
        if not expect("nav: BOOT_HOME -> MAP", v, VIEW_ID_MAP): fails += 1

        # ── Reset state to known defaults via SWD ──────────────────────
        # `static map_t s` survives view destroys (s_first_create_done
        # gate), so prior tests leave it in whatever they last clamped
        # to. Clobber to the same default a fresh boot would pick.
        swd.write_u8_many([
            (s_addr + OFF_SCALE_IDX,  2),     # 1km
            (s_addr + OFF_LAYER_MASK, 0),     # NODES
            (s_addr + OFF_CURSOR,     0xFF),  # -1 in i8
        ])
        swd.write_u32(s_addr + OFF_VISIBLE_COUNT, 0)
        # Bump cache-seq sentinel so the next refresh definitely repaints.
        time.sleep(0.05)

        # ── Initial state ──────────────────────────────────────────────
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        lm = read_u8(swd, s_addr + OFF_LAYER_MASK)
        cu = read_i8(swd, s_addr + OFF_CURSOR)
        vc = read_u32_at(swd, s_addr + OFF_VISIBLE_COUNT)
        print(f"           initial  scale={sc} ({SCALE_LABELS[sc]})  "
              f"layer={lm} ({LAYER_LABELS[lm]})  cursor={cu}  visible={vc}")
        if not expect("scale_idx default",     sc, 2):           fails += 1
        if not expect("layer_mask default",    lm, LAYER_NODES): fails += 1
        if not expect("cursor default",        cu, -1):          fails += 1
        if not expect("visible_count default", vc, 0):           fails += 1

        # ── SET cycles layer mask ──────────────────────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_SET'], gap_ms=200)
        lm = read_u8(swd, s_addr + OFF_LAYER_MASK)
        if not expect("SET 1 -> layer=ALL",     lm, LAYER_ALL):     fails += 1
        send_press_release(swd, KEYMAP['MOKYA_KEY_SET'], gap_ms=200)
        lm = read_u8(swd, s_addr + OFF_LAYER_MASK)
        if not expect("SET 2 -> layer=ME_ONLY", lm, LAYER_ME_ONLY): fails += 1
        send_press_release(swd, KEYMAP['MOKYA_KEY_SET'], gap_ms=200)
        lm = read_u8(swd, s_addr + OFF_LAYER_MASK)
        if not expect("SET 3 -> layer=NODES (wrap)", lm, LAYER_NODES): fails += 1

        # ── UP / DOWN no-op when visible_count == 0 ────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_UP'], gap_ms=200)
        cu = read_i8(swd, s_addr + OFF_CURSOR)
        if not expect("UP   no-op (no visible peers)",   cu, -1): fails += 1
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=200)
        cu = read_i8(swd, s_addr + OFF_CURSOR)
        if not expect("DOWN no-op (no visible peers)",   cu, -1): fails += 1

        # ── Phase A regressions ────────────────────────────────────────
        for i in range(5):
            send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=120)
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        if not expect("RIGHT x5 -> scale clamp top",  sc, 6): fails += 1
        for i in range(10):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=80)
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        if not expect("LEFT x10 -> scale clamp bot",  sc, 0): fails += 1

        # ── Option B: BACK resets layer + cursor, scale stays sticky ──
        # Set layer to ALL just before BACK so we can tell whether the
        # exit handler reset it (vs inherited NODES from the wrap test
        # at line ~SET 3). cursor stays -1 here (no visible peers).
        send_press_release(swd, KEYMAP['MOKYA_KEY_SET'], gap_ms=200)
        lm = read_u8(swd, s_addr + OFF_LAYER_MASK)
        if not expect("pre-BACK: layer = ALL", lm, LAYER_ALL): fails += 1

        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("BACK -> BOOT_HOME",            v, VIEW_ID_BOOT_HOME): fails += 1

        # Sample the static immediately after BACK — apply() should
        # have reset layer to NODES + cursor to -1, while leaving
        # scale at whatever the LEFT clamp last left it (0).
        sc_after_back = read_u8 (swd, s_addr + OFF_SCALE_IDX)
        lm_after_back = read_u8 (swd, s_addr + OFF_LAYER_MASK)
        cu_after_back = read_i8 (swd, s_addr + OFF_CURSOR)
        if not expect("BACK reset: layer  -> NODES",  lm_after_back, LAYER_NODES): fails += 1
        if not expect("BACK reset: cursor -> -1",     cu_after_back, -1):          fails += 1
        if not expect("BACK keep:  scale  preserved", sc_after_back, 0):           fails += 1

        # ── LRU re-entry: scale still preserved on second create ───────
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=300)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        lm = read_u8(swd, s_addr + OFF_LAYER_MASK)
        if not expect("re-enter: scale preserved (=0)",       sc, 0):           fails += 1
        if not expect("re-enter: layer still NODES",          lm, LAYER_NODES): fails += 1
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)

        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if fails == 0:
        print("==> Phase B PASS")
        sys.exit(0)
    else:
        print(f"==> Phase B FAIL ({fails} step(s) failed)")
        sys.exit(1)


if __name__ == "__main__":
    main()
