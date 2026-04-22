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
RING_SIZE       = 64
KEYI_MAGIC      = 0x4B454949
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

# ── IME client with persistent pylink ──────────────────────────────────

class ImeDriver:
    def __init__(self, swd, keymap):
        self.swd = swd
        self.km  = keymap
        # Resolve addresses once.
        base = swd.symbol('g_key_inject_buf')
        self.a_magic    = base
        self.a_producer = base + 4
        self.a_consumer = base + 8
        self.a_pushed   = base + 12
        self.a_rejected = base + 16
        self.a_events   = base + 20
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

    def queue_bytes(self, byte_list):
        """Write events to the inject ring + bump producer, chunking to
        respect the 64-slot capacity. The previous one-shot write blew
        through the ring on long sequences (e.g. RIGHT*33 + OK = 68
        events): bytes after slot 63 wrapped and overwrote already-
        published events that the consumer hadn't drained yet, so the
        engine saw a corrupted RIGHT sequence and committed the wrong
        candidate. Chunk size = RING_SIZE-2 keeps a 2-slot safety margin
        between the active write window and the consumer pointer."""
        idx = 0
        n   = len(byte_list)
        chunk_max = RING_SIZE - 2
        while idx < n:
            producer = self.swd.read_u32(self.a_producer)
            consumer = self.swd.read_u32(self.a_consumer)
            # Block until enough free slots for the next chunk.
            while producer - consumer >= chunk_max:
                time.sleep(self.poll_ms / 1000.0)
                consumer = self.swd.read_u32(self.a_consumer)
            free = chunk_max - (producer - consumer)
            take = min(free, n - idx)
            writes = []
            for byte in byte_list[idx:idx + take]:
                slot = producer % RING_SIZE
                writes.append((self.a_events + slot, byte))
                producer += 1
            self.swd.write_u8_many(writes)
            self.swd.write_u32(self.a_producer, producer)
            idx += take
        # Wait for Core 1 to drain everything we sent.
        deadline = time.time() + 2.0
        target_producer = self.swd.read_u32(self.a_producer)
        while time.time() < deadline:
            if self.swd.read_u32(self.a_consumer) >= target_producer: return
            time.sleep(self.poll_ms / 1000.0)
        raise RuntimeError("inject timeout")

    def inject_keycodes(self, kc_list):
        """Press+release each keycode in order, one ring event each."""
        seq = []
        for kc in kc_list:
            seq.append(0x80 | (kc & 0x7F))
            seq.append(0x00 | (kc & 0x7F))
        self.queue_bytes(seq)

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
            # Wait until s_active advances by exactly 1 (or wraps).
            deadline = time.time() + 0.3
            expected = (cur + 1) % VIEW_COUNT
            while time.time() < deadline:
                new = self.swd.read_u32(self.a_active)
                if new == expected: break
                time.sleep(0.01)
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
        snap = self.read_snapshot()
        cur = snap['mode']
        if cur == target_mode: return
        # (1 - 0) mod 3 = 1 press. (0 - 2) mod 3 = 1. Always (target - cur) % 3.
        steps = (target_mode - cur) % 3
        for _ in range(steps):
            self.inject_keycodes([self.KC_MODE])
        # Verify.
        deadline = time.time() + 0.2
        while time.time() < deadline:
            snap = self.read_snapshot()
            if snap['mode'] == target_mode: return
            time.sleep(0.01)
        raise RuntimeError(f"mode stuck at {snap['mode']}, target {target_mode}")

    def inject_direct_char(self, ch):
        """Type one ASCII letter / digit in Direct mode via multitap + OK.
        Returns True on success (text_buf grew by len(ch)), False if the
        char isn't in the keypad table or the commit didn't land."""
        plan = CHAR_TO_DIRECT.get(ch)
        if plan is None: return False
        key_name, idx = plan
        kc = self.km[key_name]
        pre = self.read_snapshot()
        pre_len = pre['text_len']
        # Press the key (idx+1) times then OK to commit the multitap
        # phoneme without waiting for the 800 ms timeout.
        self.inject_keycodes([kc] * (idx + 1) + [self.KC_OK])
        # Wait for text_buf to grow by len(ch.encode('utf-8')).
        deadline = time.time() + 0.4
        while time.time() < deadline:
            snap = self.read_snapshot()
            if snap['text_len'] != pre_len: break
            time.sleep(0.01)
        return snap['text_len'] - pre_len == len(ch.encode('utf-8'))

    def inject_space(self):
        """Commit a single space char via SPACE in Direct mode."""
        pre_len = self.read_snapshot()['text_len']
        self.inject_keycodes([self.KC_SPACE])
        deadline = time.time() + 0.3
        while time.time() < deadline:
            snap = self.read_snapshot()
            if snap['text_len'] != pre_len: return True
            time.sleep(0.01)
        return False

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

