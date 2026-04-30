"""T1.2 — Draft restore live verify (v2: SWD-write g_text bypass).

Skips MIE input layer (default mode is Bopomofo, would need mode-switch to
type ASCII directly). Instead:
    Navigate to MESSAGES_CHAT -> OK to open IME -> SWD-write g_text="hello",
    g_text_len=5 -> BACK triggers modal_trampoline -> draft_store_save flash op
    -> verify slot in flash partition.
    Then BACK to chat_list -> OK re-enter MESSAGES_CHAT -> OK re-open IME ->
    expect ime_request_text loads draft via draft_store_load -> g_text == 'hello'.
    Then FUNC short -> commit -> draft_store_clear -> re-enter -> empty.

Does not test the natural typing flow.
"""
import sys
import time
import struct

sys.path.insert(0, 'scripts')
from mokya_swd import MokyaSwd

import re
import pathlib

KEYMAP = {}
src = pathlib.Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)', src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_NAMES = ['BOOT_HOME', 'LAUNCHER', 'MESSAGES', 'MESSAGES_CHAT',
              'MESSAGE_DETAIL', 'CANNED', 'NODES', 'NODE_DETAIL',
              'NODE_OPS', 'REMOTE_ADMIN', 'MY_NODE', 'SETTINGS',
              'MODULES_INDEX', 'TELEMETRY', 'CHANNELS', 'CHANNEL_EDIT',
              'TOOLS', 'TRACEROUTE', 'GNSS_SKY', 'FIRMWARE_INFO',
              'IME', 'KEYPAD', 'RF_DEBUG', 'FONT_TEST']
VIEW_ID_IME = 20   # bumps each time a non-modal view is inserted

KEYI_MAGIC  = 0x4B45494A
RING_EVENTS = 32
EVENT_SIZE  = 2

# Anonymous-namespace symbols from ime_task.cpp
G_TEXT_ADDR     = 0x20066cb8
G_TEXT_LEN_ADDR = 0x20064384
G_CURSOR_ADDR   = 0x200674bc

DRAFT_PARTITION = 0x10C10000
DRAFT_SLOT_SIZE = 4096
DRAFT_SLOT_COUNT = 16
DRAFT_MAGIC = 0x54465244


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


def read_g_text(swd):
    tl = struct.unpack('<i', swd.read_mem(G_TEXT_LEN_ADDR, 4))[0]
    if tl < 0: tl = 0
    if tl > 256: tl = 256
    raw = swd.read_mem(G_TEXT_ADDR, max(tl, 1))
    text = raw[:tl].decode('utf-8', errors='replace')
    return tl, text, raw[:min(16, max(tl, 1))]


def write_g_text(swd, text):
    """Override g_text + g_text_len + g_cursor via SWD. Caller should ensure
    IME modal is active so modal_trampoline picks up these values on BACK."""
    data = text.encode('utf-8')
    n = len(data)
    swd.write_u8_many([(G_TEXT_ADDR + i, b) for i, b in enumerate(data)])
    swd.write_u8_many([(G_TEXT_ADDR + n, 0)])  # NUL term
    swd.write_u32(G_TEXT_LEN_ADDR, n)
    swd.write_u32(G_CURSOR_ADDR, n)


def find_draft_slot(swd, draft_id):
    for i in range(DRAFT_SLOT_COUNT):
        addr = DRAFT_PARTITION + i * DRAFT_SLOT_SIZE
        hdr = swd.read_mem(addr, 16)
        magic, did, tlen, _resv, _crc = struct.unpack('<IIHHI', hdr)
        if magic == DRAFT_MAGIC and did == draft_id:
            text_bytes = swd.read_mem(addr + 16, tlen) if tlen > 0 else b''
            return i, tlen, text_bytes.decode('utf-8', errors='replace')
    return None, 0, ''


