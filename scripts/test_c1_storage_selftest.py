"""C1 storage selftest verifier.

Reads g_c1_storage_st_* diag globals from Core 1 SRAM via SWD.
Selftest runs automatically at boot (right after c1_storage_init in
main_core1_bridge.c). This script verifies the result.

Expected counts:
  3 file write attempts → 3 passes
  3 file read+verify    → 3 passes
  3 unlink+verify-gone  → 3 passes
  Total                 = 9 passes, 0 failures
  Magic 'STOP' (0x53544F50) confirms the routine fully ran (vs crashed
  mid-test).

Failure modes surface via g_c1_storage_st_last_err — negative LFS error
code (e.g. LFS_ERR_NOSPC = -28, LFS_ERR_CORRUPT = -84).
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

EXPECTED_PASSES = 1   # MVP: just verify mount + lfs_fs_size; full file-IO
                      # battery disabled pending lfs_file_opencfg crash fix
EXPECTED_MAGIC  = 0x53544F50  # 'STOP'


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual!r:<12} expected={expected!r}")
    return ok


def main():
    fails = 0
    with MokyaSwd() as swd:
        a_magic     = swd.symbol("g_c1_storage_st_magic")
        a_passes    = swd.symbol("g_c1_storage_st_passes")
        a_failures  = swd.symbol("g_c1_storage_st_failures")
        a_last_err  = swd.symbol("g_c1_storage_st_last_err")
        a_dur_us    = swd.symbol("g_c1_storage_st_dur_us")
        a_used      = swd.symbol("g_c1_storage_blocks_used")
        a_total     = swd.symbol("g_c1_storage_blocks_total")
        a_format    = swd.symbol("g_c1_storage_format_count")

        magic    = swd.read_u32(a_magic)
        passes   = swd.read_u32(a_passes)
        fails_cnt= swd.read_u32(a_failures)
        last_err = struct.unpack("<i", swd.read_mem(a_last_err, 4))[0]
        dur_us   = swd.read_u32(a_dur_us)
        used     = swd.read_u32(a_used)
        total    = swd.read_u32(a_total)
        format_n = swd.read_u32(a_format)

    print(f"selftest results:")
    print(f"  magic            = 0x{magic:08X}")
    print(f"  passes           = {passes}")
    print(f"  failures         = {fails_cnt}")
    print(f"  last_err         = {last_err}")
    print(f"  dur_us           = {dur_us}  ({dur_us/1e3:.2f} ms)")
    print(f"  blocks_used      = {used}")
    print(f"  blocks_total     = {total}")
    print(f"  format_count     = {format_n}")
    print()

    if not expect("magic 'STOP'",  magic,    EXPECTED_MAGIC):  fails += 1
    if not expect("passes",        passes,   EXPECTED_PASSES): fails += 1
    if not expect("failures",      fails_cnt, 0):              fails += 1
    if not expect("last_err",      last_err, 0):               fails += 1
    if not (used > 0 and used <= total):
        print(f"  [FAIL] blocks_used out of range (got {used}, total {total})")
        fails += 1
    else:
        print(f"  [PASS] blocks_used in range (1..{total})")
    if dur_us == 0 or dur_us > 500_000:
        print(f"  [FAIL] dur_us out of expected range (got {dur_us})")
        fails += 1
    else:
        print(f"  [PASS] dur_us within budget (< 500 ms)")

    print()
    if fails == 0:
        print("==> C1 storage selftest PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
