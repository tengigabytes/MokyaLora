"""T2.4 modules-index live verify (SWD key-inject).

Sequence:
  1. Reset to BOOT_HOME, FUNC -> launcher, navigate to Set tile (2,0),
     OK -> VIEW_ID_SETTINGS (10).
  2. RIGHT (deep-link key) -> VIEW_ID_MODULES_INDEX (11).
  3. UP × ROW_COUNT to home cursor at row 0 (S-7.1 Canned Message).
  4. OK -> VIEW_ID_SETTINGS again, but now settings_app_view::s.cursor
     should point at the Canned Message group node (slot 1+SG_CANNED_MSG).
  5. From the group, BACK -> root.  Then RIGHT to re-enter modules_index.
  6. DOWN once to row 1 (S-7.2 External Notif, TBD), OK -> stays in
     MODULES_INDEX, status label updates with "TBD".
"""
import sys
import time
import pathlib
import re
import subprocess
import struct

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

VIEW_ID_BOOT_HOME     = 0
VIEW_ID_LAUNCHER      = 1
VIEW_ID_SETTINGS      = 10
VIEW_ID_MODULES_INDEX = 11

# settings_keys.h enum: SG_CANNED_MSG = 12 (after DEVICE/LORA/POSITION/POWER/
# DISPLAY/CHANNEL/OWNER/SECURITY/TELEMETRY/NEIGHBOR/RANGE_TEST/DETECT_SENSOR).
# Group nodes occupy slots 1..15 in settings_tree.c, so SG_CANNED_MSG -> slot 13.
SG_CANNED_MSG          = 12
SG_CANNED_MSG_TREE_SLOT = 1 + SG_CANNED_MSG  # 13

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


def find_symbol_addr(elf, name):
    out = subprocess.check_output([
        r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-nm.exe",
        elf], text=True)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == name:
            return int(parts[0], 16)
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

    # settings_app_view::s — pointer to "panel" then "bc_lbl" then "rows[12]"
    # (12 ptrs * 4 = 48), then "cursor" (settings_tree_node_t* — 4 B).
    # First 14 ptrs * 4 = 56 bytes header before the cursor pointer.
    # Verify by reading objdump for the actual size; cursor is the 15th
    # field in settings_app_t.
    settings_s_addr = find_static_s(elf, "settings_app_view.c")
    if settings_s_addr is None:
        print("!! could not locate settings_app_view::s")
        return 1
    print(f"settings_app_view::s @ 0x{settings_s_addr:08x}")

    # Locate s_nodes (the settings tree static node array) so we can
    # decode the cursor pointer as a slot index.  s_nodes is a static
    # in settings_tree.c — find via objdump.
    s_nodes_addr = find_static_s(elf, "settings_tree.c")
    # In settings_tree.c the static "s_nodes" is renamed (it's the array
    # of struct settings_tree_node).  Find it directly by name.
    if s_nodes_addr is None:
        # Try via nm for a non-pointer static
        out = subprocess.check_output([
            r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-objdump.exe",
            "-t", elf], text=True)
        for line in out.splitlines():
            m = re.match(r'^([0-9a-fA-F]+)\s+l\s+O\s+\.bss\s+\S+\s+s_nodes$', line)
            if m:
                s_nodes_addr = int(m.group(1), 16)
                break
    if s_nodes_addr is None:
        print("!! could not locate settings_tree::s_nodes")
        return 1
    print(f"settings_tree::s_nodes @ 0x{s_nodes_addr:08x}")

    # settings_app_t cursor offset: panel(4) + bc_lbl(4) + rows[12](48)
    # = 56 bytes.
    SETTINGS_S_CURSOR_OFF = 4 + 4 + 12 * 4   # 56

    with MokyaSwd() as swd:
        ki = KeyInject(swd)

        def status(label):
            v, name = ki.view()
            extra = ''
            if v == VIEW_ID_SETTINGS:
                cur = swd.read_u32(settings_s_addr + SETTINGS_S_CURSOR_OFF)
                if cur:
                    # node size = 16 B per settings_tree.c
                    slot = (cur - s_nodes_addr) // 16
                    extra = f' cursor_slot={slot}'
                else:
                    extra = ' cursor=NULL'
            print(f'[{label:<32}] view={v:2d}({name}){extra}')
            return v

        # Reset to BOOT_HOME
        v, _ = ki.view()
        for _ in range(8):
            if v == VIEW_ID_BOOT_HOME: break
            ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
            v, _ = ki.view()
        if v != VIEW_ID_BOOT_HOME:
            print(f'!! could not reach BOOT_HOME (v={v})'); return 1
        status('start')

        # FUNC -> launcher; home cursor; DOWN ×2, RIGHT ×0 -> Set (2,0); OK
        ki.press(KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        if status('FUNC -> launcher') != VIEW_ID_LAUNCHER:
            return 1
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        ki.press(KEYMAP['MOKYA_KEY_DOWN']); ki.press(KEYMAP['MOKYA_KEY_DOWN'])
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        if status('OK on Set tile') != VIEW_ID_SETTINGS:
            print('!! settings view not active'); return 1

        # RIGHT to deep-link into MODULES_INDEX
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=300)
        if status('RIGHT -> modules_index') != VIEW_ID_MODULES_INDEX:
            print('!! modules_index not active'); fail += 1; return fail

        # Home cursor; OK on row 0 (S-7.1 Canned Message - WIRED)
        for _ in range(11): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = status('OK on S-7.1 -> SETTINGS')
        if v != VIEW_ID_SETTINGS:
            print(f'!! expected SETTINGS got {v}'); fail += 1
        else:
            cur = swd.read_u32(settings_s_addr + SETTINGS_S_CURSOR_OFF)
            slot = (cur - s_nodes_addr) // 16 if cur else -1
            if slot != SG_CANNED_MSG_TREE_SLOT:
                print(f'!! cursor slot={slot} expected {SG_CANNED_MSG_TREE_SLOT} '
                      f'(SG_CANNED_MSG)'); fail += 1
            else:
                print(f'  OK cursor parked at SG_CANNED_MSG (slot {slot})')

        # BACK -> root (cursor was at group, so BACK climbs one level)
        ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        v = status('BACK in settings -> root')
        if v != VIEW_ID_SETTINGS:
            print(f'!! still in settings after BACK (v={v})'); fail += 1
        else:
            cur = swd.read_u32(settings_s_addr + SETTINGS_S_CURSOR_OFF)
            slot = (cur - s_nodes_addr) // 16 if cur else -1
            if slot != 0:
                print(f'!! cursor slot={slot} expected 0 (root)'); fail += 1

        # RIGHT again -> modules_index, DOWN to row 1 (S-7.2 TBD), OK
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=300)
        if status('RIGHT -> modules_index again') != VIEW_ID_MODULES_INDEX:
            fail += 1
        ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=200)
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=300)
        v = status('OK on S-7.2 (TBD)')
        if v != VIEW_ID_MODULES_INDEX:
            print(f'!! TBD row should not navigate (v={v})'); fail += 1

    if fail:
        print(f'\n=== T2.4 verify: {fail} FAIL ===')
        return 1
    print('\n=== T2.4 verify: PASS ===')
    return 0


if __name__ == '__main__':
    sys.exit(main())
