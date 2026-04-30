"""Diagnostic: is map_view's `static s` cross-test "state leakage" a
firmware bug, or the documented preservation feature
(s_first_create_done + destroy()-no-memset)?

Observations:
  T0  cold boot: read s_first_create_done + s.scale_idx (BSS-zero
                 should give false + 0 — never created yet)
  T1  enter D-1 first time:  expect s_first_create_done=true,
                              s.scale_idx=SCALE_DEFAULT(2)
  T2  inject LEFT x10 in D-1: expect s.scale_idx=0
  T3  BACK out, then enter D-1 again: expect s.scale_idx=0 preserved
  T4  SYSRESETREQ (cold boot redo): expect s_first_create_done=false
                                     + s.scale_idx=0 (BSS)
  T5  enter D-1 first time post-reset: expect s.scale_idx=2 again

If T0..T5 all match → behaviour is exactly as the create()
docstring promises. The Phase A "second-run FAIL" was a stale
test expectation, not a firmware bug.

If any observation diverges → real bug; surface here.
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
ARM_NM = (r"C:/Program Files/Arm/GNU Toolchain "
          r"mingw-w64-x86_64-arm-none-eabi/bin/"
          r"arm-none-eabi-nm.exe")
JLINK = r"C:/Program Files/SEGGER/JLink_V932/JLink.exe"

KEYMAP = {}
src = Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME = 0
VIEW_ID_MAP       = 14

OFF_SCALE_IDX = 156


def find_static(elf, source_basename, name='s'):
    """Locate file-scope static `name` defined inside source_basename."""
    out = subprocess.check_output([ARM_OBJDUMP, "-t", elf], text=True)
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and source_basename in line:
            in_block = True
            continue
        if in_block:
            if 'df *ABS*' in line:
                break
            m = re.match(rf'^([0-9a-fA-F]+)\s+l\s+O\s+\.bss\s+\S+\s+{re.escape(name)}$',
                         line)
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


def report(label, observed, expected_text):
    ok = (observed == expected_text) if isinstance(expected_text, (int, str)) else False
    tag = "OK " if observed == expected_text else "?? "
    print(f"  {tag} {label:<48} observed={observed!r}  expected={expected_text!r}")


def back_to_home(swd):
    for _ in range(10):
        if read_view(swd) == VIEW_ID_BOOT_HOME:
            return True
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
    return read_view(swd) == VIEW_ID_BOOT_HOME


def enter_d1(swd):
    """FUNC + anchor cursor + DOWN + OK to navigate to D-1 (slot 3)."""
    send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
    for _ in range(3):
        send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
    for _ in range(3):
        send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
    send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=120)
    send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],   gap_ms=400)


def jlink_sysresetreq():
    """Issue a fresh AIRCR.SYSRESETREQ via Core 0 J-Link Commander.
    Closes any pylink session held by the host first."""
    script = "/tmp/jlink_reset.jlink"
    Path(script).write_text("connect\nr\ng\nqc\n", encoding="utf-8")
    win_script = subprocess.check_output(
        ['cygpath', '-w', script], text=True).strip()
    subprocess.run(
        [JLINK, '-device', 'RP2350_M33_0', '-if', 'SWD', '-speed', '4000',
         '-autoconnect', '1', '-CommanderScript', win_script],
        check=True, capture_output=True, text=True, timeout=30)


def main():
    s_addr = find_static(ELF, 'map_view.c', 's')
    f_addr = find_static(ELF, 'map_view.c', 's_first_create_done')
    print(f"map_view.c  s                    @ 0x{s_addr:08x}")
    print(f"map_view.c  s_first_create_done  @ 0x{f_addr:08x}")
    print()

    # ── Round 1: pre-create + first create + LEFT clamp + LRU re-create ──
    print("=== Round 1 — first cold-boot D-1 entry, clamp, re-enter ===")
    print()
    print("(Assumes Core 1 was running; if first run after flash, T0 will")
    print("show s_first_create_done already true if MAP was visited earlier.)")
    print()

    with MokyaSwd() as swd:
        # T0 — read state without entering D-1.
        if not back_to_home(swd):
            print("!! could not reach BOOT_HOME; abort")
            return 1
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        fcd = read_u8(swd, f_addr)
        print(f"[T0  pre-D1 (currently in BOOT_HOME)]")
        print(f"     s_first_create_done = {fcd}")
        print(f"     s.scale_idx         = {sc}")
        print()

        # T1 — enter D-1 for the first time this session.
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        enter_d1(swd)
        v = read_view(swd)
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        fcd = read_u8(swd, f_addr)
        print(f"[T1  after enter D-1 first time]")
        print(f"     view                = {v} ({'MAP' if v==VIEW_ID_MAP else '?'})")
        print(f"     s_first_create_done = {fcd}")
        print(f"     s.scale_idx         = {sc}")
        report("s_first_create_done == 1", fcd, 1)
        print()

        # T2 — clamp scale to 0 with LEFT x10.
        for _ in range(10):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        print(f"[T2  after LEFT x10 in D-1]")
        print(f"     s.scale_idx         = {sc}")
        report("scale clamped to 0", sc, 0)
        print()

        # T3 — BACK out then re-enter; scale should preserve at 0.
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        if not back_to_home(swd):
            print("!! could not reach BOOT_HOME; abort"); return 1
        # Sample the static immediately after destroy() — should still
        # hold 0 (destroy() only nulls widget pointers, never memsets).
        sc_after_destroy = read_u8(swd, s_addr + OFF_SCALE_IDX)
        fcd_after_destroy = read_u8(swd, f_addr)
        print(f"[T3a after BACK to BOOT_HOME (D-1 destroyed)]")
        print(f"     s_first_create_done = {fcd_after_destroy}")
        print(f"     s.scale_idx         = {sc_after_destroy}")
        report("scale preserved across destroy", sc_after_destroy, 0)
        report("s_first_create_done still 1",     fcd_after_destroy, 1)
        print()

        enter_d1(swd)
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        fcd = read_u8(swd, f_addr)
        print(f"[T3b after re-enter D-1]")
        print(f"     s_first_create_done = {fcd}")
        print(f"     s.scale_idx         = {sc}")
        report("scale preserved across re-create", sc, 0)
        print()

        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    # ── Round 2: hard reset, re-observe ──
    print("=== Round 2 — SYSRESETREQ, then observe T4..T5 ===")
    print()
    print("(Issuing AIRCR.SYSRESETREQ via Core 0 J-Link...)")
    jlink_sysresetreq()
    print("(Waiting 6 s for both cores + USB CDC to come up...)")
    time.sleep(6)

    with MokyaSwd() as swd:
        if not back_to_home(swd):
            print("!! Core 1 did not reach BOOT_HOME after reset; abort")
            return 1

        # T4 — read state without entering D-1, expecting BSS-zero again.
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        fcd = read_u8(swd, f_addr)
        print(f"[T4  post-reset, before D-1]")
        print(f"     s_first_create_done = {fcd}")
        print(f"     s.scale_idx         = {sc}")
        report("s_first_create_done back to 0 (BSS)", fcd, 0)
        report("s.scale_idx        back to 0 (BSS)", sc,  0)
        print()

        # T5 — enter D-1, scale should now go to SCALE_DEFAULT(2) again.
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        enter_d1(swd)
        v = read_view(swd)
        sc = read_u8(swd, s_addr + OFF_SCALE_IDX)
        fcd = read_u8(swd, f_addr)
        print(f"[T5  post-reset first D-1 entry]")
        print(f"     view                = {v} ({'MAP' if v==VIEW_ID_MAP else '?'})")
        print(f"     s_first_create_done = {fcd}")
        print(f"     s.scale_idx         = {sc}")
        report("s_first_create_done = 1", fcd, 1)
        report("s.scale_idx = SCALE_DEFAULT(2)", sc, 2)

        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    print("=== Diagnostic complete — interpret per docstring ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
