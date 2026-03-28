#!/usr/bin/env python3
"""
gen_font.py — MokyaInput Engine Font Compiler
==============================================
Subsets GNU Unifont TTF to specified Unicode ranges using fonttools,
renders each glyph at 16 px with Pillow, outputs mie_unifont_16.bin
in MIEF v1 binary format (1 bpp, MSB-first, variable bbox).

MIEF v1 format
--------------
Header (12 bytes, little-endian):
  magic[4]     b'MIEF'
  version      uint8  = 1
  px_height    uint8  = 16
  bpp          uint8  = 1
  flags        uint8  (bit 0 = RLE enabled)
  num_glyphs   uint32

Index (num_glyphs × 8 bytes, sorted ascending by codepoint):
  codepoint    uint32
  data_offset  uint32   byte offset into glyph-data section

Glyph-data section (variable-length, pointed to by index):
  adv_w  uint8   advance width in pixels (capped at 255)
  box_w  uint8   bitmap width  (0 = empty / whitespace glyph)
  box_h  uint8   bitmap height (0 = empty / whitespace glyph)
  ofs_x  int8    left bearing from origin
  ofs_y  int8    bottom bearing from baseline (positive = up)
  bitmap bytes   1 bpp, MSB first; each row padded to full byte.
                 Present only when box_w > 0 and box_h > 0.

LVGL integration (Phase 2)
--------------------------
A custom lv_font_t driver (firmware/core1/src/mie_font_driver.c) implements
get_glyph_dsc / get_glyph_bitmap callbacks that binary-search the MIEF index
and DMA-read bitmap data directly from Flash — no PSRAM copy required.

License notice
--------------
GNU Unifont © Roman Czyborra, Paul Hardy, Qianqian Fang et al.
Distributed under the SIL Open Font License 1.1 and the GNU General
Public License v2+ with font exception.
Source: https://unifoundry.com/unifont/

Requires
--------
  pip install fonttools Pillow

Usage
-----
  python gen_font.py [--unifont unifont.ttf] [--out path/to/mie_unifont_16.bin]
                     [--rle] [--no-subset]
"""

import argparse
import io
import struct
import sys
from pathlib import Path

try:
    from fonttools.ttLib import TTFont
    from fonttools.subset import Subsetter
    from fonttools.subset import Options as SubOptions
    from PIL import ImageFont, Image, ImageDraw
except ImportError as exc:
    print(f"ERROR: missing dependency — {exc}\n"
          "Run: pip install fonttools Pillow", file=sys.stderr)
    sys.exit(1)

# ── Constants ─────────────────────────────────────────────────────────────

MAGIC       = b"MIEF"
VERSION     = 1
PX_SIZE     = 16
BPP         = 1
FLAG_RLE    = 0x01

DEFAULT_UNIFONT = "unifont.ttf"
DEFAULT_OUT     = Path(__file__).parent.parent / "data" / "mie_unifont_16.bin"

# Unicode ranges — used for the large (PC) font variant.
# Must match the LVGL driver's expected coverage.
CODEPOINT_RANGES = [
    (0x0020, 0x007F),   # ASCII (printable)
    (0x00A0, 0x00FF),   # Latin-1 Supplement (×÷ etc.)
    (0x2000, 0x206F),   # General Punctuation (— … ↵ etc.)
    (0x3000, 0x303F),   # CJK Symbols & Punctuation (。、「」)
    (0x3100, 0x312F),   # Bopomofo (ㄅ–ㄩ + tone marks ˊˇˋ˙)
    (0x4E00, 0x9FFF),   # CJK Unified Ideographs (common traditional Chinese)
]

# Mandatory ranges always included in the small (embedded) font variant.
# These cover UI chrome, Bopomofo, and CJK punctuation regardless of charlist.
MANDATORY_RANGES = [
    (0x0020, 0x007F),   # ASCII printable
    (0x2000, 0x206F),   # General Punctuation (— … etc.)
    (0x3000, 0x303F),   # CJK Symbols & Punctuation
    (0x3100, 0x312F),   # Bopomofo
    (0x31A0, 0x31BF),   # Bopomofo Extended
    (0xFF00, 0xFFEF),   # Halfwidth/Fullwidth Forms
]

# ── Codepoint enumeration ─────────────────────────────────────────────────

def all_codepoints() -> list:
    cps = []
    for lo, hi in CODEPOINT_RANGES:
        cps.extend(range(lo, hi + 1))
    return cps


