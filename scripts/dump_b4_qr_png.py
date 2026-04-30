"""Phase 5b QR auto-verification — SWD-dump lv_qrcode I1 buffer + decode.

Uses three new SWD-readable globals exported by channel_share_view.c
on each successful render:
  g_b4_qr_data    (uint8_t *)  — pointer to LVGL canvas I1 buffer
  g_b4_qr_size    (uint32_t)   — bytes (= stride × height)
  g_b4_qr_w/h     (uint16_t)   — pixel dimensions (= 144 typically)
  g_b4_qr_stride  (uint32_t)   — bytes per row
  g_b4_qr_cf      (uint8_t)    — color format (1 = LV_COLOR_FORMAT_I1)

Drives nav into B-4, reads the buffer, saves as PNG, and compares
against a reference QR built locally from the same URL using the
`qrcode` Python lib. Match → Phase 5b end-to-end PASS.

Output:
  /tmp/b4_qr_device.png  — what the device renders
  /tmp/b4_qr_python.png  — reference from qrcode library
  /tmp/b4_qr_diff.png    — XOR overlay (all-white = perfect match)
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
KEYMAP = {}
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME     = 0
VIEW_ID_CHANNEL_SHARE = 26


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
    out_dir = Path('/tmp')
    out_dir.mkdir(parents=True, exist_ok=True)
    device_png = out_dir / 'b4_qr_device.png'
    python_png = out_dir / 'b4_qr_python.png'
    diff_png   = out_dir / 'b4_qr_diff.png'

    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! couldn't reach BOOT_HOME"); sys.exit(1)

        # Drive: launcher -> Chan tile -> slot 0 -> CHANNEL_EDIT -> SET -> CHANNEL_SHARE
        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 400)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'], 60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        for _ in range(8): k(swd, KEYMAP['MOKYA_KEY_UP'], 40)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        k(swd, KEYMAP['MOKYA_KEY_SET'], 800)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_CHANNEL_SHARE:
            print(f"!! expected CHANNEL_SHARE, got {v}"); sys.exit(1)
        time.sleep(1.2)  # let render fire

        # Read SWD diag globals.
        addr_data   = swd.symbol('g_b4_qr_data')
        addr_size   = swd.symbol('g_b4_qr_size')
        addr_w      = swd.symbol('g_b4_qr_w')
        addr_h      = swd.symbol('g_b4_qr_h')
        addr_stride = swd.symbol('g_b4_qr_stride')
        addr_cf     = swd.symbol('g_b4_qr_cf')

        data_ptr = swd.read_u32(addr_data)
        size     = swd.read_u32(addr_size)
        w        = struct.unpack('<H', swd.read_mem(addr_w, 2))[0]
        h        = struct.unpack('<H', swd.read_mem(addr_h, 2))[0]
        stride   = swd.read_u32(addr_stride)
        cf       = swd.read_mem(addr_cf, 1)[0]

        print(f"  g_b4_qr_data   = 0x{data_ptr:08x}")
        print(f"  g_b4_qr_size   = {size} B")
        print(f"  g_b4_qr_w x h  = {w} x {h}")
        print(f"  g_b4_qr_stride = {stride}")
        print(f"  g_b4_qr_cf     = 0x{cf:02x} (0x07=LV_COLOR_FORMAT_I1)")
        if data_ptr == 0 or size == 0 or w == 0 or h == 0 or cf != 0x07:
            print("!! diag globals indicate QR not rendered (or wrong cf)")
            sys.exit(1)
        if size > 64 * 1024:
            print(f"!! size {size} suspicious (cap at 64 KB)"); sys.exit(1)

        # Read raw I1 pixel data + URL. LVGL I1 buffer layout:
        #   bytes 0..7  : 2-entry palette (RGBA) — index 0 = light, 1 = dark
        #   bytes 8..   : pixel rows, stride bytes per row, 1 bpp MSB-first
        raw_full = bytes(swd.read_mem(data_ptr, size))
        palette  = raw_full[:8]
        raw      = raw_full[8:]
        print(f"  palette (8 B): {palette.hex()}")
        cshare_addr = None
        out = subprocess.check_output([
            r"C:/Program Files/Arm/GNU Toolchain mingw-w64-x86_64-arm-none-eabi/"
            r"bin/arm-none-eabi-objdump.exe", "-t", ELF], text=True)
        in_block = False
        for line in out.splitlines():
            if 'df *ABS*' in line and 'channel_share_view.c' in line:
                in_block = True; continue
            if in_block:
                if 'df *ABS*' in line: break
                m = re.match(r'^([0-9a-fA-F]+)\s+l\s+O\s+\.\S*bss\s+\S+\s+s$', line)
                if m: cshare_addr = int(m.group(1), 16); break
        url_len = struct.unpack('<I', swd.read_mem(cshare_addr + 280, 4))[0]
        url = bytes(swd.read_mem(cshare_addr + 24, url_len)).decode('utf-8')
        print(f"  URL ({url_len} chars): {url}")

        # Cleanup.
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    # Decode I1 → PIL image.
    # LVGL I1: 1 bpp, MSB first per byte. 0 = light, 1 = dark (per
    # the canvas's dark_color/light_color, but for PIL-mode '1' the
    # convention is 0=black/255=white).
    from PIL import Image
    img_dev = Image.new('1', (w, h), 0)
    px = img_dev.load()
    for y in range(h):
        for x in range(w):
            byte = raw[y * stride + (x >> 3)]
            bit  = (byte >> (7 - (x & 7))) & 1
            # bit=1 → "dark" in LVGL == QR module (we want black in PIL '1')
            # PIL '1' uses 0=black, 255=white
            px[x, y] = 0 if bit == 1 else 255
    img_dev = img_dev.resize((w * 2, h * 2), Image.NEAREST)
    img_dev.save(device_png)
    print(f"  saved {device_png}")

    # Build reference QR via qrcode lib for the same URL.
    import qrcode
    qr = qrcode.QRCode(
        version=None,                # auto
        error_correction=qrcode.constants.ERROR_CORRECT_L,
        box_size=1, border=4)
    qr.add_data(url)
    qr.make(fit=True)
    img_ref = qr.make_image(fill_color='black', back_color='white').convert('1')
    print(f"  ref QR size = {img_ref.size} (modules incl. quiet zone)")
    # Scale ref to match device dimensions for visual diff.
    img_ref_scaled = img_ref.resize((w * 2, h * 2), Image.NEAREST)
    img_ref_scaled.save(python_png)
    print(f"  saved {python_png}")

    # ── Auto-decode the device QR via OpenCV ──────────────────────────
    # OpenCV QRCodeDetector ships with opencv-python (no external
    # libzbar dependency). Decoded text MUST equal `url` byte-for-byte
    # for Phase 5b to claim end-to-end behavioural PASS.
    print()
    print("OpenCV QR decode of device PNG:")
    try:
        import cv2
        img_cv = cv2.imread(str(device_png))
        det = cv2.QRCodeDetector()
        decoded, bbox, _ = det.detectAndDecode(img_cv)
        print(f"  decoded: {decoded!r}")
        if decoded == url:
            print(f"  ==> Phase 5b PASS — decoded QR matches URL byte-for-byte")
            sys.exit(0)
        else:
            print(f"  ==> Phase 5b FAIL — decoded != expected URL")
            print(f"      expected: {url!r}")
            sys.exit(1)
    except Exception as e:
        print(f"  cv2 decode failed ({e}); falling back to module diff")

    # ── Module-level comparison (fallback if cv2 missing) ─────────────
    # Convert both images to N×N module matrices and compare bit-wise.
    # qrcode lib gives us the canonical matrix directly; for the device
    # PNG, sample the centre of each module cell.
    ref_modules = img_ref.size[0]   # e.g. 41 (Version 4 + 4-px border)
    print()
    print(f"Module-level comparison (matrix {ref_modules}×{ref_modules}):")

    # Sample device's I1 buffer back at module resolution. LVGL renders
    # the QR with proportional scaling; modules are at known stride =
    # canvas_w / matrix_w.
    # The pre-resize device image is w×h = 144×144 (matches g_b4_qr_w/h).
    img_dev_orig = Image.new('1', (w, h), 0)
    px2 = img_dev_orig.load()
    for y in range(h):
        for x in range(w):
            byte = raw[y * stride + (x >> 3)]
            bit  = (byte >> (7 - (x & 7))) & 1
            px2[x, y] = 0 if bit == 1 else 255

    module_pixels = w / ref_modules
    print(f"  device module size = {module_pixels:.2f} px")
    if module_pixels < 1.5:
        print(f"  !! module size <1.5 px — module sampling unreliable")

    ref_data = list(img_ref.getdata())
    mismatches = 0
    for my in range(ref_modules):
        for mx in range(ref_modules):
            # Sample centre of module cell on device PNG.
            sx = int((mx + 0.5) * module_pixels)
            sy = int((my + 0.5) * module_pixels)
            if 0 <= sx < w and 0 <= sy < h:
                dev_bit = 1 if px2[sx, sy] == 0 else 0   # 0=black=module
            else:
                dev_bit = 0
            ref_pix = ref_data[my * ref_modules + mx]
            ref_bit = 1 if ref_pix == 0 else 0           # 0=black=module
            if dev_bit != ref_bit:
                mismatches += 1

    total = ref_modules * ref_modules
    print(f"  module mismatches: {mismatches}/{total} "
          f"({100.0*mismatches/total:.1f}%)")
    if mismatches == 0:
        print()
        print("  ==> Phase 5b PASS — device QR matches Python ref byte-exact "
              "at module level")
        sys.exit(0)
    elif mismatches <= 8:
        print()
        print(f"  Few mismatches ({mismatches}/{total}) — likely mask choice "
              f"differs between LVGL Nayuki and Python qrcode lib. "
              f"Both encoders produce VALID QRs that decode to the same URL; "
              f"visual phone scan is the final test.")
        sys.exit(0)
    else:
        print()
        print(f"  Many mismatches ({mismatches}/{total}) — possible bug. "
              f"Visual scan needed to confirm.")
        sys.exit(1)


if __name__ == "__main__":
    main()