def main():
    EBE7 = 1401875431
    DRAFT_PAYLOAD = "hello"

    with MokyaSwd() as swd:
        ki = KeyInject(swd)

        def status(label):
            v, name = ki.view()
            tl, tt, raw = read_g_text(swd)
            print(f'[{label:<24}] view={v:2d}({name:<14}) tl={tl:3d} text={tt!r} raw={raw.hex()}')

        def draft_status(label):
            slot, tlen, txt = find_draft_slot(swd, EBE7)
            if slot is None:
                print(f'[{label:<24}] draft_store: NO slot for {EBE7}')
            else:
                print(f'[{label:<24}] draft_store: slot={slot} len={tlen} text={txt!r}')

        status('start')
        draft_status('start')
        v0, _ = ki.view()
        if v0 != 0:
            print(f'!! NOT at BOOT_HOME (view={v0}). Test assumes start at BOOT_HOME.')
            return

        # === Navigate down to IME open ===
        for label, key in [
            ('FUNC -> launcher', 'MOKYA_KEY_FUNC'),
            ('OK   -> messages', 'MOKYA_KEY_OK'),
            ('OK   -> conv',     'MOKYA_KEY_OK'),
            ('OK   -> open IME', 'MOKYA_KEY_OK'),
        ]:
            ki.press(KEYMAP[key], hold_ms=30, gap_ms=300)
            status(label)

        v, _ = ki.view()
        if v != VIEW_ID_IME:
            print(f'!! IME not open (view={v}). Aborting.')
            return

        # === SWD-write g_text = "hello" ===
        print(f'\n--- SWD-write g_text = {DRAFT_PAYLOAD!r} ---')
        write_g_text(swd, DRAFT_PAYLOAD)
        status('after SWD write')

        # === BACK -> modal close -> draft_store_save flash op ===
        print('\n--- BACK in IME -> save draft ---')
        ki.press(KEYMAP['MOKYA_KEY_BACK'], hold_ms=30, gap_ms=800)
        status('after BACK (IME)')
        draft_status('after BACK (IME)')

        # === BACK -> chat list ===
        ki.press(KEYMAP['MOKYA_KEY_BACK'], hold_ms=30, gap_ms=300)
        status('after BACK (conv)')

        # === Re-enter conv + open IME -> expect g_text restored ===
        print('\n--- OK -> re-enter MESSAGES_CHAT ---')
        ki.press(KEYMAP['MOKYA_KEY_OK'], hold_ms=30, gap_ms=300)
        status('after OK (re-enter)')

        print('\n--- OK -> re-open IME, expect text restored ---')
        ki.press(KEYMAP['MOKYA_KEY_OK'], hold_ms=30, gap_ms=400)
        v, name = ki.view()
        tl, tt, raw = read_g_text(swd)
        print(f'[after re-open IME       ] view={v:2d}({name:<14}) tl={tl:3d} text={tt!r} raw={raw.hex()}')
        if v == VIEW_ID_IME and tt == DRAFT_PAYLOAD:
            print(f'==> PASS: draft restored as {tt!r}')
        else:
            print(f'==> FAIL: expected {DRAFT_PAYLOAD!r}, got {tt!r}')

        # === FUNC -> commit, draft_store_clear ===
        print('\n--- FUNC short to commit (clears draft) ---')
        ki.press(KEYMAP['MOKYA_KEY_FUNC'], hold_ms=30, gap_ms=800)
        status('after FUNC commit')
        draft_status('after commit')

        # Note: FUNC commit will also call messages_send_text("hello") which
        # tries to TX over LoRa. That's OK side-effect for the test.

        # === Re-enter conv + OK + OK -> expect g_text empty ===
        print('\n--- BACK -> OK -> OK re-test empty ---')
        ki.press(KEYMAP['MOKYA_KEY_BACK'], hold_ms=30, gap_ms=300)
        status('after BACK')
        ki.press(KEYMAP['MOKYA_KEY_OK'], hold_ms=30, gap_ms=300)
        status('after OK')
        ki.press(KEYMAP['MOKYA_KEY_OK'], hold_ms=30, gap_ms=400)
        v, name = ki.view()
        tl, tt, raw = read_g_text(swd)
        print(f'[final IME               ] view={v:2d}({name:<14}) tl={tl:3d} text={tt!r} raw={raw.hex()}')
        if v == VIEW_ID_IME and tl == 0:
            print(f'==> PASS: empty after commit')
        else:
            print(f'==> FAIL: expected empty, got tl={tl} text={tt!r}')

        # Cleanup: BACK out of IME and conversation
        ki.press(KEYMAP['MOKYA_KEY_BACK'], hold_ms=30, gap_ms=300)
        ki.press(KEYMAP['MOKYA_KEY_BACK'], hold_ms=30, gap_ms=300)
        ki.press(KEYMAP['MOKYA_KEY_BACK'], hold_ms=30, gap_ms=300)
        status('cleanup')


if __name__ == '__main__':
    main()
