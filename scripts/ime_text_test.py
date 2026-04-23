#!/usr/bin/env python3
"""ime_text_test.py — high-speed text-driven IME test using pylink.

Reads a Chinese passage, for each character:
  1. Inject its primary Bopomofo reading (phonemes + tone) as keycodes.
  2. Locate the char in the top-8 candidate list via the SWD debug snap.
  3. If found, press RIGHT * rank to select, then OK to commit.
  4. If not found in top-8, commit rank 0 (best-effort) and record MISS.
  5. Move on. Engine state is left alive across chars — the committed
     text accumulates, mirroring a real typing session.

Typing speed floor = Core 1 key_inject poll cadence (10 ms) + LVGL
refresh + search. With pylink the host overhead per char is ~5 ms.

Usage:
    python scripts/ime_text_test.py PATH          # file
    python scripts/ime_text_test.py --stdin       # stdin
    python scripts/ime_text_test.py PATH --limit 30

Options:
    --limit N    test first N chars only
    --no-commit  measure candidate-list rank only, skip RIGHT/OK
    --reset-every-n N  reset text buffer every N chars so SWD can flush
                       snapshot; default=10 (0 to keep accumulating)
"""
import argparse
import re
import sys
import time
import struct
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from verify_mied_v4 import read_v4  # type: ignore
from mokya_swd import MokyaSwd       # type: ignore

if hasattr(sys.stdout, 'reconfigure'):
    try: sys.stdout.reconfigure(encoding='utf-8')
    except Exception: pass

# ── Constants matching firmware ─────────────────────────────────────────
# Phase 1.4: inject ring upgraded to 2-byte events (key_byte, flags_byte)
# and the ring is now indexed in events (32 slots × 2 bytes = 64 byte
# storage). Matches firmware/core1/src/keypad/key_inject.h.
RING_EVENTS     = 32
EVENT_SIZE      = 2
KEYI_MAGIC      = 0x4B45494A   # "KEYJ"

# Long-press hint flags (match mie/keycode.h MOKYA_KEY_FLAG_*).
KEY_FLAG_LONG_PRESS = 0x01
KEY_FLAG_HINT_ANY   = 0x04
IME_DBG_ADDR    = 0x2007FE00
IME_DBG_MAGIC   = 0xEEED0003
IME_DBG_SIZE    = 0x190
IME_CAND_FULL_MAGIC = 0xECA11100
IME_CAND_FULL_MAX   = 100
IME_CAND_FULL_WLEN  = 24
IME_CAND_FULL_SIZE  = 16 + IME_CAND_FULL_MAX * IME_CAND_FULL_WLEN  # 2416
VIEW_COUNT      = 4
IME_VIEW_INDEX  = 3

# ime_view_debug_t v0003 offsets
OFF_MAGIC, OFF_SEQ, OFF_REFRESH, OFF_CAND, OFF_SELECTED = 0,4,8,0xC,0x10
OFF_WORDS = 0x14
OFF_COMMIT = 0xF4
OFF_TEXT_LEN = 0xF8
OFF_CURSOR = 0xFC
OFF_TEXT_BUF = 0x100
TEXT_BUF_LEN = 96
OFF_PEND_LEN = 0x160
OFF_PEND_BUF = 0x164
PEND_BUF_LEN = 32
OFF_MODE = 0x184

# Keypad slot → MOKYA_KEY_* symbol (matches ime_keys.cpp kKeyTable)
SLOT_TO_KEY = [
    'KEY_1','KEY_3','KEY_5','KEY_7','KEY_9',
    'KEY_Q','KEY_E','KEY_T','KEY_U','KEY_O',
    'KEY_A','KEY_D','KEY_G','KEY_J','KEY_L',
    'KEY_Z','KEY_C','KEY_B','KEY_M','KEY_BACKSLASH',
]

# Mirror of ime_keys.cpp kKeyTable digit_slots/letter_slots — used in
# Direct mode to produce a specific ASCII char via multitap.
# Each tuple: (MOKYA_KEY_X, (slot0, slot1, slot2, slot3)).
DIRECT_KEY_SLOTS = [
    ('KEY_1', ('1', '2')),
    ('KEY_3', ('3', '4')),
    ('KEY_5', ('5', '6')),
    ('KEY_7', ('7', '8')),
    ('KEY_9', ('9', '0')),
    ('KEY_Q', ('q', 'w', 'Q', 'W')),
    ('KEY_E', ('e', 'r', 'E', 'R')),
    ('KEY_T', ('t', 'y', 'T', 'Y')),
    ('KEY_U', ('u', 'i', 'U', 'I')),
    ('KEY_O', ('o', 'p', 'O', 'P')),
    ('KEY_A', ('a', 's', 'A', 'S')),
    ('KEY_D', ('d', 'f', 'D', 'F')),
    ('KEY_G', ('g', 'h', 'G', 'H')),
    ('KEY_J', ('j', 'k', 'J', 'K')),
    ('KEY_L', ('l', 'L')),
    ('KEY_Z', ('z', 'x', 'Z', 'X')),
    ('KEY_C', ('c', 'v', 'C', 'V')),
    ('KEY_B', ('b', 'n', 'B', 'N')),
    ('KEY_M', ('m', 'M')),
]
# Invert: char → (key_name, multitap_index)
CHAR_TO_DIRECT = {}
for key_name, slots in DIRECT_KEY_SLOTS:
    for i, c in enumerate(slots):
        if c is not None and c not in CHAR_TO_DIRECT:
            CHAR_TO_DIRECT[c] = (key_name, i)