def load_charlist(path: str) -> set:
    """Read a charlist file (one UTF-8 character per line), return set of codepoints."""
    cps: set = set()
    with open(path, encoding='utf-8') as f:
        for line in f:
            stripped = line.rstrip('\n')
            if stripped:
                cps.add(ord(stripped[0]))   # take first codepoint only
    return cps


def small_codepoints(charlist_path: str) -> list:
    """Return sorted codepoint list for the small font variant.

    = mandatory ranges ∪ all codepoints from the charlist file.
    Charlist codepoints outside mandatory ranges (e.g. CJK Extension A) are
    included automatically — this ensures full dict coverage.
    """
    mandatory: set = set()
    for lo, hi in MANDATORY_RANGES:
        mandatory.update(range(lo, hi + 1))
    extra = load_charlist(charlist_path)
    return sorted(mandatory | extra)

# ── fonttools subsetting ──────────────────────────────────────────────────

def subset_ttf_bytes(src_path: str, codepoints: list) -> bytes:
    """Return a subsetted TTF as an in-memory bytes object."""
    font = TTFont(src_path)
    opts = SubOptions()
    opts.layout_features = ['*']
    sub = Subsetter(opts)
    sub.populate(unicodes=set(codepoints))
    sub.subset(font)
    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()

# ── Glyph rendering ───────────────────────────────────────────────────────

# Baseline offset from the top of a PX_SIZE cell.
# Pillow's truetype coordinate origin is the top-left of the cell;
# the baseline sits ~3 px above the bottom for a 16 px font.
_DESCENDER_PX = 3

