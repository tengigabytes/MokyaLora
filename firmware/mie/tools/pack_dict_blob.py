#!/usr/bin/env python3
"""
pack_dict_blob.py -- Combine MIE dictionary .bin files into a single
flash-resident blob for MokyaLora Core 1.

Layout (little-endian, see docs/design-notes/mie-architecture.md):

    0x00  "MDBL"             magic (4 bytes)
    0x04  u16                version = 1
    0x06  u16                reserved (0)
    0x08  u32 zh_dat_off
    0x0C  u32 zh_dat_size
    0x10  u32 zh_val_off
    0x14  u32 zh_val_size
    0x18  u32 en_dat_off
    0x1C  u32 en_dat_size
    0x20  u32 en_val_off
    0x24  u32 en_val_size
    0x28  payload -- each section padded to 16-byte alignment

Sizes of zero are valid (means "section absent"); the loader treats a
zero-size section as "no English dict", etc.

The blob is flashed as a raw partition at a dedicated Core 1 address
(e.g. 0x10300000); Core 1 copies it to PSRAM at boot and hands the
section pointers to mie_dict_open_memory().
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC   = b"MDBL"
VERSION = 1
HEADER_SIZE = 0x28
ALIGN = 16


def pad_to_align(buf: bytearray, align: int) -> None:
    rem = len(buf) % align
    if rem:
        buf.extend(b"\x00" * (align - rem))


def load_section(path: Path | None) -> bytes:
    if path is None or not path.exists():
        return b""
    return path.read_bytes()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zh-dat",  required=True, type=Path)
    ap.add_argument("--zh-val",  required=True, type=Path)
    ap.add_argument("--en-dat",  type=Path, default=None)
    ap.add_argument("--en-val",  type=Path, default=None)
    ap.add_argument("--out",     required=True, type=Path)
    args = ap.parse_args()

    zh_dat = load_section(args.zh_dat)
    zh_val = load_section(args.zh_val)
    en_dat = load_section(args.en_dat)
    en_val = load_section(args.en_val)

    if not zh_dat or not zh_val:
        print("ERROR: zh_dat and zh_val are mandatory", file=sys.stderr)
        return 1

    # Build payload with 16-byte alignment between sections.
    payload = bytearray()
    # Pad to align on the first section too, so HEADER_SIZE + offset stays
    # 16-byte aligned.
    offsets = {}
    for name, data in [("zh_dat", zh_dat), ("zh_val", zh_val),
                       ("en_dat", en_dat), ("en_val", en_val)]:
        pad_to_align(payload, ALIGN)
        offsets[name] = HEADER_SIZE + len(payload) if data else 0
        payload.extend(data)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    struct.pack_into("<HH", header, 4, VERSION, 0)
    struct.pack_into("<II", header, 0x08, offsets["zh_dat"], len(zh_dat))
    struct.pack_into("<II", header, 0x10, offsets["zh_val"], len(zh_val))
    struct.pack_into("<II", header, 0x18, offsets["en_dat"], len(en_dat))
    struct.pack_into("<II", header, 0x20, offsets["en_val"], len(en_val))

    blob = bytes(header) + bytes(payload)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    total = len(blob)
    print(f"  MDBL blob      : {total:>10,} bytes  ({total / 1024 / 1024:.2f} MB)")
    print(f"    zh_dat       : {len(zh_dat):>10,} bytes  @ 0x{offsets['zh_dat']:08x}")
    print(f"    zh_val       : {len(zh_val):>10,} bytes  @ 0x{offsets['zh_val']:08x}")
    print(f"    en_dat       : {len(en_dat):>10,} bytes  @ 0x{offsets['en_dat']:08x}")
    print(f"    en_val       : {len(en_val):>10,} bytes  @ 0x{offsets['en_val']:08x}")
    print(f"  Output         : {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
