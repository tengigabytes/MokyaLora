"""Quick SWD probe: dump channel_share_view's static `s` after entering B-4.

Should help diagnose why url_len reads 0 — possibilities:
  - render() never ran (refresh() gated wrong, or view crashed)
  - render() ran but channel_share_url_build returned 0
"""
import re, struct, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd
from mokya_rtt import build_frame, TYPE_KEY_EVENT

ELF = "build/core1_bridge/core1_bridge.elf"
ARM_OBJDUMP = (r"C:/Program Files/Arm/GNU Toolchain "
               r"mingw-w64-x86_64-arm-none-eabi/bin/"
               r"arm-none-eabi-objdump.exe")
KEYMAP = {}
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)


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


def main():
    addr = find_static(ELF, 'channel_share_view.c', 's')
    print(f"s @ 0x{addr:08x}")
    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        # back home
        for _ in range(8):
            v = swd.read_u32(swd.symbol('s_view_router_active'))
            if v == 0: break
            k(swd, KEYMAP['MOKYA_KEY_BACK'], 300)
        # Nav: launcher → Chan tile (0,1) → CHANNELS → slot 0 → CHANNEL_EDIT → SET → CHANNEL_SHARE
        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 400)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'], 60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        for _ in range(8): k(swd, KEYMAP['MOKYA_KEY_UP'], 40)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        print(f"  view after CHANNEL_EDIT entry: {v}")
        k(swd, KEYMAP['MOKYA_KEY_SET'], 800)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        print(f"  view after SET: {v}")

        for sleep_s in (0.2, 1.0, 2.0):
            time.sleep(sleep_s)
            data = swd.read_mem(addr, 280)
            header_p, qr_p, url_p, status_p = struct.unpack('<IIII', data[:16])
            active = data[16]
            change_seq = struct.unpack('<I', data[20:24])[0]
            url_buf = bytes(data[24:280])  # 256 B
            url_len = struct.unpack('<I', swd.read_mem(addr + 280, 4))[0]
            url_head = url_buf[:64].split(b'\x00', 1)[0]
            print(f"  t+{sleep_s:.1f}s: hdr=0x{header_p:08x} qr=0x{qr_p:08x} "
                  f"url=0x{url_p:08x} stat=0x{status_p:08x}")
            print(f"          active={active} change_seq={change_seq} url_len={url_len}")
            print(f"          url_buf head: {url_head!r}")

        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        for _ in range(3):
            v = swd.read_u32(swd.symbol('s_view_router_active'))
            if v == 0: break
            k(swd, KEYMAP['MOKYA_KEY_BACK'], 300)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)


if __name__ == "__main__":
    main()