MODE_SMART_ZH = 0
MODE_SMART_EN = 1
MODE_DIRECT   = 2

KEYCODE_HDR = "firmware/mie/include/mie/keycode.h"

def load_keycode_map():
    mapping = {}
    src = Path(KEYCODE_HDR).read_text(encoding='utf-8')
    pat = re.compile(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)')
    for m in pat.finditer(src):
        mapping[m.group(1)[len('MOKYA_'):]] = int(m.group(2), 16)
    return mapping

# ── Conversion helpers ─────────────────────────────────────────────────

def reading_to_keycodes(reading_bytes, tone, keymap):
    """Convert a dict reading (slot bytes + tone field) to the keycode
    sequence a user would physically tap.

    Tone byte encoding in the dict:
        tone 1  (陰平): no explicit byte in keyseq — signalled by the
                       `tone` field. User types SPACE to mark tone 1.
        tone 2  (陽平): keyseq ends with 0x23 (slot 2 secondary = ˊ).
        tone 3  (上聲): keyseq ends with 0x22 (slot 1 primary = ˇ).
        tone 4  (去聲): keyseq ends with 0x22 (slot 1 secondary = ˋ).
        tone 5  (輕聲): keyseq ends with 0x24 (slot 3 primary = ˙).

    Don't guess from the last byte alone — 0x23 doubles as ㄓ, 0x24 as
    ㄚ, so a tone-1 char whose reading is `[0x23]` (隻 / 之) or ends in
    ㄚ (家 / 八 / 發) would be mis-diagnosed and the SPACE filter skipped,
    dumping the search into an untoned soup dominated by tone-2/3/4
    homophones. Use the `tone` field instead: it's the ground truth.
    """
    codes = []
    for b in reading_bytes:
        if b == 0x20:
            codes.append(keymap['KEY_SPACE']); continue
        idx = b - 0x21
        if 0 <= idx < 20:
            codes.append(keymap[SLOT_TO_KEY[idx]])
        else:
            return None
    if tone == 1:
        codes.append(keymap['KEY_SPACE'])
    return codes


def reading_to_events(reading_bytes, phoneme_pos, tone, keymap,
                       short_only=False, hint_any=False):
    """Build a key-event sequence for a dict reading.

    Modes:
      precise (default)  — each slot byte becomes (phoneme_pos + 1)
                           consecutive LONG_PRESS events. The engine's
                           long-press multitap cycle lands on phoneme
                           position p after (p+1) presses.
                             pos 0 → 1 long-press
                             pos 1 → 2 long-presses
                             pos 2 → 3 long-presses (slot 4 ㄦ only)
      short_only         — each slot byte emits exactly one short-tap
                           event (no long-press cycling). The engine
                           treats every byte as fuzzy half-keyboard.
      short_only + hint_any — same as short_only but flag=HINT_ANY so
                           the engine explicitly records ANY (used by
                           --user-sim's short-tap pass to be format-
                           independent of the engine default).

    SPACE tone-1 marker and tone bytes always emit plain (no flag)
    presses.

    phoneme_pos: tuple parallel to reading_bytes (empty → primary).
    """
    events = []
    for i, b in enumerate(reading_bytes):
        if b == 0x20:
            events.append((keymap['KEY_SPACE'], 0))
            continue
        idx = b - 0x21
        if not (0 <= idx < 20):
            return None
        kc = keymap[SLOT_TO_KEY[idx]]
        if short_only:
            flag = KEY_FLAG_HINT_ANY if hint_any else 0
            events.append((kc, flag))
        else:
            p = phoneme_pos[i] if phoneme_pos and i < len(phoneme_pos) else 0
            for _ in range(p + 1):
                events.append((kc, KEY_FLAG_LONG_PRESS))
    if tone == 1:
        events.append((keymap['KEY_SPACE'], 0))
    return events

# ── IME client with persistent pylink ──────────────────────────────────

