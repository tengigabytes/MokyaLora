"""T2.2 tools-app live verify (SWD key-inject).

Exercises T-1 traceroute / T-6 GNSS sky / T-8 firmware info navigation
plus the T-1 OK-fires-traceroute path (encode succeeds, pending_pid
gets stamped).

Sequence:
  1. Reset to BOOT_HOME, FUNC -> launcher, navigate to Tools tile (1,2),
     OK -> VIEW_ID_TOOLS (12).
  2. UP × 11 to home cursor; verify each row entry (T-1, T-3 placeholder,
     T-6, T-8) navigates correctly via DOWN-and-OK presses.
  3. While in T-1, OK fires traceroute for cursor-row peer; check
     pending_pid stamps non-zero in traceroute_view::s.
  4. Returns to BOOT_HOME at the end.

Assumes mokya is booted with a fresh T2.2 build flashed.
"""
import sys
import time
import pathlib
import re
import subprocess

sys.path.insert(0, 'scripts')
from mokya_swd import MokyaSwd

KEYMAP = {}
src = pathlib.Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)', src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_NAMES = ['BOOT_HOME', 'LAUNCHER', 'MESSAGES', 'MESSAGES_CHAT',
              'MESSAGE_DETAIL', 'CANNED', 'NODES', 'NODE_DETAIL',
              'NODE_OPS', 'MY_NODE', 'SETTINGS', 'TELEMETRY',
              'CHANNELS', 'CHANNEL_EDIT', 'TOOLS',
              'TRACEROUTE', 'GNSS_SKY', 'FIRMWARE_INFO', 'IME',
              'KEYPAD', 'RF_DEBUG', 'FONT_TEST']

VIEW_ID_BOOT_HOME    = 0
VIEW_ID_LAUNCHER     = 1
VIEW_ID_TOOLS        = 14
VIEW_ID_TRACEROUTE   = 15
VIEW_ID_GNSS_SKY     = 16
VIEW_ID_FIRMWARE_INFO = 17

KEYI_MAGIC  = 0x4B45494A
RING_EVENTS = 32
EVENT_SIZE  = 2

# traceroute_view::s offsets — derived from struct layout in the .c
# (header, rows[5], divider, result[4]) = 11 ptrs * 4 = 44 bytes;
# then cursor (u32) at +44, scroll_top (u32) at +48, cache_seq at +52,
# pending_peer at +56, pending_pid at +60.
TRVIEW_S_PENDING_PEER_OFF = 56
TRVIEW_S_PENDING_PID_OFF  = 60


def find_static_s(elf, source_basename):
    out = subprocess.check_output([
        r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-objdump.exe",
        "-t", elf], text=True)
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and source_basename in line:
            in_block = True
            continue
        if in_block:
            if 'df *ABS*' in line:
                break
            m = re.match(r'^([0-9a-fA-F]+)\s+l\s+O\s+\.bss\s+\S+\s+s$', line)
            if m: return int(m.group(1), 16)
    return None


class KeyInject:
    def __init__(self, swd):
        self.swd = swd
        base = swd.symbol('g_key_inject_buf')
        self.base       = base
        self.magic_a    = base + 0
        self.producer_a = base + 4
        self.events_a   = base + 20
        self.s_active_a = swd.symbol('s_view_router_active')
        m = swd.read_u32(self.magic_a)
        assert m == KEYI_MAGIC

    def push_events(self, evs):
        producer = self.swd.read_u32(self.producer_a)
        ops = []
        for kb, fb in evs:
            slot = producer % RING_EVENTS
            base = self.events_a + slot * EVENT_SIZE
            ops.append((base + 0, kb & 0xFF))
            ops.append((base + 1, fb & 0xFF))
            producer += 1
        self.swd.write_u8_many(ops)
        self.swd.write_u32(self.producer_a, producer)

    def press(self, kc, hold_ms=30, gap_ms=200):
        self.push_events([(0x80 | (kc & 0x7F), 0)])
        time.sleep(hold_ms / 1000.0)
        self.push_events([(0x00 | (kc & 0x7F), 0)])
        time.sleep(gap_ms / 1000.0)

    def view(self):
        v = self.swd.read_u32(self.s_active_a)
        return v, VIEW_NAMES[v] if v < len(VIEW_NAMES) else f'?{v}'


