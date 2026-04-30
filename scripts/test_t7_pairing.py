"""Phase 1 verification — T-7 pairing code (admin pubkey display).

Drives nav into T-7, SWD-reads pairing_view's static `s.b64_buf`,
compares byte-equal against `meshtastic --info` publicKey.
"""
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from mokya_rtt import build_frame, TYPE_KEY_EVENT  # type: ignore

ELF = "build/core1_bridge/core1_bridge.elf"
ARM_OBJDUMP = (r"C:/Program Files/Arm/GNU Toolchain "
               r"mingw-w64-x86_64-arm-none-eabi/bin/"
               r"arm-none-eabi-objdump.exe")

KEYMAP = {}
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME  = 0
VIEW_ID_TOOLS      = 18
VIEW_ID_T7_PAIRING = 27

# pairing_view::s layout
#   8 ptrs × 4 = 32
#   +32  last_change_seq (u32)
#   +36  hex_buf[65]  → +36..+100
#   +101 b64_buf[48]  → +101..+148
PAIR_OFF_HEX = 36
PAIR_OFF_B64 = 101
PAIR_LEN_HEX = 64       # without NUL
PAIR_LEN_B64 = 44       # without NUL


def find_static(elf, src, name='s'):
    out = subprocess.check_output([ARM_OBJDUMP, "-t", elf], text=True)
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and src in line:
            in_block = True; continue
        if in_block:
            if 'df *ABS*' in line: break
            m = re.match(rf'^([0-9a-fA-F]+)\s+l\s+O\s+\.\S*bss\s+\S+\s+{re.escape(name)}$', line)
            if m: return int(m.group(1), 16)
    return None


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


def main():
    fails = 0

    # 1. Read CLI ground truth.
    print("Fetching meshtastic --info publicKey...")
    cli = subprocess.run(
        ['python', '-m', 'meshtastic', '--port', 'COM16', '--info'],
        capture_output=True, text=True, timeout=30)
    cli_b64 = None
    for line in cli.stdout.splitlines():
        if '"publicKey"' in line:
            m = re.search(r'"publicKey":\s*"([^"]+)"', line)
            if m:
                cli_b64 = m.group(1)
                break
    if cli_b64 is None:
        print("!! couldn't extract publicKey from --info; abort")
        sys.exit(1)
    print(f"  CLI publicKey ({len(cli_b64)} chars): {cli_b64}")

    # 2. Resolve symbols.
    pair_addr = find_static(ELF, 'pairing_view.c', 's')
    if pair_addr is None:
        print("!! pairing_view::s not found"); sys.exit(1)
    print(f"  pairing_view::s @ 0x{pair_addr:08x}")

    # 3. Drive nav: BOOT_HOME → launcher → Tools tile (1,2) → tools_view
    #               → T-7 row (anchor cur_row=0, DOWN ×6) → OK
    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! couldn't reach BOOT_HOME"); sys.exit(1)

        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 400)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'],   60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        # Tools tile = (1,2)
        k(swd, KEYMAP['MOKYA_KEY_DOWN'],  80)
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_TOOLS:
            print(f"!! TOOLS not active (v={v})"); sys.exit(1)

        # tools_view rows: T-1 / T-2 / T-3 / T-4 / T-5 / T-6 / T-7 / T-8
        # Anchor cur_row = 0 then DOWN ×6 → T-7
        for _ in range(11): k(swd, KEYMAP['MOKYA_KEY_UP'], 40)
        for _ in range(6):  k(swd, KEYMAP['MOKYA_KEY_DOWN'], 60)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        print(f"  view = {v} (expected {VIEW_ID_T7_PAIRING} T7_PAIRING)")
        if v != VIEW_ID_T7_PAIRING:
            print("!! wrong view"); sys.exit(1)
        time.sleep(0.5)

        # 4. SWD-read s.b64_buf + s.hex_buf.
        b64 = bytes(swd.read_mem(pair_addr + PAIR_OFF_B64,
                                  PAIR_LEN_B64)).decode('ascii', errors='replace')
        hexs = bytes(swd.read_mem(pair_addr + PAIR_OFF_HEX,
                                   PAIR_LEN_HEX)).decode('ascii', errors='replace')
        print(f"  device hex   ({len(hexs)} chars): {hexs}")
        print(f"  device b64   ({len(b64)} chars): {b64}")

        # Cleanup.
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    # 5. Compare.
    print()
    if b64 == cli_b64:
        print(f"  [PASS] device base64 matches CLI publicKey byte-for-byte")
    else:
        print(f"  [FAIL] device base64 != CLI publicKey")
        print(f"         device: {b64!r}")
        print(f"         CLI   : {cli_b64!r}")
        fails += 1

    # Sanity-check hex matches base64 by re-encoding.
    import base64
    try:
        decoded = base64.b64decode(cli_b64)
        cli_hex = decoded.hex().upper()
        if hexs == cli_hex:
            print(f"  [PASS] device hex matches base64-decoded pubkey")
        else:
            print(f"  [FAIL] device hex != decoded pubkey")
            print(f"         device hex: {hexs}")
            print(f"         CLI hex   : {cli_hex}")
            fails += 1
    except Exception as e:
        print(f"  [SKIP] hex compare ({e})")

    print()
    if fails == 0:
        print("==> Phase 1 (T-7) PASS — pairing pubkey display matches CLI ground truth")
        sys.exit(0)
    else:
        print(f"==> Phase 1 FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