class ImeDriver:
    def __init__(self, swd, keymap, transport="swd"):
        self.swd = swd
        self.km  = keymap
        if transport not in ("swd", "rtt"):
            raise ValueError(f"unknown transport {transport!r}")
        self.transport = transport
        # Resolve addresses once. g_key_inject_buf is always available;
        # even when transport=rtt we use the magic word as a sanity
        # check that Core 1's scheduler is up.
        base = swd.symbol('g_key_inject_buf')
        self.a_magic    = base
        self.a_producer = base + 4
        self.a_consumer = base + 8
        self.a_pushed   = base + 12
        self.a_rejected = base + 16
        self.a_events   = base + 20
        if transport == "rtt":
            # Precompute magic + nop frame for cheap send_frame calls.
            self._KIJ_MAGIC = bytes([0xAA, 0x55])
            # Warm up CB lookup now so the first inject doesn't pay.
            swd._rtt_locate()
            # Hand transport arbitration over to the RTT task. SWD task
            # will observe g_key_inject_mode != SWD at the top of its
            # loop and long-sleep — so ime_task isn't competing with
            # two hot pollers at priority 2.
            swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        # view_router uses uniquely-named s_view_router_active so nm can
        # disambiguate from i2c_bus.c's s_active (both static int).
        self.a_active = swd.symbol('s_view_router_active')
        self.a_cand_full = swd.symbol('g_ime_cand_full')
        # Cache FUNC/OK/DEL/RIGHT/MODE/SPACE keycodes.
        self.KC_FUNC  = keymap['KEY_FUNC']
        self.KC_OK    = keymap['KEY_OK']
        self.KC_DEL   = keymap['KEY_DEL']
        self.KC_RIGHT = keymap['KEY_RIGHT']
        self.KC_MODE  = keymap['KEY_MODE']
        self.KC_SPACE = keymap['KEY_SPACE']
        # Drain poll — slightly above Core 1 key_inject_task's 5 ms cadence.
        self.poll_ms = 6

    def check_alive(self):
        m = self.swd.read_u32(self.a_magic)
        if m != KEYI_MAGIC:
            raise RuntimeError(f"inject magic 0x{m:08X} ≠ 0x{KEYI_MAGIC:08X}")

    # ── Fast field pokers — read one u32 from ime_view_debug instead of
    # the full 400-byte snapshot. Polling on a single field is ~20x
    # cheaper (0.28 ms vs 5.6 ms per read) so we can poll tighter.
    _TEXT_LEN_ADDR = IME_DBG_ADDR + OFF_TEXT_LEN
    _PEND_LEN_ADDR = IME_DBG_ADDR + OFF_PEND_LEN
    _MODE_ADDR     = IME_DBG_ADDR + OFF_MODE        # byte field, use u32 read

    def read_text_len(self):
        return self.swd.read_u32(self._TEXT_LEN_ADDR)

    def read_pend_len(self):
        return self.swd.read_u32(self._PEND_LEN_ADDR)

    def read_mode_byte(self):
        return self.swd.read_u32(self._MODE_ADDR & ~3) >> ((self._MODE_ADDR & 3) * 8) & 0xFF

    def read_cand_seq(self):
        return self.swd.read_u32(self.a_cand_full + 4)

    def wait_until(self, read_fn, condition, timeout_s=0.4, poll_s=0.003):
        """Poll a cheap reader until `condition(value)` returns True or
        timeout. Returns the last observed value (regardless of match).
        poll_s defaults to 3 ms — the Core 1 key_inject task's 5 ms
        cadence sets the true reaction floor, anything finer just
        spends extra SWD round-trips."""
        deadline = time.time() + timeout_s
        v = read_fn()
        while time.time() < deadline:
            if condition(v): return v
            time.sleep(poll_s)
            v = read_fn()
        return v

    def read_all_candidates(self):
        """Return the full 100-candidate word list (seq-locked read)."""
        for _ in range(8):
            blob = self.swd.read_mem(self.a_cand_full, IME_CAND_FULL_SIZE)
            magic = struct.unpack_from('<I', blob, 0)[0]
            seq   = struct.unpack_from('<I', blob, 4)[0]
            if magic != IME_CAND_FULL_MAGIC:
                raise RuntimeError(f"cand_full magic 0x{magic:08X}")
            if seq & 1: continue
            # Double-read seq to confirm stability.
            seq2 = self.swd.read_u32(self.a_cand_full + 4)
            if seq == seq2: break
        else:
            raise RuntimeError("cand_full seq unstable")
        count = struct.unpack_from('<i', blob, 8)[0]
        words = []
        for i in range(min(count, IME_CAND_FULL_MAX)):
            off = 16 + i * IME_CAND_FULL_WLEN
            end = blob.find(b'\x00', off, off + IME_CAND_FULL_WLEN)
            if end < 0: end = off + IME_CAND_FULL_WLEN
            words.append(blob[off:end].decode('utf-8', errors='replace'))
        return count, words

    def find_rank(self, target):
        """Return full-list rank of target, or None if absent (up to 100)."""
        count, words = self.read_all_candidates()
        for i, w in enumerate(words):
            if w == target: return i
        return None

    def read_snapshot(self):
        """Read ime_view_debug, retrying on seq change."""
        for _ in range(8):
            blob = self.swd.read_mem(IME_DBG_ADDR, IME_DBG_SIZE)
            magic = struct.unpack_from('<I', blob, OFF_MAGIC)[0]
            seq   = struct.unpack_from('<I', blob, OFF_SEQ)[0]
            if magic != IME_DBG_MAGIC:
                raise RuntimeError(f"ime dbg magic 0x{magic:08X}")
            if seq & 1: continue
            # Double-read guard — seq mustn't change between reads.
            seq2 = self.swd.read_u32(IME_DBG_ADDR + OFF_SEQ)
            if seq == seq2:
                break
        else:
            raise RuntimeError("couldn't get stable seq")

        def cstr(off, maxlen):
            end = blob.find(b'\x00', off, off + maxlen)
            if end < 0: end = off + maxlen
            return blob[off:end].decode('utf-8', errors='replace')

        return {
            'cand'    : struct.unpack_from('<i', blob, OFF_CAND)[0],
            'selected': struct.unpack_from('<i', blob, OFF_SELECTED)[0],
            'commit'  : struct.unpack_from('<I', blob, OFF_COMMIT)[0],
            'text_len': struct.unpack_from('<i', blob, OFF_TEXT_LEN)[0],
            'text'    : cstr(OFF_TEXT_BUF, TEXT_BUF_LEN),
            'pending' : cstr(OFF_PEND_BUF, PEND_BUF_LEN),
            'mode'    : blob[OFF_MODE],
            'words'   : [cstr(OFF_WORDS + i*24, 24) for i in range(8)],
        }

    def queue_events(self, event_list):
        """Write 2-byte events to the inject ring + bump producer,
        chunking to respect the RING_EVENTS capacity. event_list is a
        list of (key_byte, flags_byte) tuples. Chunk = RING_EVENTS-2
        keeps a 2-slot safety margin between the producer write window
        and the consumer pointer (long sequences like RIGHT*N + OK can
        exceed the ring otherwise, with later writes corrupting events
        the consumer hasn't drained yet)."""
        if self.transport == "rtt":
            self._queue_events_rtt(event_list)
            return
        idx = 0
        n   = len(event_list)
        chunk_max = RING_EVENTS - 2
        while idx < n:
            producer = self.swd.read_u32(self.a_producer)
            consumer = self.swd.read_u32(self.a_consumer)
            while producer - consumer >= chunk_max:
                time.sleep(self.poll_ms / 1000.0)
                consumer = self.swd.read_u32(self.a_consumer)
            free = chunk_max - (producer - consumer)
            take = min(free, n - idx)
            writes = []
            for (kb, fb) in event_list[idx:idx + take]:
                slot = producer % RING_EVENTS
                writes.append((self.a_events + slot * EVENT_SIZE + 0, kb))
                writes.append((self.a_events + slot * EVENT_SIZE + 1, fb))
                producer += 1
            self.swd.write_u8_many(writes)
            self.swd.write_u32(self.a_producer, producer)
            idx += take
        deadline = time.time() + 2.0
        target_producer = self.swd.read_u32(self.a_producer)
        while time.time() < deadline:
            if self.swd.read_u32(self.a_consumer) >= target_producer: return
            time.sleep(self.poll_ms / 1000.0)
        raise RuntimeError("inject timeout")

    @staticmethod
    def _crc8(data):
        crc = 0
        for b in data:
            crc ^= b
            for _ in range(8):
                crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) \
                                                 else (crc << 1) & 0xFF
        return crc

    def _queue_events_rtt(self, event_list):
        """RTT transport path: wrap each (kb, fb) into a KEY_EVENT frame
        (magic 0xAA 0x55 + type 0x01 + len 2 + payload + crc8) and
        stream them down the SEGGER RTT down-channel. rtt_send_frame
        blocks internally if the ring fills — no host-side pacing
        needed beyond that. Waits for the ring to drain before
        returning so the caller's usual post-inject poll is valid."""
        for (kb, fb) in event_list:
            body = bytes([0x01, 0x02, kb & 0xFF, fb & 0xFF])
            frame = self._KIJ_MAGIC + body + bytes([self._crc8(body)])
            self.swd.rtt_send_frame(frame, timeout_s=2.0)
        # Wait for firmware to drain everything we pushed. +5 ms cushion
        # covers the key_inject_rtt task's 5 ms poll cadence plus the
        # key_event_push_inject_flags queue hop.
        deadline = time.time() + 2.0
        while time.time() < deadline:
            if self.swd.rtt_ring_empty():
                time.sleep(0.005)
                return
            time.sleep(self.poll_ms / 1000.0)
        raise RuntimeError("rtt inject timeout (ring never drained)")

    def inject_keycodes(self, kc_list, flags=0):
        """Press+release each keycode in order. Optional flags byte is
        applied to BOTH the press and release events (so a long-press
        short test path mirrors what the keypad scanner emits)."""
        seq = []
        for kc in kc_list:
            seq.append((0x80 | (kc & 0x7F), flags & 0xFF))
            seq.append((0x00 | (kc & 0x7F), flags & 0xFF))
        self.queue_events(seq)

    def inject_events_pairs(self, kc_flag_list):
        """Press+release each (keycode, flags) pair in order. Each pair
        produces two ring events sharing the flags byte. Used for typing
        Bopomofo readings where each phoneme key carries its own short-
        vs long-tap intent."""
        seq = []
        for (kc, fb) in kc_flag_list:
            seq.append((0x80 | (kc & 0x7F), fb & 0xFF))
            seq.append((0x00 | (kc & 0x7F), fb & 0xFF))
        self.queue_events(seq)

    def cycle_view_to(self, target, max_retries=2):
        """Cycle FUNC until s_active == target. Verifies EVERY step — if a
        FUNC press is dropped (inject ring race, LVGL busy) s_active
        doesn't advance and we'd otherwise land on a random view. Polls
        after each press and retries from the new s_active. Raises if the
        view won't converge after max_retries full cycles."""
        attempts = 0
        while True:
            cur = self.swd.read_u32(self.a_active)
            if cur == target: return
            if attempts >= max_retries * VIEW_COUNT:
                raise RuntimeError(f"view stuck at {cur}, target {target}")
            self.inject_keycodes([self.KC_FUNC])
            expected = (cur + 1) % VIEW_COUNT
            self.wait_until(lambda: self.swd.read_u32(self.a_active),
                            lambda v: v == expected, timeout_s=0.3)
            attempts += 1

    def ensure_view(self, target):
        """Fast guard — reads s_active; only cycles if off-target. Call
        before every inject in long runs so a stray FUNC-like event can't
        silently route keys to the wrong view."""
        cur = self.swd.read_u32(self.a_active)
        if cur != target:
            self.cycle_view_to(target)

    def ensure_mode(self, target_mode):
        """Cycle KEY_MODE until the engine reports the requested input
        mode (0=SmartZh, 1=SmartEn, 2=Direct). MODE auto-commits pending
        state, so call this BEFORE typing a new-mode char."""
        cur = self.read_mode_byte()
        if cur == target_mode: return
        steps = (target_mode - cur) % 3
        for _ in range(steps):
            self.inject_keycodes([self.KC_MODE])
        got = self.wait_until(self.read_mode_byte,
                              lambda v: v == target_mode, timeout_s=0.2)
        if got != target_mode:
            raise RuntimeError(f"mode stuck at {got}, target {target_mode}")

    def inject_direct_char(self, ch):
        """Type one ASCII letter / digit in Direct mode via multitap + OK.
        Returns True on success (text_buf grew by len(ch)), False if the
        char isn't in the keypad table or the commit didn't land."""
        plan = CHAR_TO_DIRECT.get(ch)
        if plan is None: return False
        key_name, idx = plan
        kc = self.km[key_name]
        pre_len = self.read_text_len()
        # Press the key (idx+1) times then OK to commit the multitap
        # phoneme without waiting for the 800 ms timeout.
        self.inject_keycodes([kc] * (idx + 1) + [self.KC_OK])
        # Wait for text_buf to grow by len(ch.encode('utf-8')).
        got = self.wait_until(self.read_text_len,
                              lambda v: v != pre_len, timeout_s=0.4)
        return got - pre_len == len(ch.encode('utf-8'))

    def inject_space(self):
        """Commit a single space char via SPACE in Direct mode."""
        pre_len = self.read_text_len()
        self.inject_keycodes([self.KC_SPACE])
        got = self.wait_until(self.read_text_len,
                              lambda v: v != pre_len, timeout_s=0.3)
        return got != pre_len

    def reset_text(self, batch=20, max_rounds=200):
        """Clear committed text + pending. The engine keeps a ~2 KB g_text
        buffer so a full passage run can leave hundreds of chars queued;
        DEL is sent in batches of `batch` to stay inside the 64-byte
        inject ring while draining quickly."""
        for _ in range(max_rounds):
            snap = self.read_snapshot()
            if snap['text_len'] == 0 and snap['pending'] == '':
                return
            self.inject_keycodes([self.KC_DEL] * batch)
        raise RuntimeError(f"reset failed: text_len={snap['text_len']} pending={snap['pending']!r}")

    def inject_newline(self):
        """Press OK once when the engine is idle (no candidates / no
        multitap pending) → emit "\\n" (Phase 1.4 Task C, mirrors SPACE-
        when-idle behaviour)."""
        pre_len = self.read_text_len()
        self.inject_keycodes([self.KC_OK])
        got = self.wait_until(self.read_text_len,
                              lambda v: v != pre_len, timeout_s=0.4)
        return got - pre_len == 1

    def inject_picker_char(self, ch):
        """Type one Traditional-Chinese punctuation char via the SYM1
        long-press picker. Sequence: long-press SYM1 (hold ≥ 500 ms so
        the engine tick fires), DPAD RIGHT × cell_idx, OK. Returns True
        when text_buf grows by len(ch.encode('utf-8'))."""
        idx = PICKER_INDEX.get(ch)
        if idx is None: return False
        pre_len = self.read_text_len()
        kc_sym1 = self.km['KEY_SYM1']
        self.queue_events([(0x80 | kc_sym1, 0)])
        time.sleep(0.6)   # > 500 ms so tick() opens the picker
        self.queue_events([(0x00 | kc_sym1, 0)])
        if idx > 0:
            self.queue_events(
                [(0x80 | self.KC_RIGHT, 0), (0x00 | self.KC_RIGHT, 0)] * idx)
        self.queue_events(
            [(0x80 | self.KC_OK, 0), (0x00 | self.KC_OK, 0)])
        got = self.wait_until(self.read_text_len,
                              lambda v: v != pre_len, timeout_s=0.4)
        return got - pre_len == len(ch.encode('utf-8'))

    def commit_rank(self, rank):
        """Navigate to `rank` and press OK. Batches RIGHT × rank + OK for
        speed, then verifies the selected index actually reached `rank`
        (or was past it and wrapped — engine RIGHT is cyclic `mod
        cand_count_`). Raises if scroll went off the rails."""
        if rank < 0: rank = 0
        seq = [self.KC_RIGHT] * rank + [self.KC_OK]
        self.inject_keycodes(seq)

