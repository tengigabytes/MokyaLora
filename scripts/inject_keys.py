#!/usr/bin/env python3
"""inject_keys.py — push virtual keypresses to Core 1 via J-Link SWD,
read committed text back. Supports a regression-test runner.

Hardware-side contract:
    - g_key_inject_buf  (firmware/core1/src/keypad/key_inject.h):
        ring buffer; address found by ELF symbol lookup at runtime.
    - 0x2007FE00 ime_view_debug_t (v0003):
        candidates / committed text / pending / mode snapshot.

CLI:
    --status                       show inject + IME state
    --reset                        clear committed text + pending (DEL spam)
    --view {keypad,rf,font,ime}    cycle FUNC until s_active matches
    --commit                       press OK once (commit selected candidate)
    keys...                        inject press+release for each token
    --test FILE                    run regression cases from tab-sep file:
                                       expected\tKEY1 KEY2 KEY3 ...

Examples:
    python scripts/inject_keys.py --view ime
    python scripts/inject_keys.py KEY_D KEY_J KEY_9 KEY_Z --commit
    python scripts/inject_keys.py --status
    python scripts/inject_keys.py --reset
"""
import argparse
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DEFAULT_ELF = "build/core1_bridge/core1_bridge.elf"
JLINK  = r"C:/Program Files/SEGGER/JLink_V932/JLink.exe"
ARM_NM = (r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/"
          r"bin/arm-none-eabi-nm.exe")

RING_SIZE     = 64
KEYI_MAGIC    = 0x4B454949
IME_DBG_ADDR  = 0x2007FE00
IME_DBG_MAGIC = 0xEEED0003
IME_DBG_SIZE  = 0x190

# ime_view_debug_t v0003 field offsets — must match firmware/core1/src/ui/ime_view.c
IME_OFF_MAGIC         = 0x000
IME_OFF_SEQ           = 0x004
IME_OFF_REFRESH       = 0x008
IME_OFF_CAND_COUNT    = 0x00C
IME_OFF_SELECTED      = 0x010
IME_OFF_WORDS         = 0x014
IME_OFF_RAW0          = 0x0D4
IME_OFF_RAW1          = 0x0E4
IME_OFF_COMMIT_COUNT  = 0x0F4
IME_OFF_TEXT_LEN      = 0x0F8
IME_OFF_CURSOR_POS    = 0x0FC
IME_OFF_TEXT_BUF      = 0x100
IME_OFF_TEXT_BUF_LEN  = 96
IME_OFF_PENDING_LEN   = 0x160
IME_OFF_PENDING_BUF   = 0x164
IME_OFF_PENDING_BUF_LEN = 32
IME_OFF_MODE          = 0x184

# View names → s_active value (matches view_router.c init order)
VIEW_NAMES = {'keypad': 0, 'rf': 1, 'font': 2, 'ime': 3}
VIEW_COUNT = 4

KEYCODE_HDR = "firmware/mie/include/mie/keycode.h"

# ── Symbol resolution ────────────────────────────────────────────────────

def load_keycode_map():
    mapping = {}
    src = Path(KEYCODE_HDR).read_text(encoding='utf-8')
    pat = re.compile(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)')
    for m in pat.finditer(src):
        name = m.group(1)[len('MOKYA_'):]
        mapping[name] = int(m.group(2), 16)
    return mapping

