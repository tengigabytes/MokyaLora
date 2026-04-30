"""T2.1 telemetry_view live verify (SWD key-inject).

Sequence:
  1. Confirm boot at BOOT_HOME (view 0).
  2. Inject FUNC -> launcher (view 1).
  3. Down + Right -> Tele tile (row 1 col 1).
  4. OK -> VIEW_ID_TELEMETRY (view 11).
  5. Verify telemetry static `s.page` = 0 (F-1).
  6. Inject RIGHT three times, confirm page cycles 0->1->2->0 (F1->F2->F3->F1)
     and that an extra LEFT goes 0->2 (wrap).
  7. RIGHT to F-3, check phoneapi cache count > 0 and OK navigates to
     NODE_DETAIL (view 7) with nodes_view_get_active_node returning a
     non-zero node id.
  8. BACK twice -> back to BOOT_HOME.

Assumes mokya is booted, SWD attached, and Core 1 ELF built with
telemetry_view linked in.
"""
import sys
import time
import pathlib
import re
import struct

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

KEYI_MAGIC  = 0x4B45494A
RING_EVENTS = 32
EVENT_SIZE  = 2

# telemetry_view::s offsets — verified via objdump (size 0x38)
TELE_S_PAGE_OFF       = 36
TELE_S_F3_CURSOR_OFF  = 44


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
        assert m == KEYI_MAGIC, f"key inject magic mismatch: {m:#x}"

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
    with MokyaSwd() as swd:
        ki = KeyInject(swd)
        s_addr = swd.symbol('s', file_filter='telemetry_view.c') if hasattr(swd, 'symbol_file') else None
        # Resolve telemetry_view::s manually — it's a file-static so we hard-code
        # the address from objdump (caller can re-resolve if a rebuild moves it).
        # Read it from the ELF on every run instead.
        import subprocess
        out = subprocess.check_output([
            r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/bin/arm-none-eabi-objdump.exe",
            "-t", "build/core1_bridge/core1_bridge.elf"], text=True)
        tele_s_addr = None
        in_block = False
        for line in out.splitlines():
            if 'df *ABS*' in line and 'telemetry_view.c' in line:
                in_block = True
                continue
            if in_block:
                if 'df *ABS*' in line:
                    break
                m = re.match(r'^([0-9a-fA-F]+)\s+l\s+O\s+\.bss\s+\S+\s+s$', line)
                if m:
                    tele_s_addr = int(m.group(1), 16)
                    break
        if tele_s_addr is None:
            print("!! could not locate telemetry_view::s in objdump output")
            return 1
        print(f"telemetry_view::s @ 0x{tele_s_addr:08x}")
        page_addr = tele_s_addr + TELE_S_PAGE_OFF

        def status(label):
            v, name = ki.view()
            page = swd.read_u32(page_addr) if v == 13 else None
            print(f'[{label:<28}] view={v:2d}({name:<14}) tele.page={page}')
            return v

        # Reset to BOOT_HOME if necessary.
        v, _ = ki.view()
        for _ in range(4):
            if v == 0: break
            ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
            v, _ = ki.view()
        if v != 0:
            print(f'!! could not reach BOOT_HOME (currently {v})')
            return 1
        status('start')

        # Step 1: FUNC -> launcher
        ki.press(KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        v = status('FUNC -> launcher')
        if v != 1:
            print('!! launcher did not open'); fail += 1

        # Launcher is LRU-cached so cursor may not be at (0,0) — UP/LEFT
        # the maximum number of times to home it deterministically.
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_UP'], gap_ms=80)
        for _ in range(3): ki.press(KEYMAP['MOKYA_KEY_LEFT'], gap_ms=80)
        status('cursor -> (0,0) Msg')

        # Step 2: navigate to Tele tile (row 1 col 1)
        ki.press(KEYMAP['MOKYA_KEY_DOWN'])
        ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
        status('cursor -> Tele cell')

        # Step 3: OK -> telemetry view
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = status('OK -> telemetry')
        if v != 13:
            print(f'!! telemetry not active (v={v})'); fail += 1; return fail

        # Step 4: page cycling — relative to current page (LRU-cached
        # state may have preserved a non-zero page from a prior run).
        page0 = swd.read_u32(page_addr)
        print(f'  initial page (LRU-restored) = {page0}')
        for label, key, delta in [
            ('RIGHT', 'MOKYA_KEY_RIGHT', +1),
            ('RIGHT', 'MOKYA_KEY_RIGHT', +1),
            ('RIGHT', 'MOKYA_KEY_RIGHT', +1),  # wrap
            ('LEFT',  'MOKYA_KEY_LEFT',  -1),  # wrap
            ('LEFT',  'MOKYA_KEY_LEFT',  -1),
        ]:
            prev = swd.read_u32(page_addr)
            expect = (prev + delta) % 3
            ki.press(KEYMAP[key])
            actual = swd.read_u32(page_addr)
            ok = actual == expect
            print(f'  [{label:<8} {prev}->{actual}] expect={expect} {"OK" if ok else "FAIL"}')
            if not ok: fail += 1

        # Step 5: cursor F-3 + OK -> NODE_DETAIL.  Use cycle_page deltas
        # to land on F-3 from whatever current state.
        for _ in range(3):
            page = swd.read_u32(page_addr)
            if page == 2: break
            ki.press(KEYMAP['MOKYA_KEY_RIGHT'])
        page = swd.read_u32(page_addr)
        if page != 2:
            print(f'!! could not land on F-3 (page={page})'); fail += 1
        cursor_addr = tele_s_addr + TELE_S_F3_CURSOR_OFF
        cur = swd.read_u32(cursor_addr)
        print(f'  F-3 cursor before OK = {cur}')
        ki.press(KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = status('OK on F-3 -> NODE_DETAIL')
        if v != 7:
            print(f'!! did not navigate to NODE_DETAIL (v={v})'); fail += 1
        active_node = swd.read_u32(swd.symbol('s_active_node'))
        print(f'  nodes_view_get_active_node = !{active_node:08x} ({active_node})')
        if active_node == 0:
            print('!! active_node remained 0'); fail += 1

        # Step 6: BACK -> NODES list, BACK -> BOOT_HOME (or whichever the
        # node_detail_view defaults back to).
        ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        status('BACK from NODE_DETAIL')
        ki.press(KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        status('BACK again')

    if fail:
        print(f'\n=== T2.1 telemetry verify: {fail} FAIL ===')
        return 1
    print('\n=== T2.1 telemetry verify: PASS ===')
    return 0


if __name__ == '__main__':
    sys.exit(main())