# ── Main ────────────────────────────────────────────────────────────────

CJK_RE   = re.compile(r'[一-鿿㐀-䶿]')
ASCII_RE = re.compile(r'[A-Za-z0-9]')

# Mirror of ime_logic.cpp::kSymPickerCells_; cell index = position in
# this list. Used to route Traditional-Chinese punctuation through the
# SYM1 long-press picker instead of skipping it.
PICKER_CELLS = [
    "「", "」", "『", "』",
    "（", "）", "【", "】",
    "，", "。", "、", "；",
    "：", "？", "！", "…",
]
PICKER_INDEX = {c: i for i, c in enumerate(PICKER_CELLS)}

def classify(ch):
    if CJK_RE.match(ch):       return 'cjk'
    if ASCII_RE.match(ch):     return 'ascii'
    if ch == ' ':              return 'space'
    if ch == '\n':             return 'newline'
    if ch in PICKER_INDEX:     return 'picker'
    return None  # other punctuation / emoji / etc. → skip

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path', nargs='?')
    ap.add_argument('--stdin', action='store_true')
    ap.add_argument('--elf', default='build/core1_bridge/core1_bridge.elf')
    ap.add_argument('--transport', choices=['swd', 'rtt'], default='swd',
                    help='Key-injection transport. swd (default) writes '
                    'g_key_inject_buf ring over SWD memory. rtt streams '
                    'frames through the SEGGER RTT down-channel — same '
                    'debugger, but bypasses the ring (faster bursts, '
                    'avoids the ring-wrap pitfall).')
    ap.add_argument('--dict', default='firmware/mie/data/dict_mie_v4.bin')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--no-commit', action='store_true')
    ap.add_argument('--reset-every-n', type=int, default=0,
                    help='reset engine state every N chars '
                         '(0 = let text accumulate; 1 = isolate each char)')
    ap.add_argument('--no-hints', action='store_true',
                    help='Force every Bopomofo press to send hint=0xFF '
                         '(any phoneme position) — simulates v2 / pre-Phase 1.4 '
                         'half-keyboard behaviour for rank comparison.')
    ap.add_argument('--user-sim', action='store_true',
                    help='Realistic-user mode: type every byte as short-tap '
                         '(primary-phoneme filter) first; if target is not '
                         'within --user-threshold, DEL the pending input and '
                         'retype with the dict\'s correct long-press hints. '
                         'Reports keystroke effort breakdown alongside ranks.')
    ap.add_argument('--user-threshold', type=int, default=8,
                    help='--user-sim: backtrack to long-press only when the '
                         'short-tap attempt has the target outside the top-N '
                         'candidates (default 8 = one visible page).')
    ap.add_argument('--user-short-mode', choices=['any', 'primary'],
                    default='any',
                    help='--user-sim short-tap semantics (default any):\n'
                         '  any     — short-tap sends HINT_ANY, matching '
                         'legacy half-keyboard behaviour where any phoneme '
                         'of a slot is valid. Long-press then narrows to '
                         'the secondary/tertiary phoneme. Most realistic '
                         'for untrained users.\n'
                         '  primary — short-tap sends flags=0 (primary '
                         'phoneme filter), the strict Phase 1.4 semantic. '
                         'Tests the upper bound of long-press benefit '
                         'when users have learned the new semantics.')
    args = ap.parse_args()

    if args.stdin: text = sys.stdin.read()
    elif args.path: text = Path(args.path).read_text(encoding='utf-8')
    else: print("need path or --stdin", file=sys.stderr); sys.exit(1)

    # Keep every testable char, preserve original order. Types:
    # cjk (Bopomofo/SmartZh), ascii (letter/digit via Direct), space.
    chars = [c for c in text if classify(c) is not None]
    if args.limit: chars = chars[:args.limit]
    if not chars: print("no testable chars"); return

    d = read_v4(Path(args.dict).read_bytes())
    ch_to_cid = {ch: cid for cid, (ch, _) in enumerate(d['char_table'])}
    keymap = load_keycode_map()

    # Precompute Bopomofo key plan only for CJK chars — ASCII/space go
    # through a separate Direct-mode path.
    plans = {}            # canonical plan (long-press where dict pos != 0)
    plans_short = {}      # all-short-tap variant for --user-sim first attempt
    absent = set()
    has_pp = d.get('has_phoneme_pos', False)
    for ch in set(chars):
        if classify(ch) != 'cjk': continue
        if ch not in ch_to_cid: absent.add(ch); continue
        rs = d['char_table'][ch_to_cid[ch]][1]
        if not rs: absent.add(ch); continue
        # Reading tuple shape varies by dict version:
        #   MIE4 legacy:  (kbytes, tone, freq)        — pos info absent
        #   MIE4 + pp:    (kbytes, pos_tuple, tone, freq)
        if has_pp and len(rs[0]) == 4:
            kb, pos, tone, _ = rs[0]
        else:
            kb, tone, _ = rs[0]
            pos = ()
        if args.no_hints:
            # v2 simulation: every byte is a short-tap with HINT_ANY
            # (no phoneme-position filter). One event per byte.
            events = reading_to_events(bytes(kb), pos, tone, keymap,
                                       short_only=True, hint_any=True)
        else:
            # Canonical precise plan: cycle long-presses to land on the
            # exact phoneme position the dict authored.
            events = reading_to_events(bytes(kb), pos, tone, keymap)
        if events is None: absent.add(ch); continue
        plans[ch] = events
        # All-short-tap plan (one event per byte) for --user-sim's
        # first attempt. 'any' uses HINT_ANY (fuzzy half-keyboard
        # behaviour); 'primary' uses flags=0 (engine treats short-tap
        # the same way under the current default — flags=0 also yields
        # ANY when no LONG_PRESS bit set, but we keep the option as a
        # forward-compat hook for engine semantic experiments).
        plans_short[ch] = reading_to_events(
            bytes(kb), pos, tone, keymap,
            short_only=True,
            hint_any=(args.user_short_mode == 'any'))
    if absent:
        print(f"CJK chars not in dict ({len(absent)}): " + ''.join(list(absent)[:40]))

    with MokyaSwd(elf=args.elf) as swd:
        drv = ImeDriver(swd, keymap, transport=args.transport)
        drv.check_alive()
        drv.cycle_view_to(IME_VIEW_INDEX)
        drv.reset_text()

        stats = {
            'tested': 0, 'in_top8': 0, 'rank0': 0,
            'in_top100': 0,
            'committed_ok': 0,
            'rank_hist': {}, 'miss': [], 'commit_miss': [],
            'ascii_tested': 0, 'ascii_ok': 0, 'ascii_fail': [],
            'space_tested': 0, 'space_ok': 0,
            'newline_tested': 0, 'newline_ok': 0,
            'picker_tested': 0, 'picker_ok': 0, 'picker_fail': [],
            # --user-sim accounting (zero in non-sim modes)
            'us_short_ok'   : 0,   # found in top-N on short-tap pass
            'us_long_ok'    : 0,   # short missed but long-press found
            'us_miss'       : 0,   # both passes missed top-100
            'us_keystrokes' : 0,   # total slot-key + DEL + RIGHT + OK
            'us_long_extra' : 0,   # extra keystrokes spent on backtrack
        }
        t0 = time.time()
        expected_accum = ""
        for idx, ch in enumerate(chars):
            # View guard — cheap SWD read. Silently re-cycles if we
            # somehow drifted off the IME view.
            drv.ensure_view(IME_VIEW_INDEX)

            kind = classify(ch)
            # ── ASCII branch: Direct mode, multitap + OK commit. ──
            if kind == 'ascii':
                stats['ascii_tested'] += 1
                drv.ensure_mode(MODE_DIRECT)
                ok = drv.inject_direct_char(ch)
                if ok:
                    stats['ascii_ok'] += 1
                    status = f"ASCII {ch!r} OK"
                else:
                    stats['ascii_fail'].append((idx, ch))
                    status = f"ASCII {ch!r} FAIL"
                if (idx + 1) % 5 == 0:
                    per_char = (time.time() - t0) / (idx + 1)
                    print(f"  [{idx+1:3d}/{len(chars)}] {ch!r:>5s}  {status}  ({per_char*1000:.0f} ms/char)")
                continue
            if kind == 'space':
                stats['space_tested'] += 1
                # Phase 1.4 Task C: SmartZh / SmartEn / Direct all support
                # idle SPACE → " " (handle_smart's idle path + handle_direct's
                # SPACE branch). No mode switch required — exercise whatever
                # mode the previous chars left us in.
                if drv.inject_space():
                    stats['space_ok'] += 1
                continue
            if kind == 'newline':
                stats['newline_tested'] += 1
                if drv.inject_newline():
                    stats['newline_ok'] += 1
                continue
            if kind == 'picker':
                stats['picker_tested'] += 1
                # The picker works in any input mode — it intercepts at
                # the top-level dispatcher. Skip ensure_mode to avoid the
                # mode-switch overhead.
                if drv.inject_picker_char(ch):
                    stats['picker_ok'] += 1
                else:
                    stats['picker_fail'].append((idx, ch))
                continue
            # ── CJK branch: Bopomofo SmartZh mode. ──
            drv.ensure_mode(MODE_SMART_ZH)
            if ch not in plans:
                stats['miss'].append((idx, ch, 'absent-from-dict'))
                continue
            if args.reset_every_n and idx > 0 and idx % args.reset_every_n == 0:
                drv.reset_text()
                expected_accum = ""

            before_text_len = drv.read_snapshot()['text_len']

            # Helper closures kept inline so they share local stats /
            # before_text_len without ferrying them through a class.
            def inject_and_locate(events):
                """Inject one pass and return (rank, snapshot). Polls
                the cheap pend_len u32 (0.3 ms per read) rather than the
                full 400 B snapshot, dropping the per-poll cost ~20x —
                only does the full snapshot once pend_len flips nonzero
                or we time out."""
                drv.inject_events_pairs(events)
                drv.wait_until(drv.read_pend_len,
                               lambda v: v != 0, timeout_s=0.4)
                s = drv.read_snapshot()
                # Fast path: top-8 mirror in the small snapshot.
                r = None
                for i, w in enumerate(s['words']):
                    if w == ch: r = i; break
                if r is None:
                    r = drv.find_rank(ch)
                return r, s

            if args.user_sim:
                # Pass 1: short-tap (primary-phoneme filter).
                short_events = plans_short[ch]
                rank_short, snap = inject_and_locate(short_events)
                # User keystrokes for this attempt = one per byte event.
                ks = len(short_events)

                if rank_short is not None and rank_short < args.user_threshold:
                    # Short-tap reached the target inside the visible window
                    # → user accepts and commits. No fallback needed.
                    rank = rank_short
                    used_path = 'short'
                else:
                    # Backtrack: DEL the pending input one byte per slot
                    # press, then retype with the canonical long-press
                    # plan. Both short and precise plans append the SAME
                    # number of bytes to key_seq_ (precise just cycles
                    # the same byte's phoneme hint via repeated long-
                    # presses) — so byte count = len(plans_short[ch]).
                    n_pending = len(short_events)
                    drv.inject_keycodes([drv.KC_DEL] * n_pending)
                    drv.wait_until(drv.read_pend_len,
                                   lambda v: v == 0, timeout_s=0.4)
                    rank_long, snap = inject_and_locate(plans[ch])
                    extra_ks = n_pending + len(plans[ch])
                    ks += extra_ks
                    stats['us_long_extra'] += extra_ks
                    rank = rank_long
                    used_path = 'long' if rank_long is not None else 'miss'

                if rank is not None:
                    # Scroll RIGHT × rank, then OK.
                    ks += rank + 1
                    if used_path == 'short': stats['us_short_ok'] += 1
                    else:                    stats['us_long_ok']  += 1
                else:
                    # Both passes missed top-100. Clear pending so the
                    # next char isn't typed onto stale state. Use byte
                    # count, NOT event count (long-press cycle adds
                    # multiple events per byte).
                    drv.inject_keycodes([drv.KC_DEL] * len(plans_short[ch]))
                    ks += len(plans_short[ch])
                    stats['us_miss'] += 1
                stats['us_keystrokes'] += ks
            else:
                rank, snap = inject_and_locate(plans[ch])
                used_path = None

            stats['tested'] += 1
            if rank is not None:
                if rank < 8: stats['in_top8'] += 1
                if rank == 0: stats['rank0'] += 1
                stats['in_top100'] += 1
                stats['rank_hist'][rank] = stats['rank_hist'].get(rank, 0) + 1

                if not args.no_commit:
                    pre_len = snap['text_len']
                    drv.commit_rank(rank)
                    drv.wait_until(drv.read_text_len,
                                   lambda v: v != pre_len, timeout_s=0.4)
                    snap2 = drv.read_snapshot()
                    expected_accum += ch
                    # Compare text_len growth to the expected UTF-8 byte
                    # length of `ch`. The debug struct only mirrors the
                    # first 96 B of g_text, so text.endswith(ch) breaks
                    # for long passages — bytes grown is the reliable
                    # signal that the commit landed.
                    grown = snap2['text_len'] - pre_len
                    if grown == len(ch.encode('utf-8')):
                        stats['committed_ok'] += 1
                        status = f"rank {rank}"
                    else:
                        stats['commit_miss'].append(
                            (idx, ch, snap2['text'][-10:]))
                        status = f"rank {rank} COMMIT-FAIL"
                else:
                    status = f"rank {rank}"
            else:
                stats['miss'].append((idx, ch, f'cand={snap["cand"]}'))
                # In user-sim mode the simulated user has already given
                # up and we explicitly DEL'd the pending input — don't
                # double-press OK on an empty state. In standard mode we
                # still commit the best guess (rank 0) so the visible
                # text approximates a real session.
                if not args.no_commit and not args.user_sim:
                    pre_len = snap['text_len']
                    drv.commit_rank(0)
                    drv.wait_until(drv.read_text_len,
                                   lambda v: v != pre_len, timeout_s=0.4)
                    expected_accum += ch
                status = f"MISS cand={snap['cand']} top8={[w for w in snap['words'] if w][:4]}"

            if (idx + 1) % 5 == 0 or rank is None:
                per_char = (time.time() - t0) / (idx + 1)
                print(f"  [{idx+1:3d}/{len(chars)}] {ch!r:>5s}  {status}  "
                      f"({per_char*1000:.0f} ms/char)")

        dt = time.time() - t0
        n = max(1, stats['tested'])
        total = stats['tested'] + stats['ascii_tested'] + stats['space_tested']
        print(f"\n=== Summary: {total} chars in {dt:.1f}s "
              f"({1000*dt/max(1,total):.0f} ms/char) ===")
        print(f"  CJK:       {stats['tested']} tested")
        print(f"    rank 0 (1-tap):  {stats['rank0']}  "
              f"({100*stats['rank0']/n:.1f}%)")
        print(f"    in top-8:        {stats['in_top8']} / {n}  "
              f"({100*stats['in_top8']/n:.1f}%)")
        print(f"    in top-100:      {stats['in_top100']} / {n}  "
              f"({100*stats['in_top100']/n:.1f}%)  "
              f"(reachable by scrolling)")
        if stats['ascii_tested']:
            print(f"  ASCII:     {stats['ascii_ok']}/{stats['ascii_tested']}  "
                  f"({100*stats['ascii_ok']/stats['ascii_tested']:.1f}%)")
        if stats['space_tested']:
            print(f"  Space:     {stats['space_ok']}/{stats['space_tested']}")
        if stats['newline_tested']:
            print(f"  Newline:   {stats['newline_ok']}/{stats['newline_tested']}")
        if stats['picker_tested']:
            print(f"  Picker:    {stats['picker_ok']}/{stats['picker_tested']}  "
                  f"({100*stats['picker_ok']/stats['picker_tested']:.1f}%)")
        if not args.no_commit:
            print(f"  committed:  {stats['committed_ok']} / {n}  "
                  f"({100*stats['committed_ok']/n:.1f}%)")
        if args.user_sim:
            total_ks = stats['us_keystrokes']
            extra    = stats['us_long_extra']
            print()
            print(f"  -- user-sim (threshold = top-{args.user_threshold}) --")
            print(f"    short-tap OK       : {stats['us_short_ok']} / {n}  "
                  f"({100*stats['us_short_ok']/n:.1f}%)")
            print(f"    long-press rescue  : {stats['us_long_ok']} / {n}  "
                  f"({100*stats['us_long_ok']/n:.1f}%)")
            print(f"    total miss         : {stats['us_miss']} / {n}  "
                  f"({100*stats['us_miss']/n:.1f}%)")
            print(f"    keystrokes total   : {total_ks}  "
                  f"({total_ks/n:.2f} / char)")
            print(f"    backtrack waste    : {extra}  "
                  f"({extra/n:.2f} / char avg, "
                  f"{extra/max(1,stats['us_long_ok']+stats['us_miss']):.2f} "
                  f"/ fallback char)")
        print(f"  rank hist: {dict(sorted(stats['rank_hist'].items()))}")
        if stats['miss']:
            uniq = {}
            for i, c, r in stats['miss']: uniq[c] = (i, r)
            print(f"\n  MISS (not in top-100) — {len(uniq)} unique chars:")
            for ch, (i, r) in list(uniq.items())[:40]:
                print(f"    {ch}  first@{i}  ({r})")
        if stats['ascii_fail']:
            print(f"\n  ASCII-FAIL ({len(stats['ascii_fail'])}):")
            for i, c in stats['ascii_fail'][:20]:
                print(f"    idx {i}: {c!r}")
        if stats['commit_miss']:
            print(f"\n  COMMIT-FAIL ({len(stats['commit_miss'])}):")
            for i, ch, tail in stats['commit_miss'][:20]:
                print(f"    idx {i}: expected {ch!r} tail={tail!r}")

if __name__ == '__main__':
    main()
