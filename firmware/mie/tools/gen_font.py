#!/usr/bin/env python3
"""
gen_font.py — MokyaInput Engine Font Compiler
==============================================
Extracts 16×16 bitmap glyphs from GNU Unifont for the 8,104 standard
Traditional Chinese characters defined by the Taiwan Ministry of Education.

Output files (written to ../data/):
  font_glyphs.bin   — packed 16×16 bit-per-pixel glyph data (32 bytes/glyph)
  font_index.bin    — uint32 offset table indexed by Unicode code point

Usage:
  python gen_font.py --unifont unifont-15.1.04.hex --charlist charlist_8104.txt

Dependencies:
  pip install requests  (optional, for automatic Unifont download)
"""

import argparse
import struct
import sys
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "data"
GLYPH_BYTES = 32  # 16×16 pixels, 1 bit per pixel = 32 bytes


def parse_args():
    p = argparse.ArgumentParser(description="Compile Unifont glyphs to MIE binary format")
    p.add_argument("--unifont", required=True, help="Path to unifont .hex file")
    p.add_argument("--charlist", required=True, help="Path to newline-separated Unicode code points (hex)")
    p.add_argument("--output-dir", default=str(OUTPUT_DIR), help="Output directory")
    return p.parse_args()


def load_unifont(hex_path: str) -> dict:
    """Parse a Unifont .hex file into {codepoint: bytes} for 16-wide glyphs."""
    glyphs = {}
    with open(hex_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            cp_str, bitmap_str = line.split(":")
            cp = int(cp_str, 16)
            bitmap = bytes.fromhex(bitmap_str)
            if len(bitmap) == GLYPH_BYTES:  # 16×16 only
                glyphs[cp] = bitmap
    return glyphs


def main():
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Loading Unifont from {args.unifont} ...")
    glyphs = load_unifont(args.unifont)

    with open(args.charlist, "r", encoding="utf-8") as f:
        charlist = [int(line.strip(), 16) for line in f if line.strip()]

    print(f"Compiling {len(charlist)} glyphs ...")
    glyph_data = bytearray()
    index_data = bytearray()  # (codepoint: uint32, offset: uint32) pairs

    for cp in charlist:
        if cp not in glyphs:
            print(f"  WARNING: U+{cp:04X} not found in Unifont, using blank glyph", file=sys.stderr)
            bitmap = bytes(GLYPH_BYTES)
        else:
            bitmap = glyphs[cp]
        offset = len(glyph_data)
        index_data += struct.pack("<II", cp, offset)
        glyph_data += bitmap

    (output_dir / "font_glyphs.bin").write_bytes(glyph_data)
    (output_dir / "font_index.bin").write_bytes(index_data)

    print(f"Done. Glyph data: {len(glyph_data):,} bytes, Index: {len(index_data):,} bytes")
    print(f"Output written to {output_dir}/")


if __name__ == "__main__":
    main()
