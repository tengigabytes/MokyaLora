"""T2.5 Remote Admin live verify (SWD key-inject).

Tests the C-3 OP_REMOTE_ADMIN navigation + arm/disarm state machine
without actually firing destructive actions (firing would reboot or
factory-reset ebe7; not safe in CI).

Sequence:
  1. Reset to BOOT_HOME, FUNC -> launcher, navigate to Nodes tile (0,2),
     OK -> NODES list. UP × N to home, find first non-self peer (cursor
     advance until cache.take_node_at(cursor) != my_node), OK -> C-2.
  2. OK -> C-3 (NODE_OPS). DOWN × 6 to land on OP_REMOTE_ADMIN row.
  3. OK -> VIEW_ID_REMOTE_ADMIN.
  4. Verify s.target_node_num == ebe7 (stash from nodes_view).
  5. Press OK once on row 0 -> armed_row = 0.
  6. Press UP -> armed_row = -1 (disarm via cursor move).
  7. Press OK twice -> armed_row would briefly = 0 then fire on second OK.
     SKIP THIS — actual fire reboots target.
  8. Press OK to arm row 0, press BACK -> armed_row = -1, view stays
     in REMOTE_ADMIN.
  9. BACK in idle state -> navigate back to NODE_OPS.

Real-fire verification is left as a manual test against a sacrificial
peer or the local node (with its own admin auth bypass).
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
              'NODE_OPS', 'REMOTE_ADMIN', 'MY_NODE', 'SETTINGS',
              'MODULES_INDEX', 'TELEMETRY', 'MAP', 'MAP_NAV',
              'CHANNELS', 'CHANNEL_EDIT', 'TOOLS', 'TRACEROUTE',
              'RANGE_TEST', 'GNSS_SKY', 'FIRMWARE_INFO', 'IME',
              'KEYPAD', 'RF_DEBUG', 'FONT_TEST']

VIEW_ID_BOOT_HOME    = 0
VIEW_ID_LAUNCHER     = 1
VIEW_ID_NODES        = 6
VIEW_ID_NODE_DETAIL  = 7
VIEW_ID_NODE_OPS     = 8
VIEW_ID_REMOTE_ADMIN = 9

KEYI_MAGIC  = 0x4B45494A
RING_EVENTS = 32
EVENT_SIZE  = 2

# remote_admin_view::s offsets — derived from struct layout:
# header(4) + rows[5](20) + status(4) + warn(4) = 32, then
# target_node_num u32 @ 32, cursor u8 @ 36, armed_row i8 @ 37,
# armed_deadline_ms u32 @ 40 (after 2-byte align gap).
RADM_S_TARGET_OFF      = 32
RADM_S_CURSOR_OFF      = 36
RADM_S_ARMED_ROW_OFF   = 37


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
    radm_s_addr = find_static_s(elf, "remote_admin_view.c")
    if radm_s_addr is None:
        print("!! could not locate remote_admin_view::s")
        return 1
    print(f"remote_admin_view::s @ 0x{radm_s_addr:08x}")

    with MokyaSwd() as swd:
        ki = KeyInject(swd)

        def status(label):
            v, name = ki.view()
            extra = ''
            if v == VIEW_ID_REMOTE_ADMIN:
                target = swd.read_u32(radm_s_addr + RADM_S_TARGET_OFF)
                cursor = swd.read_mem(radm_s_addr + RADM_S_CURSOR_OFF, 1)[0]
                armed_b = swd.read_mem(radm_s_addr + RADM_S_ARMED_ROW_OFF, 1)[0]
                armed = armed_b if armed_b < 128 else armed_b - 256
                extra = f' target=!{target:08x} cursor={cursor} armed={armed}'
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

        # FUNC -> launcher; home cursor; DOWN ×0, RIGHT ×2 -> Nodes (0,2)
        ki.press(KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        if status('FUNC -> launcher') != VIEW_ID_LAUNCHER: return 1
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        if status('OK on Nodes tile') != VIEW_ID_NODES: return 1

        # Pick the first non-self node from the list. The first row may
        # be ourselves (most-recently-heard sort puts us up top often
        # because we re-emit NodeInfo periodically).  Advance cursor
        # one step so we target ebe7 or another peer instead of self.
        ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=120)
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        if status('OK on Nodes row 1 -> NODE_DETAIL') != VIEW_ID_NODE_DETAIL:
            print('!! NODE_DETAIL not active'); fail += 1; return fail

        # OK on NODE_DETAIL -> NODE_OPS (per spec: enters C-3 ops menu).
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        if status('OK on NODE_DETAIL -> NODE_OPS') != VIEW_ID_NODE_OPS:
            print('!! NODE_OPS not active'); fail += 1; return fail

        # NODE_OPS rows: DM(0)/Alias(1)/Favorite(2)/Ignore(3)/Traceroute(4)
        # /RequestPos(5)/RemoteAdmin(6). DOWN × 6.
        for _ in range(6): ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=80)
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = status('OK on OP_REMOTE_ADMIN -> REMOTE_ADMIN')
        if v != VIEW_ID_REMOTE_ADMIN:
            print(f'!! REMOTE_ADMIN not active (v={v})'); fail += 1; return fail

        target = swd.read_u32(radm_s_addr + RADM_S_TARGET_OFF)
        if target == 0 or target == 0xFFFFFFFF:
            print(f'!! target unset / broadcast (=0x{target:08x})'); fail += 1

        # Move cursor to row 1 (Shutdown 5s) before arming — using row 1
        # avoids accidental fire if a UP/DOWN bug ever leaves armed_row
        # set on a re-issued OK (row 0 = Reboot would actually reboot
        # the target, undesirable for CI).  Also lets us test UP at a
        # non-boundary cursor.
        ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=120)
        cursor = swd.read_mem(radm_s_addr + RADM_S_CURSOR_OFF, 1)[0]
        if cursor != 1:
            print(f'!! cursor expected 1 got {cursor}'); fail += 1

        # Arm row 1 (Shutdown — same bool variant, won't fire from
        # any of the disarm tests below).
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=200)
        armed = swd.read_mem(radm_s_addr + RADM_S_ARMED_ROW_OFF, 1)[0]
        armed_signed = armed if armed < 128 else armed - 256
        if armed_signed != 1:
            print(f'!! after 1×OK on row 1 expected armed=1 got {armed_signed}')
            fail += 1
        else:
            print(f'  OK arm row 1 -> armed=1')

        # UP disarms (post-fix: always disarms in armed state, regardless
        # of whether cursor moves).
        ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=200)
        armed = swd.read_mem(radm_s_addr + RADM_S_ARMED_ROW_OFF, 1)[0]
        armed_signed = armed if armed < 128 else armed - 256
        if armed_signed != -1:
            print(f'!! after UP expected disarm (-1) got {armed_signed}')
            fail += 1
        else:
            print(f'  OK UP disarmed (armed=-1)')

        # Re-arm row 1 (cursor already at 1; UP in armed state did NOT
        # move cursor since disarm pre-empted the move). Press DOWN if
        # cursor isn't 1 (defensive).
        cursor = swd.read_mem(radm_s_addr + RADM_S_CURSOR_OFF, 1)[0]
        if cursor != 1:
            # Cursor moved unexpectedly. Bring it back.
            for _ in range(5): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=80)
            ki.press(KEYMAP['MOKYA_KEY_DOWN'], gap_ms=120)
        # First OK arms row 1 again (cursor=1, armed=-1 → arm).
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=200)
        armed = swd.read_mem(radm_s_addr + RADM_S_ARMED_ROW_OFF, 1)[0]
        if (armed if armed < 128 else armed - 256) != 1:
            print(f'  re-arm failed (armed={armed})'); fail += 1
        ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=200)
        v = status('BACK after arm: still in REMOTE_ADMIN, disarmed')
        armed = swd.read_mem(radm_s_addr + RADM_S_ARMED_ROW_OFF, 1)[0]
        armed_signed = armed if armed < 128 else armed - 256
        if v != VIEW_ID_REMOTE_ADMIN or armed_signed != -1:
            print(f'  !! expected REMOTE_ADMIN view + armed=-1 got '
                  f'view={v} armed={armed_signed}')
            fail += 1
        else:
            print(f'  OK BACK disarmed without nav')

        # BACK in idle state -> NODE_OPS
        ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        v = status('BACK in idle -> NODE_OPS')
        if v != VIEW_ID_NODE_OPS:
            print(f'!! expected NODE_OPS got {v}'); fail += 1

    if fail:
        print(f'\n=== T2.5 verify: {fail} FAIL ===')
        return 1
    print('\n=== T2.5 verify: PASS ===')
    print('NOTE: actual destructive fire (reboot/shutdown/reset) is')
    print('      not exercised by this script — verify manually.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
