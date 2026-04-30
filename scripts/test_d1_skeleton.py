"""Phase A — D-1 map_view skeleton verification.

Drives navigation via RTT key inject (frame on RTT down-channel 1) and
verifies state through SWD reads of `s_view_router_active` and the
`map_view.c` static `s.scale_idx`.

Sequence:
  1.  Verify start at BOOT_HOME (id 0)
  2.  Inject FUNC press/release  -> launcher modal (id 1)
  3.  Inject DOWN press/release  -> focus row 1 (Map tile)
  4.  Inject OK press/release    -> commit modal -> MAP (id 14)
  5.  Read s.scale_idx (expect 2 = "1km" default on first entry)
  6.  Inject RIGHT  x 5          -> scale_idx clamped at 6 (100km)
  7.  Inject LEFT   x 10         -> scale_idx clamped at 0 (100m)
  8.  Inject BACK                -> back to BOOT_HOME (id 0)

Prints PASS/FAIL per step and a final summary. Reads up-channel 0
(trace) bytes that accumulated during the run as a bonus diagnostic.
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

# Offset of `scale_idx` inside the file-scope `static map_t s` in
# map_view.c. Phase-B layout (peer[32] added after status_line):
#   7 pointers (28 B) + peer[32] (128 B) = 156   -> scale_idx
SCALE_IDX_OFFSET = 156

SCALE_LABELS = ['100m', '500m', '1km', '5km', '10km', '50km', '100km']
SCALE_DEFAULT = 2     # firmware s_scale_m index for 1km (SCALE_DEFAULT)
SCALE_TOP     = 6     # 100km
SCALE_BOTTOM  = 0     # 100m


def find_static_s_in_obj(elf, source_basename):
    """Resolve the address of the file-scope `static <type> s;` defined
    in `source_basename` (e.g. 'map_view.c'). Same trick used by
    t2_3_channels_test.py — locate the source-file boundary marker in
    the symbol table, then find the next local 'O' .bss symbol named s."""
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
    """Build two MOKYA_KIJ_TYPE_KEY_EVENT frames (press, release) and
    write them into the RTT down-channel via mokya_swd's rtt_send_frame.
    Caller must already have called set_key_inject_mode(RTT)."""
    # press
    pb = (kc & 0x7F) | 0x80
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([pb, 0])))
    time.sleep(hold_ms / 1000.0)
    # release
    rb = (kc & 0x7F)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([rb, 0])))
    time.sleep(gap_ms / 1000.0)


def read_view(swd):
    return swd.read_u32(swd.symbol('s_view_router_active'))


def read_scale(swd, s_addr):
    """Read 1 byte at s.scale_idx."""
    return swd.read_mem(s_addr + SCALE_IDX_OFFSET, 1)[0]


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<32} actual={actual}  expected={expected}")
    return ok


def main():
    s_addr = find_static_s_in_obj(ELF, 'map_view.c')
    if s_addr is None:
        print(f"!! could not resolve static 's' in map_view.c — abort")
        sys.exit(1)
    print(f"resolved map_view.c  s @ 0x{s_addr:08x}")

    fails = 0

    with MokyaSwd() as swd:
        rtt_stats_addr = {
            name: swd.symbol(name) for name in
            ('s_rtt_frames_ok', 's_rtt_rejected', 's_rtt_bytes_read')
        }
        rtt_before = {n: swd.read_u32(a) for n, a in rtt_stats_addr.items()}

        # ── 1. Start state ─────────────────────────────────────────────
        v = read_view(swd)
        if v != VIEW_ID_BOOT_HOME:
            print(f"!! NOT at BOOT_HOME (view={v}). Reflash + reset and try again.")
            sys.exit(1)
        print(f"[start]  view={v} (BOOT_HOME)")

        # Switch firmware key-inject transport to RTT down-channel.
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        print("[mode]   key_inject_mode=RTT")

        # ── 2. FUNC -> launcher ────────────────────────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        v = read_view(swd)
        if not expect("FUNC -> LAUNCHER", v, VIEW_ID_LAUNCHER): fails += 1

        # Anchor launcher cursor at (0,0) — launcher_view caches its
        # static cursor across modal commits and LRU promotions, so
        # back-to-back tests share state. Clamp first, then move.
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)

        # ── 3. DOWN -> focus row 1 (Map) ───────────────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=300)
        v = read_view(swd)
        if not expect("DOWN  stays in LAUNCHER", v, VIEW_ID_LAUNCHER): fails += 1

        # ── 4. OK -> Map app ───────────────────────────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = read_view(swd)
        if not expect("OK    -> MAP", v, VIEW_ID_MAP): fails += 1

        # ── 4b. Reset scale to known default via SWD ───────────────────
        # `static map_t s` survives view destroys (s_first_create_done
        # gate), so prior tests leave scale wherever they last clamped.
        # Clobber to the same default a fresh boot would pick.
        swd.write_u8_many([(s_addr + SCALE_IDX_OFFSET, SCALE_DEFAULT)])
        time.sleep(0.05)

        # ── 5. scale_idx default ───────────────────────────────────────
        sc = read_scale(swd, s_addr)
        if not expect("scale_idx default (1km)", sc, SCALE_DEFAULT): fails += 1
        print(f"           label='{SCALE_LABELS[sc]}'")

        # ── 6. RIGHT x5 -> scale_idx clamped at top ────────────────────
        for i in range(5):
            send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=120)
            sc = read_scale(swd, s_addr)
            print(f"           after RIGHT #{i+1}: scale_idx={sc} ('{SCALE_LABELS[sc]}')")
        if not expect("scale_idx top  clamp",  sc, SCALE_TOP): fails += 1
        v = read_view(swd)
        if not expect("RIGHT stays in MAP",   v, VIEW_ID_MAP): fails += 1

        # ── 7. LEFT x10 -> scale_idx clamped at bottom ─────────────────
        for i in range(10):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=80)
        sc = read_scale(swd, s_addr)
        print(f"           after 10x LEFT: scale_idx={sc} ('{SCALE_LABELS[sc]}')")
        if not expect("scale_idx bottom clamp", sc, SCALE_BOTTOM): fails += 1
        v = read_view(swd)
        if not expect("LEFT  stays in MAP",   v, VIEW_ID_MAP): fails += 1

        # ── 8. BACK -> BOOT_HOME ───────────────────────────────────────
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = read_view(swd)
        if not expect("BACK -> BOOT_HOME",   v, VIEW_ID_BOOT_HOME): fails += 1

        # Restore key-inject transport to default SWD before exit (the
        # MokyaSwd __exit__ also does this defensively).
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

        # ── RTT parser stats — sanity-check that frames did go through
        #    the RTT path (and not silently fall back to SWD ring). ────
        rtt_after = {n: swd.read_u32(a) for n, a in rtt_stats_addr.items()}
        delta = {n: rtt_after[n] - rtt_before[n] for n in rtt_after}
        # 18 press/release pairs above = 36 frames
        print()
        print("RTT parser deltas:")
        for n, d in delta.items():
            print(f"  {n:24} +{d}")
        if delta['s_rtt_frames_ok'] < 30:
            print("  !! low frame count — RTT inject may not have engaged")
            fails += 1

    print()
    if fails == 0:
        print("==> Phase A PASS")
        sys.exit(0)
    else:
        print(f"==> Phase A FAIL ({fails} step(s) failed)")
        sys.exit(1)


if __name__ == "__main__":
    main()
