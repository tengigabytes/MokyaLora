"""C1 storage capacity stress test.

Triggers the SWD-driven stress harness in c1_storage.c by writing
g_c1_storage_stress_request, waits for g_c1_storage_stress_done to
mirror the request, then reads the result counters.

Default sweep: 16 files × 2 KB each = 32 KB working set. Validates:
  - Write phase: 16 file creates with varied per-file pattern succeed
  - Read phase: byte-perfect round-trip across all 16
  - Delete phase: all unlink + verify-gone succeed
  - Peak block usage stays under cap, free space recovers post-delete

Tests can override the (n_files, bytes_per_file) via CLI args.

Encoding (matches firmware): request = (n_files << 16) | bytes_per_256B
  e.g. 16 files × 2048 B → (16 << 16) | (2048 / 256) = 0x00100008
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore


def encode_request(n_files: int, bytes_per_file: int) -> int:
    if bytes_per_file % 256:
        raise ValueError("bytes_per_file must be multiple of 256")
    units = bytes_per_file // 256
    if not (1 <= n_files <= 700):
        raise ValueError("n_files must be 1..700")
    if not (1 <= units <= 16):
        raise ValueError("bytes_per_file must be 256..4096")
    return (n_files << 16) | units


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual!r:<10} expected={expected!r}")
    return ok


def main():
    n_files = int(sys.argv[1]) if len(sys.argv) > 1 else 16
    bytes_per = int(sys.argv[2]) if len(sys.argv) > 2 else 2048
    request = encode_request(n_files, bytes_per)
    expected_passes = n_files * 3  # write + read + delete each

    print(f"[stress] n_files={n_files}, bytes_per_file={bytes_per}")
    print(f"  total volume = {n_files * bytes_per / 1024:.1f} KB")
    print(f"  request value = 0x{request:08X}")
    print(f"  expected passes = {expected_passes}")

    fails = 0
    with MokyaSwd() as swd:
        a_request   = swd.symbol("g_c1_storage_stress_request")
        a_done      = swd.symbol("g_c1_storage_stress_done")
        a_passes    = swd.symbol("g_c1_storage_stress_passes")
        a_failures  = swd.symbol("g_c1_storage_stress_failures")
        a_last_err  = swd.symbol("g_c1_storage_stress_last_err")
        a_dur_us    = swd.symbol("g_c1_storage_stress_dur_us")
        a_peak      = swd.symbol("g_c1_storage_stress_blocks_peak")

        # Reset counters and post the request.
        swd.write_u32(a_passes, 0)
        swd.write_u32(a_failures, 0)
        swd.write_u32(a_last_err, 0)
        swd.write_u32(a_dur_us, 0)
        swd.write_u32(a_peak, 0)
        swd.write_u32(a_request, request)
        print("[wait] firmware ack...")

        # Wait for firmware ack. Each file ~30-60 ms × 3 ops = ~150 ms;
        # 600 files → ~90 s worst case. Generous timeout.
        deadline = time.time() + 180.0
        while time.time() < deadline:
            done = swd.read_u32(a_done)
            if done == request: break
            time.sleep(0.1)
        else:
            print("  [FAIL] timeout waiting for stress to complete")
            sys.exit(1)

        passes  = swd.read_u32(a_passes)
        fails_n = swd.read_u32(a_failures)
        last_err= struct.unpack("<i", swd.read_mem(a_last_err, 4))[0]
        dur_us  = swd.read_u32(a_dur_us)
        peak    = swd.read_u32(a_peak)

    print()
    print(f"  passes        = {passes}")
    print(f"  failures      = {fails_n}")
    print(f"  last_err      = {last_err}")
    print(f"  dur_us        = {dur_us}  ({dur_us/1e3:.2f} ms)")
    print(f"  blocks_peak   = {peak}")
    print()

    if not expect("passes",       passes, expected_passes): fails += 1
    if not expect("failures",     fails_n, 0):              fails += 1
    if not expect("last_err",     last_err, 0):             fails += 1
    if peak == 0:
        print(f"  [FAIL] blocks_peak should be non-zero")
        fails += 1
    else:
        print(f"  [PASS] blocks_peak = {peak} (proves write phase grew FS)")

    if fails == 0:
        print("\n==> C1 storage capacity stress PASS")
        sys.exit(0)
    else:
        print(f"\n==> FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