def nm_symbol(elf, sym):
    r = subprocess.run([ARM_NM, elf], capture_output=True, text=True, check=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == sym:
            return int(parts[0], 16)
    raise RuntimeError(f"symbol {sym!r} not found in {elf}")

# ── J-Link interface ─────────────────────────────────────────────────────

def jlink_script(device, cmds):
    with tempfile.NamedTemporaryFile('w', suffix='.jlink', delete=False,
                                     encoding='ascii') as f:
        f.write("connect\n")
        f.write("\n".join(cmds))
        f.write("\nqc\n")
        path = f.name
    try:
        r = subprocess.run(
            [JLINK, '-device', device, '-if', 'SWD', '-speed', '4000',
             '-autoconnect', '1', '-CommanderScript', path],
            capture_output=True, text=True, timeout=30)
        return r.stdout
    finally:
        Path(path).unlink(missing_ok=True)

def parse_mem_block(out, addr, length_bytes):
    """Find addr lines in J-Link output, concat hex bytes."""
    bytes_out = bytearray()
    re_line = re.compile(r'^([0-9A-Fa-f]{8})\s*=\s*((?:[0-9A-Fa-f]{2}\s?)+)\s*$')
    have = {}
    for line in out.splitlines():
        line = line.strip()
        m = re_line.match(line)
        if not m: continue
        a = int(m.group(1), 16)
        hexes = m.group(2).split()
        have[a] = bytes(int(h, 16) for h in hexes)
    # Stitch contiguous chunks
    cur = addr
    while len(bytes_out) < length_bytes and cur in have:
        bytes_out += have[cur]
        cur += len(have[cur])
    return bytes(bytes_out[:length_bytes])

def read_mem(device, addr, length_bytes):
    """Read length_bytes starting at addr. Uses mem8 for byte-aligned reads."""
    cmds = [f'mem8 0x{addr:08X} {length_bytes}']
    out = jlink_script(device, cmds)
    return parse_mem_block(out, addr, length_bytes)

def read_u32(device, addr):
    b = read_mem(device, addr, 4)
    return struct.unpack('<I', b)[0]

def write_u8_batch(device, ops):
    """ops: list of (addr, byte_value)."""
    cmds = [f'w1 0x{a:08X} 0x{v & 0xFF:02X}' for a, v in ops]
    return jlink_script(device, cmds)

def write_u32(device, addr, val):
    return jlink_script(device, [f'w4 0x{addr:08X} 0x{val & 0xFFFFFFFF:08X}'])

def write_batch(device, cmds):
    return jlink_script(device, cmds)

# ── Buffer addresses (cached) ────────────────────────────────────────────

_cached = {}

def buf_addrs(elf):
    if 'inject' in _cached:
        return _cached
    base = nm_symbol(elf, 'g_key_inject_buf')
    _cached['inject_base']     = base
    _cached['inject_magic']    = base + 0
    _cached['inject_producer'] = base + 4
    _cached['inject_consumer'] = base + 8
    _cached['inject_pushed']   = base + 12
    _cached['inject_rejected'] = base + 16
    _cached['inject_events']   = base + 20
    _cached['s_active']        = nm_symbol(elf, 's_active')
    return _cached

# ── Inject + state ──────────────────────────────────────────────────────

def check_inject_alive(device, elf):
    a = buf_addrs(elf)
    magic = read_u32(device, a['inject_magic'])
    if magic != KEYI_MAGIC:
        raise RuntimeError(
            f"key_inject_buf magic = 0x{magic:08X} ≠ expected 0x{KEYI_MAGIC:08X}; "
            f"flash latest firmware")

def queue_events(device, elf, byte_list, batch_size=16):
    a = buf_addrs(elf)
    producer = read_u32(device, a['inject_producer'])
    cmds = []
    for byte in byte_list:
        slot = producer % RING_SIZE
        cmds.append(f'w1 0x{a["inject_events"] + slot:08X} 0x{byte:02X}')
        producer += 1
        if len(cmds) >= batch_size:
            cmds.append(f'w4 0x{a["inject_producer"]:08X} 0x{producer:08X}')
            write_batch(device, cmds)
            cmds = []
            # Wait for the polling task (10 ms cadence) to drain the ring.
            time.sleep(0.05)
            producer = read_u32(device, a['inject_producer'])
    if cmds:
        cmds.append(f'w4 0x{a["inject_producer"]:08X} 0x{producer:08X}')
        write_batch(device, cmds)

def press(kc):     return [0x80 | (kc & 0x7F), 0x00 | (kc & 0x7F)]
def press_only(kc): return [0x80 | (kc & 0x7F)]
def release_only(kc): return [0x00 | (kc & 0x7F)]

def inject_keys(device, elf, kc_list, hold_ms=30, gap_ms=20):
    """For each keycode, send press then release with small delays."""
    for kc in kc_list:
        queue_events(device, elf, press(kc))
        time.sleep((hold_ms + gap_ms) / 1000.0)

# ── ime_view_debug snapshot ──────────────────────────────────────────────

def read_ime_snapshot(device):
    """Read the v0003 seqlock-protected snapshot. Retries up to 5x if a
    write was in progress when we halted the CPU (odd seq, or seq changed
    across two reads of the same block)."""
    for _ in range(5):
        blob = read_mem(device, IME_DBG_ADDR, IME_DBG_SIZE)
        magic = struct.unpack_from('<I', blob, IME_OFF_MAGIC)[0]
        seq = struct.unpack_from('<I', blob, IME_OFF_SEQ)[0]
        if magic != IME_DBG_MAGIC:
            raise RuntimeError(
                f"ime_view_debug magic = 0x{magic:08X} ≠ "
                f"expected 0x{IME_DBG_MAGIC:08X}")
        if (seq & 1) != 0:
            continue  # write in progress, retry
        # Read seq again to confirm no write completed mid-read.
        seq2 = read_u32(device, IME_DBG_ADDR + IME_OFF_SEQ)
        if seq == seq2:
            break
    else:
        raise RuntimeError("ime_view_debug: unable to get stable seq after 5 tries")
    snap = {
        'magic'         : magic,
        'refresh_count' : struct.unpack_from('<I', blob, IME_OFF_REFRESH)[0],
        'cand_count'    : struct.unpack_from('<i', blob, IME_OFF_CAND_COUNT)[0],
        'selected'      : struct.unpack_from('<i', blob, IME_OFF_SELECTED)[0],
        'commit_count'  : struct.unpack_from('<I', blob, IME_OFF_COMMIT_COUNT)[0],
        'text_len'      : struct.unpack_from('<i', blob, IME_OFF_TEXT_LEN)[0],
        'cursor_pos'    : struct.unpack_from('<i', blob, IME_OFF_CURSOR_POS)[0],
        'pending_len'   : struct.unpack_from('<i', blob, IME_OFF_PENDING_LEN)[0],
        'mode'          : blob[IME_OFF_MODE],
    }
    # Strings — safe-trim to NUL.
    def take_cstr(off, maxlen):
        end = blob.find(b'\x00', off, off + maxlen)
        if end < 0: end = off + maxlen
        return blob[off:end].decode('utf-8', errors='replace')
    snap['text']       = take_cstr(IME_OFF_TEXT_BUF, IME_OFF_TEXT_BUF_LEN)
    snap['pending']    = take_cstr(IME_OFF_PENDING_BUF, IME_OFF_PENDING_BUF_LEN)
    snap['words']      = [
        take_cstr(IME_OFF_WORDS + i * 24, 24) for i in range(8)
    ]
    return snap

# ── High-level actions ──────────────────────────────────────────────────

def cmd_status(device, elf):
    check_inject_alive(device, elf)
    a = buf_addrs(elf)
    inj = {
        'producer' : read_u32(device, a['inject_producer']),
        'consumer' : read_u32(device, a['inject_consumer']),
        'pushed'   : read_u32(device, a['inject_pushed']),
        'rejected' : read_u32(device, a['inject_rejected']),
    }
    active = read_u32(device, a['s_active'])
    snap = read_ime_snapshot(device)
    print(f"key_inject @ 0x{a['inject_base']:08X}: "
          f"producer={inj['producer']} consumer={inj['consumer']} "
          f"pushed={inj['pushed']} rejected={inj['rejected']}")
    print(f"active view = {active} ({[k for k,v in VIEW_NAMES.items() if v == active] or '?'})")
    print(f"ime_view: refresh={snap['refresh_count']} mode={snap['mode']} "
          f"cand={snap['cand_count']} selected={snap['selected']} "
          f"commit_count={snap['commit_count']}")
    print(f"  text   ({snap['text_len']:3d}B): {snap['text']!r}")
    print(f"  pending({snap['pending_len']:3d}B): {snap['pending']!r}")
    if snap['cand_count'] > 0:
        print(f"  candidates (top 8):")
        for i, w in enumerate(snap['words']):
            if not w: continue
            mark = '<-' if i == snap['selected'] else '  '
            print(f"    [{i}] {mark} {w!r}")

def cmd_view(device, elf, target_name, keymap):
    check_inject_alive(device, elf)
    a = buf_addrs(elf)
    target = VIEW_NAMES[target_name]
    cur = read_u32(device, a['s_active'])
    steps = (target - cur) % VIEW_COUNT
    if steps == 0:
        print(f"already on view {target_name} (s_active={cur})")
        return
    print(f"cycling FUNC {steps}x to reach {target_name}")
    for _ in range(steps):
        inject_keys(device, elf, [keymap['KEY_FUNC']], hold_ms=50, gap_ms=80)
    time.sleep(0.2)
    cur = read_u32(device, a['s_active'])
    if cur != target:
        print(f"WARNING: ended on s_active={cur}, expected {target}")

def cmd_reset(device, elf, keymap, max_dels=64):
    """Drain pending + commit ledger by mashing DEL until both empty."""
    check_inject_alive(device, elf)
    for _ in range(max_dels):
        snap = read_ime_snapshot(device)
        if snap['text_len'] == 0 and snap['pending_len'] == 0:
            return
        inject_keys(device, elf, [keymap['KEY_DEL']], hold_ms=20, gap_ms=20)
    raise RuntimeError("reset failed: text_len + pending_len still > 0 after 64 DELs")

def cmd_commit(device, elf, keymap):
    check_inject_alive(device, elf)
    inject_keys(device, elf, [keymap['KEY_OK']], hold_ms=30, gap_ms=80)
    time.sleep(0.1)

# ── Test runner ─────────────────────────────────────────────────────────

def parse_keys(tokens, keymap):
    """Resolve token names (KEY_X / 0xNN / decimal) into keycode ints."""
    out = []
    for tok in tokens:
        if not tok or tok.startswith('#'): continue
        if tok.startswith('0x'):    out.append(int(tok, 16))
        elif tok.isdigit():         out.append(int(tok))
        elif tok in keymap:         out.append(keymap[tok])
        elif ('KEY_' + tok) in keymap: out.append(keymap['KEY_' + tok])
        else:
            raise ValueError(f"unknown key token: {tok!r}")
    return out

def cmd_test(device, elf, test_path, keymap):
    """Test file format (tab-separated):
        expected_text<TAB>KEY1 KEY2 ...
    Lines starting with '#' or blank are skipped.
    For each row:
        - reset the engine
        - ensure IME view active
        - inject keys
        - press OK
        - read text_buf — pass if equals expected
    """
    cases = []
    for ln, raw in enumerate(Path(test_path).read_text(encoding='utf-8').splitlines(), 1):
        s = raw.rstrip()
        if not s or s.startswith('#'): continue
        if '\t' not in s:
            print(f"WARN line {ln}: no tab separator, skipping: {s!r}")
            continue
        expected, keys_str = s.split('\t', 1)
        cases.append((ln, expected.strip(), keys_str.strip().split()))
    print(f"loaded {len(cases)} test cases from {test_path}")

    # Make sure we're on IME view once.
    cmd_view(device, elf, 'ime', keymap)

    passes = 0
    fails  = []
    for ln, expected, keys in cases:
        try:
            cmd_reset(device, elf, keymap)
            kc_list = parse_keys(keys, keymap)
            inject_keys(device, elf, kc_list, hold_ms=30, gap_ms=40)
            time.sleep(0.15)  # let IME finish search
            snap_before = read_ime_snapshot(device)
            cmd_commit(device, elf, keymap)
            time.sleep(0.15)
            snap_after  = read_ime_snapshot(device)
            got = snap_after['text']
            ok = (got == expected)
            if ok:
                passes += 1
                print(f"  PASS  L{ln:03d}  {expected!r}  (cands={snap_before['cand_count']})")
            else:
                fails.append((ln, expected, got, snap_before, snap_after))
                top8 = ' '.join(repr(w) for w in snap_before['words'] if w)
                print(f"  FAIL  L{ln:03d}  expected={expected!r}  got={got!r}  "
                      f"cands={snap_before['cand_count']}  top8=[{top8}]")
        except Exception as e:
            fails.append((ln, expected, f"<{e!s}>", None, None))
            print(f"  ERR   L{ln:03d}  {expected!r}  exception: {e}")

    print(f"\n=== {passes}/{len(cases)} passed ===")
    if fails:
        print(f"FAILED ({len(fails)}):")
        for ln, exp, got, _, _ in fails:
            print(f"  L{ln:03d}  {exp!r}  →  {got!r}")
    return 0 if not fails else 1

# ── main ────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', default=DEFAULT_ELF)
    ap.add_argument('--device', default='RP2350_M33_1')
    ap.add_argument('--status', action='store_true')
    ap.add_argument('--reset', action='store_true')
    ap.add_argument('--view', choices=list(VIEW_NAMES.keys()))
    ap.add_argument('--commit', action='store_true')
    ap.add_argument('--test', metavar='FILE',
                    help='run regression test cases from a tab-sep file')
    ap.add_argument('keys', nargs='*',
                    help='keycode tokens to inject (e.g. KEY_D KEY_OK)')
    args = ap.parse_args()

    keymap = load_keycode_map()

    if args.test:
        sys.exit(cmd_test(args.device, args.elf, args.test, keymap))

    if args.view:
        cmd_view(args.device, args.elf, args.view, keymap)
    if args.reset:
        cmd_reset(args.device, args.elf, keymap)
    if args.keys:
        check_inject_alive(args.device, args.elf)
        kc_list = parse_keys(args.keys, keymap)
        inject_keys(args.device, args.elf, kc_list, hold_ms=30, gap_ms=40)
    if args.commit:
        cmd_commit(args.device, args.elf, keymap)
    if args.status or not (args.view or args.reset or args.keys
                           or args.commit or args.test):
        cmd_status(args.device, args.elf)

if __name__ == '__main__':
    main()
