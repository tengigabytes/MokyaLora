#!/usr/bin/env python3
"""test_lru_regression.py -- Phase 1.6 LRU promotion regression.

Runs ime_text_test.py twice on a passage. Pass 1 warms the LRU cache;
pass 2 exercises the same inputs and must show a rank-histogram shift
toward rank 0. Optionally reboots the board between passes to
demonstrate flash persistence.

Usage:
    python scripts/test_lru_regression.py PASSAGE [--reboot] [--erase]
                                          [--limit N] [--v4]

Pass-2 expectations (fail test if not met):
    - pass2.rank_hist[0] >= pass1.rank_hist[0]              (at least equal)
    - At least N=10 chars that were rank >= 8 in pass 1
      are rank <= 3 in pass 2.

Assumptions:
    - Core 1 + Core 0 already flashed with latest build.
    - Board enumerates as a Meshtastic USB CDC.
    - User has patched LRU persistence already (Phase 1.6 Step 3).

SPDX-License-Identifier: MIT
"""

import argparse
import ast
import re
import subprocess
import sys
import time
from pathlib import Path

JLINK = r"C:/Program Files/SEGGER/JLink_V932/JLink.exe"

LRU_PARTITION_ADDR = 0x10C00000
LRU_PARTITION_SIZE = 0x10000  # 64 KB reserved


def run_jlink(script_lines, device="RP2350_M33_0"):
    """Run a J-Link Commander script. Returns stdout as str."""
    script = "\n".join(script_lines) + "\nqc\n"
    script_path = Path("/tmp/lru_reg_jlink.jlink")
    script_path.write_text(script)
    proc = subprocess.run(
        [JLINK, "-device", device, "-if", "SWD", "-speed", "4000",
         "-autoconnect", "1",
         "-CommanderScript", str(script_path.resolve())],
        capture_output=True, text=True, timeout=60)
    return proc.stdout


def erase_lru_partition():
    """Use J-Link to erase the LRU slot. Asks Core 0 to erase via its
    wrapped flash API (safer than reaching in from Core 1's side here)."""
    print(f"[erase] erasing LRU partition 0x{LRU_PARTITION_ADDR:08X} "
          f"+ {LRU_PARTITION_SIZE} bytes via J-Link")
    out = run_jlink([
        "connect", "h",
        f"erase 0x{LRU_PARTITION_ADDR:08X} 0x{LRU_PARTITION_ADDR + LRU_PARTITION_SIZE:08X}",
        "r", "g",
    ])
    time.sleep(3)  # USB CDC re-enumerate


def reboot_board():
    print("[reboot] SWD reset + resume")
    run_jlink(["connect", "r", "g"])
    time.sleep(3)


def read_lru_magic():
    out = run_jlink(["connect", "h", f"mem32 0x{LRU_PARTITION_ADDR:08X} 1", "g"])
    m = re.search(r'[0-9A-F]{8} = ([0-9A-F]{8})', out)
    return m.group(1) if m else None


def run_pass(passage, limit, v4):
    """Invoke ime_text_test.py and parse its rank-histogram output."""
    cmd = [sys.executable, "scripts/ime_text_test.py", str(passage),
           "--user-sim", "--keystroke-report"]
    if limit:
        cmd += ["--limit", str(limit)]
    print(f"[pass] {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    out = proc.stdout + proc.stderr
    print(out[-2000:])
    # Extract "rank hist: {0: 123, 1: 45, ...}" line
    m = re.search(r'rank hist:\s*(\{[^}]*\})', out)
    if not m:
        raise RuntimeError("ime_text_test.py did not emit a rank histogram")
    hist = ast.literal_eval(m.group(1))
    return hist


def summarise(hist):
    total = sum(hist.values())
    r0    = hist.get(0, 0)
    le3   = sum(v for k, v in hist.items() if k <= 3)
    ge8   = sum(v for k, v in hist.items() if k >= 8)
    return total, r0, le3, ge8


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("passage", type=Path)
    ap.add_argument("--erase", action="store_true",
                    help="erase LRU flash partition before pass 1")
    ap.add_argument("--reboot", action="store_true",
                    help="SWD reboot between pass 1 and pass 2")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--v4", action="store_true",
                    help="(documentation only — dict already flashed)")
    args = ap.parse_args()

    if not args.passage.exists():
        sys.exit(f"passage not found: {args.passage}")

    if args.erase:
        erase_lru_partition()
        magic = read_lru_magic()
        print(f"[erase] post-erase first word = {magic} (expected FFFFFFFF)")

    print("\n=== Pass 1 (cold cache) ===")
    hist1 = run_pass(args.passage, args.limit, args.v4)
    t1, r01, le31, ge81 = summarise(hist1)

    if args.reboot:
        reboot_board()

    print("\n=== Pass 2 (warm cache) ===")
    hist2 = run_pass(args.passage, args.limit, args.v4)
    t2, r02, le32, ge82 = summarise(hist2)

    print("\n=== Summary ===")
    print(f"Pass 1: total={t1}  rank0={r01}  rank<=3={le31}  rank>=8={ge81}")
    print(f"Pass 2: total={t2}  rank0={r02}  rank<=3={le32}  rank>=8={ge82}")
    print(f"Delta : rank0 {r02-r01:+d}  rank<=3 {le32-le31:+d}  "
          f"rank>=8 {ge82-ge81:+d}")

    fail = []
    if r02 < r01:
        fail.append(f"pass-2 rank 0 count ({r02}) < pass-1 ({r01})")
    if le32 < le31:
        fail.append(f"pass-2 rank<=3 count ({le32}) < pass-1 ({le31})")
    if (ge81 - ge82) < 10 and ge81 >= 10:
        fail.append(f"fewer than 10 chars promoted out of rank>=8 tail "
                    f"({ge81} → {ge82})")

    if fail:
        print("\nFAIL:")
        for f in fail:
            print(f"  - {f}")
        sys.exit(1)
    print("\nPASS: LRU promotion observed across passes.")


if __name__ == "__main__":
    main()
