"""Phase 5b QR verification — SWD-dump the lv_canvas I1 buffer into
a PNG so the user (or any QR reader) can scan it.

Approach:
  1. Drive nav into channel_share_view (B-4)
  2. Read s.qr (lv_obj_t * → lv_canvas with internal lv_draw_buf_t)
  3. Walk lv_obj internals to find the canvas's draw_buf->data ptr
  4. Read N × N / 8 bytes of I1 packed pixels (1 bit per pixel,
     MSB first per LVGL convention)
  5. Save as PNG (PIL).

If LVGL internals shift (lv_obj_t / lv_canvas struct layout depends
on LVGL version), this fallback approach reads from the qr widget
data via lv_canvas_get_draw_buf which is exported. Symbol-resolution
+ small thunk at C side might be needed; for v1 we instead use a
heuristic: scan SRAM for a 144-byte-row * 144-row pattern.

Easier path: have the firmware dump the canvas buf via TRACE on
demand. That requires a small firmware change. For now this
script emits guidance only — visual verification on hardware is
the primary path.
"""
import sys
print("=" * 60)
print("Phase 5b QR visual verification (manual)")
print("=" * 60)
print()
print("Steps:")
print("  1. On hardware, navigate launcher -> Chan -> slot 0 (PRIMARY)")
print("     -> OK to enter channel_edit_view -> press SET key")
print("  2. channel_share_view should now show:")
print("     - top: 144x144 px QR code (white modules on dark BG")
print("       per UI_COLOR_TEXT_PRIMARY / UI_COLOR_BG_PRIMARY)")
print("     - middle: URL text wrapped (~57 chars for default channel)")
print("     - bottom: 'URL N chars  QR OK  BACK'")
print()
print("  3. Open phone camera or any QR scanner, point at screen.")
print("     Decoded URL should be:")
print()

# Auto-fetch the URL the device is currently producing for slot 0,
# so the user has the exact expected string to compare against.
try:
    import re, struct, subprocess, time
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).parent))
    from mokya_swd import MokyaSwd
    from mokya_rtt import build_frame, TYPE_KEY_EVENT

    KEYMAP = {}
    for m in re.finditer(
            r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
            Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
        KEYMAP[m.group(1)] = int(m.group(2), 16)

    ELF = "build/core1_bridge/core1_bridge.elf"
    ARM_OBJDUMP = (r"C:/Program Files/Arm/GNU Toolchain "
                   r"mingw-w64-x86_64-arm-none-eabi/bin/"
                   r"arm-none-eabi-objdump.exe")
    out = subprocess.check_output([ARM_OBJDUMP, "-t", ELF], text=True)
    addr = None
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and 'channel_share_view.c' in line:
            in_block = True; continue
        if in_block:
            if 'df *ABS*' in line: break
            mm = re.match(r'^([0-9a-fA-F]+)\s+l\s+O\s+\.\S*bss\s+\S+\s+s$', line)
            if mm: addr = int(mm.group(1), 16); break
    if addr:
        with MokyaSwd() as swd:
            url_len_bytes = swd.read_mem(addr + 280, 4)
            url_len = struct.unpack('<I', url_len_bytes)[0]
            if 0 < url_len < 250:
                buf = swd.read_mem(addr + 24, url_len)
                url = bytes(buf).decode('utf-8', errors='replace')
                print(f"     {url}")
            else:
                print(f"     (couldn't read URL — url_len={url_len};")
                print(f"      navigate into B-4 first, then re-run)")
except Exception as e:
    print(f"     (URL probe failed: {e})")
    print(f"     Compare against meshtastic --info Primary URL output)")

print()
print("If the QR scan reads back the exact URL above → Phase 5b PASS.")
print("If QR doesn't scan or reads different text → bug.")
print()
print("Auto-decode is unavailable (pyzbar requires libzbar-64.dll)")
print("and qrcode lib is encoder-only. Hence visual verification.")
