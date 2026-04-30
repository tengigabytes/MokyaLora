"""Phase 2 verification — T-3 spectrum view (passive SNR bars).

Drives nav into T-3, then verifies via SWD-readable diag globals:
  - g_t3_collected     == count of CLI peers with non-self + snr known
  - g_t3_top_node_num  == node_num of CLI's strongest non-self peer
  - g_t3_top_snr_x100  matches CLI's snr × 100 for that peer
"""
import json
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from mokya_rtt import build_frame, TYPE_KEY_EVENT  # type: ignore

KEYMAP = {}
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME   = 0
VIEW_ID_TOOLS       = 18
VIEW_ID_T3_SPECTRUM = 28


def k(swd, kc, gap=200):
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([(kc & 0x7F) | 0x80, 0])))
    time.sleep(0.03)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([kc & 0x7F, 0])))
    time.sleep(gap / 1000)


def back_home(swd):
    for _ in range(8):
        if swd.read_u32(swd.symbol('s_view_router_active')) == VIEW_ID_BOOT_HOME:
            return True
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 300)
    return False


def parse_cli_peers():
    """Return (collected_count, top_node_num, top_snr_x100) from
    `meshtastic --info`. CLI prints a Nodes block as JSON-ish; the
    `snr` field is a float in dB."""
    cli = subprocess.run(
        ['python', '-m', 'meshtastic', '--port', 'COM16', '--info'],
        capture_output=True, text=True, timeout=30)
    out = cli.stdout

    # Extract my_node_num
    m = re.search(r'"myNodeNum":\s*(\d+)', out)
    my_node_num = int(m.group(1)) if m else 0

    # Find the "Nodes in mesh:" JSON block. Easiest: locate the
    # literal "Nodes in mesh: {" then grab the {} block.
    idx = out.find('Nodes in mesh:')
    if idx < 0: return 0, 0, 0
    after = out[idx + len('Nodes in mesh: '):]
    # Find matching closing brace.
    depth = 0
    end = -1
    for i, c in enumerate(after):
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    if end < 0: return 0, 0, 0
    blob = after[:end]
    try:
        nodes = json.loads(blob)
    except Exception as e:
        print(f"  !! couldn't parse Nodes block: {e}")
        return 0, 0, 0

    peers_with_snr = []
    for node_id, info in nodes.items():
        n = info.get('num', 0)
        if n == my_node_num: continue
        snr = info.get('snr')
        if snr is None: continue
        peers_with_snr.append((n, int(round(snr * 100))))

    if not peers_with_snr:
        return 0, 0, 0

    # Sort by SNR desc, take top.
    peers_with_snr.sort(key=lambda p: p[1], reverse=True)
    top_n, top_snr_x100 = peers_with_snr[0]
    return len(peers_with_snr), top_n, top_snr_x100


def main():
    fails = 0

    print("Fetching meshtastic --info ground truth...")
    n_count, top_node, top_snr = parse_cli_peers()
    print(f"  CLI: {n_count} peers with SNR; top = !{top_node:08x} @ {top_snr/100:+.2f} dB")

    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! BOOT_HOME unreachable"); sys.exit(1)

        # launcher → Tools tile (1,2)
        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 400)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'], 60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        k(swd, KEYMAP['MOKYA_KEY_DOWN'], 80)
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_TOOLS:
            print(f"!! TOOLS not active (v={v})"); sys.exit(1)

        # tools_view → T-3 (row index 2; anchor cur_row=0 then DOWN ×2)
        for _ in range(11): k(swd, KEYMAP['MOKYA_KEY_UP'], 40)
        for _ in range(2):  k(swd, KEYMAP['MOKYA_KEY_DOWN'], 60)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        print(f"  view = {v} (expected {VIEW_ID_T3_SPECTRUM} T3_SPECTRUM)")
        if v != VIEW_ID_T3_SPECTRUM:
            print("!! wrong view"); sys.exit(1)
        time.sleep(1.2)

        # Read diag globals.
        a_collected = swd.symbol('g_t3_collected')
        a_top_snr   = swd.symbol('g_t3_top_snr_x100')
        a_top_num   = swd.symbol('g_t3_top_node_num')
        dev_collected = swd.read_u32(a_collected)
        dev_top_snr   = struct.unpack('<i', swd.read_mem(a_top_snr, 4))[0]
        dev_top_num   = swd.read_u32(a_top_num)
        print(f"  device: collected={dev_collected}  "
              f"top=!{dev_top_num:08x} @ {dev_top_snr/100:+.2f} dB")

        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if dev_collected == n_count:
        print(f"  [PASS] collected count matches CLI ({dev_collected})")
    else:
        print(f"  [FAIL] collected count: device={dev_collected} cli={n_count}")
        fails += 1

    if dev_top_num == top_node:
        print(f"  [PASS] top peer node_num matches CLI (!{top_node:08x})")
    else:
        print(f"  [FAIL] top node_num: device=!{dev_top_num:08x} cli=!{top_node:08x}")
        fails += 1

    # SNR comparison: float→x100 round-trip can yield ±1 difference.
    # Allow ±5 (= ±0.05 dB) tolerance.
    if abs(dev_top_snr - top_snr) <= 5:
        print(f"  [PASS] top SNR matches within ±0.05 dB (device={dev_top_snr/100:+.2f}, cli={top_snr/100:+.2f})")
    else:
        print(f"  [FAIL] top SNR mismatch >0.05 dB")
        print(f"         device: {dev_top_snr/100:+.2f} dB")
        print(f"         cli   : {top_snr/100:+.2f} dB")
        fails += 1

    print()
    if fails == 0:
        print("==> Phase 2 (T-3) PASS — spectrum view matches CLI peer SNR ground truth")
        sys.exit(0)
    else:
        print(f"==> Phase 2 FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