def render_glyph(pil_font, ch: str):
    """
    Render a single character using Pillow.

    Returns:
        (adv_w, ofs_x, ofs_y, box_w, box_h, bitmap_bytes)
        For whitespace or empty glyphs: box_w = box_h = 0, bitmap_bytes = b''.
    """
    try:
        adv_w = int(pil_font.getlength(ch))
    except Exception:
        adv_w = PX_SIZE

    try:
        bbox = pil_font.getbbox(ch)
    except Exception:
        bbox = None

    if bbox is None:
        return (adv_w, 0, 0, 0, 0, b'')

    left, top, right, bottom = bbox
    box_w = right - left
    box_h = bottom - top

    if box_w <= 0 or box_h <= 0:
        return (adv_w, 0, 0, 0, 0, b'')

    # Render to L (greyscale) image — Pillow renders at sub-pixel quality;
    # threshold at 128 to produce clean 1-bpp output for Unifont's pixel font.
    img = Image.new('L', (box_w, box_h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-left, -top), ch, fill=255, font=pil_font)

    # Pack to 1 bpp (MSB first, each row padded to a full byte)
    pixels    = img.load()
    row_bytes = (box_w + 7) // 8
    bitmap    = bytearray(box_h * row_bytes)
    for y in range(box_h):
        for x in range(box_w):
            if pixels[x, y] >= 128:
                bitmap[y * row_bytes + x // 8] |= 0x80 >> (x % 8)

    # ofs_x : pixels to the right of the origin (left bearing)
    ofs_x = left

    # ofs_y : signed offset, positive = glyph sits above baseline.
    # Pillow 'bottom' is the number of px below the cell's top edge.
    # Baseline is at cell_top + (PX_SIZE - _DESCENDER_PX).
    baseline_from_top = PX_SIZE - _DESCENDER_PX
    ofs_y = baseline_from_top - bottom   # positive when glyph is above baseline

    return (min(adv_w, 255), ofs_x, ofs_y, box_w, box_h, bytes(bitmap))

# ── RLE compression ───────────────────────────────────────────────────────

def rle_encode(data: bytes) -> bytes:
    """
    Simple byte-level run-length encoding.
    Format: (count: uint8, value: uint8) pairs.
    Applied per-glyph bitmap; always prefix-safe (decoder knows original length
    from box_w × box_h).
    """
    if not data:
        return b''
    out = bytearray()
    i = 0
    while i < len(data):
        val   = data[i]
        count = 1
        while i + count < len(data) and data[i + count] == val and count < 255:
            count += 1
        out += struct.pack('BB', count, val)
        i += count
    return bytes(out)

# ── MIEF binary builder ───────────────────────────────────────────────────

def build_mief(pil_font, codepoints: list, use_rle: bool,
               charlist_cps: set = None) -> bytes:
    """Build MIEF binary.

    charlist_cps: optional set of codepoints from the charlist file.
    When provided, any charlist codepoint that renders as an empty glyph
    is counted and reported as a warning (potential font coverage gap).
    """
    index      = []        # (codepoint, data_offset)
    glyph_data = bytearray()
    total      = len(codepoints)
    no_glyph_count = 0

    print(f"Rendering {total:,} glyphs ...", flush=True)
    for i, cp in enumerate(codepoints):
        if i % 2000 == 0:
            print(f"  {i:6,} / {total:,}\r", end='', flush=True)

        try:
            ch = chr(cp)
        except (ValueError, OverflowError):
            continue

        adv_w, ofs_x, ofs_y, box_w, box_h, bitmap = render_glyph(pil_font, ch)

        if charlist_cps and cp in charlist_cps and box_w == 0 and box_h == 0:
            no_glyph_count += 1

        bmp_out = rle_encode(bitmap) if (use_rle and bitmap) else bitmap

        offset = len(glyph_data)
        index.append((cp, offset))

        # 5-byte glyph descriptor, then optional bitmap
        glyph_data += struct.pack('BBBbb',
                                  adv_w & 0xFF, box_w, box_h,
                                  max(-128, min(127, ofs_x)),
                                  max(-128, min(127, ofs_y)))
        glyph_data += bmp_out

    print(f"  {total:,} / {total:,}  done.")
    if no_glyph_count:
        print(f"WARNING: {no_glyph_count:,} charlist codepoints had no Unifont glyph "
              f"(will render as blank)", file=sys.stderr)

    flags      = FLAG_RLE if use_rle else 0
    num_glyphs = len(index)

    # Header (12 bytes)
    header = MAGIC + struct.pack('<BBBBI', VERSION, PX_SIZE, BPP, flags, num_glyphs)

    # Index table
    idx_bytes = bytearray()
    for cp, off in index:
        idx_bytes += struct.pack('<II', cp, off)

    return header + bytes(idx_bytes) + bytes(glyph_data)

# ── Argument parsing ──────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description="Compile GNU Unifont OTF/TTF to MIEF binary font for MokyaLora Core 1.")
    p.add_argument('--unifont',   default=DEFAULT_UNIFONT,
                   help=f'GNU Unifont .otf/.ttf path  [default: {DEFAULT_UNIFONT}]')
    p.add_argument('--out',       default=str(DEFAULT_OUT),
                   help=f'Output .bin path  [default: {DEFAULT_OUT}]')
    p.add_argument('--charlist',  metavar='TXT', default=None,
                   help='Charlist file (one UTF-8 char per line) for small font variant. '
                        'When given, only mandatory ranges + charlist codepoints are rendered.')
    p.add_argument('--rle',       action='store_true',
                   help='Enable per-glyph RLE compression')
    p.add_argument('--no-subset', action='store_true',
                   help='Skip fonttools subsetting (uses full font; slower)')
    return p.parse_args()

# ── Main ──────────────────────────────────────────────────────────────────

def main():
    args     = parse_args()
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # Choose codepoint set based on --charlist flag.
    charlist_cps: set = None
    if args.charlist:
        charlist_cps = load_charlist(args.charlist)
        codepoints   = small_codepoints(args.charlist)
        variant      = "small (charlist-subset)"
    else:
        codepoints = all_codepoints()
        variant    = "large (full ranges)"

    print(f"Font variant     : {variant}")
    print(f"Total codepoints : {len(codepoints):,}")
    print(f"RLE compression  : {'on' if args.rle else 'off'}")
    print()

    if not args.no_subset:
        print(f"Subsetting {args.unifont} via fonttools ...")
        ttf_bytes = subset_ttf_bytes(args.unifont, codepoints)
        pil_font  = ImageFont.truetype(io.BytesIO(ttf_bytes), PX_SIZE)
        print(f"  Subsetted font : {len(ttf_bytes):,} bytes")
    else:
        print(f"Loading {args.unifont} (no subset) ...")
        pil_font = ImageFont.truetype(args.unifont, PX_SIZE)

    mief = build_mief(pil_font, codepoints, use_rle=args.rle, charlist_cps=charlist_cps)
    out_path.write_bytes(mief)

    print()
    print(f"Output           : {out_path}")
    print(f"File size        : {len(mief):,} bytes  "
          f"({len(mief)/1024:.1f} KB)")
    print()
    print("License notice:")
    print("  GNU Unifont © Roman Czyborra, Paul Hardy, Qianqian Fang et al.")
    print("  SIL Open Font License 1.1 + GNU GPL v2+ with font exception.")
    print("  https://unifoundry.com/unifont/")


if __name__ == '__main__':
    main()
