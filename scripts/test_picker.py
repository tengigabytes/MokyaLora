#!/usr/bin/env python3
"""test_picker.py — SWD smoke test for the SYM1 long-press symbol picker.

Sequence:
  1. Reset committed text + pending state.
  2. Cycle to the IME view.
  3. Long-press SYM1 (press, wait > 500 ms wall-clock for tick(), release).
  4. Inject DPAD RIGHT × `cell_idx` to navigate to the target cell, then OK.
  5. Read g_text via the IME debug snapshot; verify the expected symbol
     was committed at the cursor.

Cycles through every cell in the 4×4 grid so a single run covers the
entire picker. Reports pass/fail per cell.
"""
import sys
import time
import struct
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass

# Mirror of ime_logic.cpp::kSymPickerCells_ — must stay in sync.
PICKER_CELLS = [
    "「", "」", "『", "』",
    "（", "）", "【", "】",
    "，", "。", "、", "；",
    "：", "？", "！", "…",
]
PICKER_COLS = 4

RING_EVENTS = 32
EVENT_SIZE  = 2
KEYI_MAGIC  = 0x4B45494A   # "KEYJ"

IME_DBG_ADDR    = 0x2007FE00
IME_DBG_MAGIC   = 0xEEED0003
IME_DBG_SIZE    = 0x190
OFF_TEXT_LEN    = 0xF8
OFF_TEXT_BUF    = 0x100
TEXT_BUF_LEN    = 96


def load_keymap():
    src = Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
    import re
    out = {}
    for m in re.finditer(
            r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
            src):
        out[m.group(1)[len('MOKYA_'):]] = int(m.group(2), 16)
    return out


class PickerDriver:
    def __init__(self, swd, km):
        self.swd = swd
        self.km  = km
        base = swd.symbol('g_key_inject_buf')
        self.a_magic    = base
        self.a_producer = base + 4
        self.a_consumer = base + 8
        self.a_events   = base + 20
        self.a_active   = swd.symbol('s_view_router_active')

    def queue_events(self, events):
        idx = 0
        n = len(events)
        cap = RING_EVENTS - 2
        while idx < n:
            prod = self.swd.read_u32(self.a_producer)
            cons = self.swd.read_u32(self.a_consumer)
            while prod - cons >= cap:
                time.sleep(0.005)
                cons = self.swd.read_u32(self.a_consumer)
            free = cap - (prod - cons)
            take = min(free, n - idx)
            writes = []
            for (kb, fb) in events[idx:idx + take]:
                slot = prod % RING_EVENTS
                writes.append((self.a_events + slot * EVENT_SIZE + 0, kb))
                writes.append((self.a_events + slot * EVENT_SIZE + 1, fb))
                prod += 1
            self.swd.write_u8_many(writes)
            self.swd.write_u32(self.a_producer, prod)
            idx += take
        deadline = time.time() + 2.0
        target = self.swd.read_u32(self.a_producer)
        while time.time() < deadline:
            if self.swd.read_u32(self.a_consumer) >= target: return
            time.sleep(0.005)
        raise RuntimeError("inject timeout")

    def press_release(self, kc, flags=0, hold_ms=0):
        """Send a press + (optional wall-clock hold) + release."""
        self.queue_events([(0x80 | (kc & 0x7F), flags & 0xFF)])
        if hold_ms > 0:
            time.sleep(hold_ms / 1000.0)
        self.queue_events([(0x00 | (kc & 0x7F), flags & 0xFF)])

    def read_text(self):
        blob = self.swd.read_mem(IME_DBG_ADDR, IME_DBG_SIZE)
        magic = struct.unpack_from('<I', blob, 0)[0]
        if magic != IME_DBG_MAGIC:
            raise RuntimeError(f'bad ime debug magic {magic:#x}')
        tlen = struct.unpack_from('<i', blob, OFF_TEXT_LEN)[0]
        end = blob.find(b'\x00', OFF_TEXT_BUF, OFF_TEXT_BUF + TEXT_BUF_LEN)
        if end < 0: end = OFF_TEXT_BUF + TEXT_BUF_LEN
        return tlen, blob[OFF_TEXT_BUF:end].decode('utf-8', errors='replace')

    def reset_text(self):
        for _ in range(40):
            tlen, _ = self.read_text()
            if tlen == 0: return
            self.queue_events(
                [(0x80 | self.km['KEY_DEL'], 0), (0x00 | self.km['KEY_DEL'], 0)] * 8)
        raise RuntimeError('reset failed')

    def cycle_to_ime(self):
        for _ in range(8):
            cur = self.swd.read_u32(self.a_active)
            if cur == 3: return  # IME view index
            self.press_release(self.km['KEY_FUNC'])
            time.sleep(0.05)
        raise RuntimeError('IME view not reachable')


def main():
    km = load_keymap()
    failures = []

    with MokyaSwd() as swd:
        drv = PickerDriver(swd, km)
        m = swd.read_u32(drv.a_magic)
        if m != KEYI_MAGIC:
            print(f'inject magic 0x{m:08X} != 0x{KEYI_MAGIC:08X}')
            sys.exit(1)
        drv.cycle_to_ime()

        for cell_idx, expected in enumerate(PICKER_CELLS):
            drv.reset_text()
            t0_len, _ = drv.read_text()

            # Long-press SYM1: hold for 600 ms so the IME tick (20 ms cadence)
            # crosses the 500 ms picker threshold. Using flags=0 because
            # SYM1 is not a slot key; the engine's own SYM1 long-press timer
            # uses now_ms deltas.
            drv.press_release(km['KEY_SYM1'], flags=0, hold_ms=600)
            time.sleep(0.05)

            # Navigate from cell 0 to cell_idx with RIGHT presses.
            for _ in range(cell_idx):
                drv.press_release(km['KEY_RIGHT'])
            time.sleep(0.05)

            # Commit.
            drv.press_release(km['KEY_OK'])
            time.sleep(0.1)

            tlen, text = drv.read_text()
            grown = tlen - t0_len
            ok = text.endswith(expected) and grown == len(expected.encode('utf-8'))
            mark = 'OK' if ok else 'FAIL'
            print(f'  cell {cell_idx:2d} = {expected}  → text={text!r}  '
                  f'grown={grown} (expected {len(expected.encode("utf-8"))})  {mark}')
            if not ok:
                failures.append((cell_idx, expected, text))

    print()
    if failures:
        print(f'{len(failures)} / {len(PICKER_CELLS)} FAILED:')
        for c, e, t in failures:
            print(f'  cell {c} expected {e!r} got tail {t[-6:]!r}')
        sys.exit(1)
    print(f'All {len(PICKER_CELLS)} picker cells commit correctly.')


if __name__ == '__main__':
    main()
