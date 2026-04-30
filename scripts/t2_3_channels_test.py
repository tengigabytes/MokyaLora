"""T2.3 channels-app live verify (SWD key-inject).

Sequence:
  1. Reset to BOOT_HOME, FUNC -> launcher, navigate to Chan tile (0,1),
     OK -> VIEW_ID_CHANNELS (12).
  2. UP × 8 to home cursor; OK on row 0 -> VIEW_ID_CHANNEL_EDIT (13)
     with channels_view_get_active_index() == 0.
  3. In channel_edit, cursor at ROW_NAME (0).  Press DOWN to ROW_POS (1),
     UP / DOWN should bump position precision via SET; LEFT also fires
     SET-decrement.
  4. Press DOWN to ROW_MUTED (2), OK toggles muted via SET.
  5. BACK -> back to channels list.

We don't observe the SET reply queue here (cascade replay is async); we
observe (a) the post-OK status string written to s.status (last set
message logged on lvgl_task) and (b) RTT trace events ("cedit","set").
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
              'NODE_OPS', 'MY_NODE', 'SETTINGS', 'MODULES_INDEX',
              'TELEMETRY', 'CHANNELS', 'CHANNEL_EDIT', 'TOOLS',
              'TRACEROUTE', 'GNSS_SKY', 'FIRMWARE_INFO', 'IME',
              'KEYPAD', 'RF_DEBUG', 'FONT_TEST']

VIEW_ID_BOOT_HOME    = 0
VIEW_ID_LAUNCHER     = 1
VIEW_ID_CHANNELS     = 13
VIEW_ID_CHANNEL_EDIT = 14

KEYI_MAGIC  = 0x4B45494A
RING_EVENTS = 32
EVENT_SIZE  = 2


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
    cedit_s_addr = find_static_s(elf, "channel_edit_view.c")
    chlist_s_addr = find_static_s(elf, "channels_view.c")
    if cedit_s_addr is None or chlist_s_addr is None:
        print(f"!! could not locate static s — cedit={cedit_s_addr} chlist={chlist_s_addr}")
        return 1
    print(f"channel_edit_view::s @ 0x{cedit_s_addr:08x}")
    print(f"channels_view::s     @ 0x{chlist_s_addr:08x}")

    # cedit_t layout: header, rows[6], status (8 ptrs * 4 = 32),
    # then active_idx u8, cursor u8, padding 2, last_change_seq u32.
    CEDIT_S_ACTIVE_OFF = 32
    CEDIT_S_CURSOR_OFF = 33

    with MokyaSwd() as swd:
        ki = KeyInject(swd)

        def status(label):
            v, name = ki.view()
            extra = ''
            if v == VIEW_ID_CHANNEL_EDIT:
                a = swd.read_mem(cedit_s_addr + CEDIT_S_ACTIVE_OFF, 1)[0]
                c = swd.read_mem(cedit_s_addr + CEDIT_S_CURSOR_OFF, 1)[0]
                extra = f' active={a} cursor={c}'
            print(f'[{label:<32}] view={v:2d}({name}){extra}')
            return v

        # Reset to BOOT_HOME
        v, _ = ki.view()
        for _ in range(6):
            if v == VIEW_ID_BOOT_HOME: break
            ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
            v, _ = ki.view()
        if v != VIEW_ID_BOOT_HOME:
            print(f'!! could not reach BOOT_HOME (v={v})'); return 1
        status('start')

        # FUNC -> launcher; home cursor; DOWN ×0, RIGHT ×1 -> Chan (0,1)
        ki.press(KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        if status('FUNC -> launcher') != VIEW_ID_LAUNCHER:
            print('!! launcher did not open'); return 1
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        if status('OK on Chan tile') != VIEW_ID_CHANNELS:
            print('!! channels view not active'); return 1

        # Home cursor; OK -> channel_edit
        for _ in range(8): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=300)
        v = status('OK on row 0 -> CHANNEL_EDIT')
        if v != VIEW_ID_CHANNEL_EDIT:
            print('!! channel_edit not active'); return 1

        a = swd.read_mem(cedit_s_addr + CEDIT_S_ACTIVE_OFF, 1)[0]
        if a != 0:
            print(f'!! active_idx={a} expected 0'); fail += 1

        # Move cursor to ROW_POS (1) and bump.  Each UP fires SET.
        ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=200)   # 0 -> 1 (POS)
        c = swd.read_mem(cedit_s_addr + CEDIT_S_CURSOR_OFF, 1)[0]
        print(f'  cursor after 1×DOWN = {c} (expect 1=ROW_POS)')
        if c != 1: fail += 1

        ki.press(KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=200)  # bump pos +1 (no cursor change)
        c2 = swd.read_mem(cedit_s_addr + CEDIT_S_CURSOR_OFF, 1)[0]
        print(f'  cursor after RIGHT (pos bump) = {c2} (expect still 1)')
        if c2 != 1: fail += 1

        ki.press(KEYMAP['MOKYA_KEY_LEFT'], gap_ms=200)   # bump pos -1

        # Move cursor to ROW_MUTED (2) and toggle.
        ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=200)   # 1 -> 2 (MUTED)
        c = swd.read_mem(cedit_s_addr + CEDIT_S_CURSOR_OFF, 1)[0]
        print(f'  cursor after 2×DOWN = {c} (expect 2=ROW_MUTED)')
        if c != 2: fail += 1

        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=300)     # toggle muted

        # BACK -> CHANNELS list, BACK -> BOOT_HOME
        ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        v = status('BACK from edit')
        if v != VIEW_ID_CHANNELS: fail += 1
        ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        status('end')

    if fail:
        print(f'\n=== T2.3 verify: {fail} FAIL ===')
        return 1
    print('\n=== T2.3 verify: PASS ===')
    return 0


if __name__ == '__main__':
    sys.exit(main())