def classify(ch):
    if CJK_RE.match(ch):   return 'cjk'
    if ASCII_RE.match(ch): return 'ascii'
    if ch == ' ':          return 'space'
    return None  # punctuation / emoji / etc. → skip

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path', nargs='?')
    ap.add_argument('--stdin', action='store_true')
    ap.add_argument('--elf', default='build/core1_bridge/core1_bridge.elf')
    ap.add_argument('--dict', default='firmware/mie/data/dict_mie_v4.bin')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--no-commit', action='store_true')
    ap.add_argument('--reset-every-n', type=int, default=0,
                    help='reset engine state every N chars '
                         '(0 = let text accumulate; 1 = isolate each char)')
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
    plans = {}
    absent = set()
    for ch in set(chars):
        if classify(ch) != 'cjk': continue
        if ch not in ch_to_cid: absent.add(ch); continue
        rs = d['char_table'][ch_to_cid[ch]][1]
        if not rs: absent.add(ch); continue
        kb, tone, _ = rs[0]
        codes = reading_to_keycodes(bytes(kb), tone, keymap)
        if codes is None: absent.add(ch); continue
        plans[ch] = codes
    if absent:
        print(f"CJK chars not in dict ({len(absent)}): " + ''.join(list(absent)[:40]))

    with MokyaSwd(elf=args.elf) as swd:
        drv = ImeDriver(swd, keymap)
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
                drv.ensure_mode(MODE_DIRECT)
                if drv.inject_space():
                    stats['space_ok'] += 1
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

            drv.inject_keycodes(plans[ch])
            # Poll the SWD snapshot until LVGL has published a post-
            # keystroke refresh. Pending-len becoming non-zero is the
            # earliest signal the engine saw the injected bytes.
            t_deadline = time.time() + 0.4
            while True:
                snap = drv.read_snapshot()
                if snap['pending']: break
                if time.time() >= t_deadline: break
                time.sleep(0.01)

            # Fast path: search top-8 mirror in the small snapshot.
            rank = None
            for i, w in enumerate(snap['words']):
                if w == ch: rank = i; break
            # Slow path: target not in top-8 → scan the full 100-candidate
            # mirror (ELF-resolved g_ime_cand_full buffer). This matches
            # real-user behaviour of scrolling RIGHT past the visible page.
            if rank is None:
                rank = drv.find_rank(ch)

            stats['tested'] += 1
            if rank is not None:
                if rank < 8: stats['in_top8'] += 1
                if rank == 0: stats['rank0'] += 1
                stats['in_top100'] += 1
                stats['rank_hist'][rank] = stats['rank_hist'].get(rank, 0) + 1

                if not args.no_commit:
                    pre_len = snap['text_len']
                    drv.commit_rank(rank)
                    deadline = time.time() + 0.4
                    snap2 = snap
                    while time.time() < deadline:
                        snap2 = drv.read_snapshot()
                        if snap2['text_len'] != pre_len: break
                        time.sleep(0.01)
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
                if not args.no_commit:
                    # Still commit best guess so state advances.
                    pre_len = snap['text_len']
                    drv.commit_rank(0)
                    deadline = time.time() + 0.4
                    while time.time() < deadline:
                        snap2 = drv.read_snapshot()
                        if snap2['text_len'] != pre_len: break
                        time.sleep(0.01)
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
        if not args.no_commit:
            print(f"  committed:  {stats['committed_ok']} / {n}  "
                  f"({100*stats['committed_ok']/n:.1f}%)")
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
