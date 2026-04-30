"""Diagnostic for the Phase 4 B-3 encoder failure.

Steps:
  1. Locate RTT up-buffer 0 via mokya_swd's existing _rtt_locate
     (extended for up-channel) and clear its RdOff=WrOff so subsequent
     traces are readable in isolation.
  2. Navigate to CHANNEL_ADD on slot 7.
  3. SWD-read channel_add_view's static `s` to confirm post-create
     state (psk_generated, etc.).
  4. SWD-write s.cursor=3, s.name="AuditCh", s.name_len=7, s.role=2.
  5. SWD-read s back to confirm writes took.
  6. Inject OK to fire_save().
  7. Sleep 500 ms.
  8. Read RTT up-buffer 0 — print all trace bytes that landed.
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
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME   = 0
VIEW_ID_CHANNELS    = 16
VIEW_ID_CHANNEL_ADD = 25

CADD_OFF_ACTIVE = 24
CADD_OFF_CURSOR = 25
CADD_OFF_NAME   = 26
CADD_OFF_NAMELEN = 38
CADD_OFF_ROLE   = 39
CADD_OFF_PSK    = 40
CADD_OFF_PSKGEN = 72


def find_static(elf, source_basename, name='s'):
    out = subprocess.check_output([ARM_OBJDUMP, "-t", elf], text=True)
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and source_basename in line:
            in_block = True; continue
        if in_block:
            if 'df *ABS*' in line: break
            m = re.match(rf'^([0-9a-fA-F]+)\s+l\s+O\s+\.bss\s+\S+\s+{re.escape(name)}$',
                         line)
            if m: return int(m.group(1), 16)
    return None


def send_press_release(swd, kc, hold_ms=30, gap_ms=200):
    pb = (kc & 0x7F) | 0x80
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([pb, 0])))
    time.sleep(hold_ms / 1000.0)
    rb = (kc & 0x7F)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([rb, 0])))
    time.sleep(gap_ms / 1000.0)


def back_home(swd):
    for _ in range(8):
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v == VIEW_ID_BOOT_HOME: return True
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
    return swd.read_u32(swd.symbol('s_view_router_active')) == VIEW_ID_BOOT_HOME


# ── RTT up-buffer 0 reader ────────────────────────────────────────────

class RttUp0:
    """Read SEGGER RTT up-channel 0 (default trace channel)."""
    _RTT_HDR  = 24
    _RTT_DESC = 24

    def __init__(self, swd):
        self.swd = swd
        cb = swd.symbol("_SEGGER_RTT")
        # up-buffer 0 starts right after the 24-byte header.
        d_off = cb + self._RTT_HDR + 0 * self._RTT_DESC
        d = swd._jl.memory_read32(d_off, 6)
        _, buf_ptr, size, wr, rd, _ = d
        self.buf_ptr = buf_ptr
        self.size = size
        self.wr_addr = d_off + 12
        self.rd_addr = d_off + 16

    def consume_all(self) -> bytes:
        wr = self.swd._jl.memory_read32(self.wr_addr, 1)[0]
        rd = self.swd._jl.memory_read32(self.rd_addr, 1)[0]
        if wr == rd:
            return b""
        if wr > rd:
            data = bytes(self.swd._jl.memory_read8(self.buf_ptr + rd, wr - rd))
        else:
            first = bytes(self.swd._jl.memory_read8(self.buf_ptr + rd, self.size - rd))
            second = bytes(self.swd._jl.memory_read8(self.buf_ptr, wr))
            data = first + second
        self.swd._jl.memory_write32(self.rd_addr, [wr])
        return data


def main():
    cadd_addr = find_static(ELF, 'channel_add_view.c', 's')
    print(f"channel_add_view.c  s @ 0x{cadd_addr:08x}")

    with MokyaSwd() as swd:
        rtt = RttUp0(swd)
        # Drain anything sitting in the up-buffer.
        pre = rtt.consume_all()
        print(f"  drained {len(pre)} stale bytes from RTT up-0")

        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! could not BOOT_HOME"); return 1

        # Navigate launcher → Chan → slot 7 → OK → CHANNEL_ADD
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],    gap_ms=400)
        for _ in range(8):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'], gap_ms=40)
        for _ in range(7):
            send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'], gap_ms=40)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        print(f"  view = {v} ({'CHANNEL_ADD' if v==VIEW_ID_CHANNEL_ADD else '?'})")

        # Read post-create state
        pre_state = swd.read_mem(cadd_addr, 76)
        print(f"  cadd_t after create() ({len(pre_state)} B):")
        print(f"    active_idx = {pre_state[CADD_OFF_ACTIVE]}")
        print(f"    cursor     = {pre_state[CADD_OFF_CURSOR]}")
        print(f"    name_len   = {pre_state[CADD_OFF_NAMELEN]}")
        print(f"    role       = {pre_state[CADD_OFF_ROLE]}")
        print(f"    psk_gen    = {pre_state[CADD_OFF_PSKGEN]}")
        print(f"    psk[0..7]  = {pre_state[CADD_OFF_PSK:CADD_OFF_PSK+8].hex()}")

        # SWD-write state
        name = b"AuditCh"
        ops = []
        for i, c in enumerate(name):
            ops.append((cadd_addr + CADD_OFF_NAME + i, c))
        ops.append((cadd_addr + CADD_OFF_NAME + len(name), 0))
        ops.append((cadd_addr + CADD_OFF_NAMELEN, len(name)))
        ops.append((cadd_addr + CADD_OFF_ROLE, 2))
        ops.append((cadd_addr + CADD_OFF_CURSOR, 3))
        swd.write_u8_many(ops)

        # Read back to confirm
        v_state = swd.read_mem(cadd_addr, 76)
        print(f"  after SWD-write:")
        print(f"    cursor     = {v_state[CADD_OFF_CURSOR]}")
        print(f"    name       = {bytes(v_state[CADD_OFF_NAME:CADD_OFF_NAME+8])!r}")
        print(f"    name_len   = {v_state[CADD_OFF_NAMELEN]}")
        print(f"    role       = {v_state[CADD_OFF_ROLE]}")

        # Drain trace one more time before Save (filter pre-Save spam)
        rtt.consume_all()

        # Inject OK
        print()
        print("  >>> injecting OK to fire fire_save() <<<")
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=200)
        time.sleep(0.5)

        # Read RTT trace
        trace = rtt.consume_all()
        print()
        print(f"  RTT up-0 captured {len(trace)} bytes:")
        try:
            print("  --- trace start ---")
            print(trace.decode('utf-8', errors='replace'))
            print("  --- trace end ---")
        except Exception as e:
            print(f"  decode error: {e}")
            print(f"  hex: {trace.hex()}")

        # Cleanup
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)


if __name__ == "__main__":
    sys.exit(main() or 0)