def main():
    fail = 0
    elf = "build/core1_bridge/core1_bridge.elf"
    trview_s_addr = find_static_s(elf, "traceroute_view.c")
    if trview_s_addr is None:
        print("!! could not locate traceroute_view::s")
        return 1
    print(f"traceroute_view::s @ 0x{trview_s_addr:08x}")
    pending_peer_addr = trview_s_addr + TRVIEW_S_PENDING_PEER_OFF
    pending_pid_addr  = trview_s_addr + TRVIEW_S_PENDING_PID_OFF

    with MokyaSwd() as swd:
        ki = KeyInject(swd)

        def status(label):
            v, name = ki.view()
            print(f'[{label:<32}] view={v:2d}({name})')
            return v

        # Reset to BOOT_HOME via repeated BACK.
        v, _ = ki.view()
        for _ in range(6):
            if v == VIEW_ID_BOOT_HOME: break
            ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
            v, _ = ki.view()
        if v != VIEW_ID_BOOT_HOME:
            print(f'!! could not reach BOOT_HOME (v={v})')
            return 1
        status('start')

        # Navigate to Tools tile (1,2): FUNC -> launcher -> home
        # cursor with UP/LEFT (LRU may have stashed elsewhere) ->
        # DOWN once (row 1) -> RIGHT twice (col 2) -> OK.
        ki.press(KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        if status('FUNC -> launcher') != VIEW_ID_LAUNCHER:
            return 1
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=80)
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_LEFT'], gap_ms=80)
        ki.press(KEYMAP['MOKYA_KEY_DOWN'])
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        if status('OK on Tools tile') != VIEW_ID_TOOLS:
            print('!! Tools view not active'); return 1

        # Tools list: home cursor with UP × 11, then verify each entry.
        # Entry order in tools_view.c:
        #  0 T-1  -> TRACEROUTE
        #  1 T-2  -> placeholder
        #  2 T-3  -> placeholder
        #  3 T-4  -> placeholder
        #  4 T-5  -> placeholder
        #  5 T-6  -> GNSS_SKY
        #  6 T-7  -> placeholder
        #  7 T-8  -> FIRMWARE_INFO
        for _ in range(12): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
        status('cursor home (row 0 = T-1)')

        for label, downs, expect in [
            ('OK on T-1 -> TRACEROUTE',     0, VIEW_ID_TRACEROUTE),
            ('OK on T-6 -> GNSS_SKY',       5, VIEW_ID_GNSS_SKY),
            ('OK on T-8 -> FIRMWARE_INFO',  7, VIEW_ID_FIRMWARE_INFO),
        ]:
            # Re-enter Tools, home cursor, advance, OK.
            ki.press(KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)  # launcher
            for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
            for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
            ki.press(KEYMAP['MOKYA_KEY_DOWN'])
            ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
            ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
            ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=300)  # tools
            for _ in range(12): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
            for _ in range(downs): ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=60)
            ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=300)
            v = status(label)
            if v != expect:
                print(f'  !! expected view {expect} got {v}'); fail += 1

            if expect == VIEW_ID_TRACEROUTE:
                # Snapshot pre-OK pending state, fire OK, snapshot post.
                p_peer_pre = swd.read_u32(pending_peer_addr)
                p_pid_pre  = swd.read_u32(pending_pid_addr)
                print(f'  pre-OK  pending_peer={p_peer_pre} pending_pid={p_pid_pre:#x}')
                ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
                p_peer_post = swd.read_u32(pending_peer_addr)
                p_pid_post  = swd.read_u32(pending_pid_addr)
                print(f'  post-OK pending_peer={p_peer_post} pending_pid={p_pid_post:#x}')
                if p_peer_post == 0 or p_pid_post == 0:
                    print('  !! traceroute did not register pending state')
                    fail += 1
                else:
                    print(f'  OK fired traceroute peer=!{p_peer_post:08x} pid={p_pid_post:#x}')

            # Return to Tools then BACK to home before next iteration.
            ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
            ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)

        v, _ = ki.view()
        if v != VIEW_ID_BOOT_HOME:
            for _ in range(3):
                ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
                v, _ = ki.view()
                if v == VIEW_ID_BOOT_HOME: break
        status('end')

    if fail:
        print(f'\n=== T2.2 verify: {fail} FAIL ===')
        return 1
    print('\n=== T2.2 verify: PASS ===')
    return 0


if __name__ == '__main__':
    sys.exit(main())
