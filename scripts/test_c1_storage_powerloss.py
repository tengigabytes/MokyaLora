"""C1 storage power-loss survival test.

Validates that LFS files survive SYSRESETREQ across multiple cycles,
proving:
  - mount on existing FS works on every boot
  - file written before reset is readable + correct after reset
  - LFS metadata sync via lfs_file_close commits durably

Method:
  c1_storage_init at every boot calls update_pl_marker which:
    1. Reads /.pl_marker (4 B counter, little-endian)
    2. Increments
    3. Writes back
  This script samples g_c1_storage_pl_count, triggers SYSRESETREQ via
  J-Link, samples again. Counter must monotonically grow by 1 per
  reset cycle.

Default: 3 reset cycles. Each cycle ~5 s (boot + USB re-enum).
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

JLINK = "C:/Program Files/SEGGER/JLink_V932/JLink.exe"


def request_watchdog_reset(swd):
    """Trigger chip-wide watchdog reset from Core 1 (sets g_c1_storage_
    reset_request; bridge_task arms HW watchdog with 100 ms timeout +
    spins). Cleaner than JLink SYSRESETREQ which on RP2350 only halts a
    single core's debug context without re-running the bootrom-driven
    cold-start path."""
    swd.write_u32(swd.symbol("g_c1_storage_reset_request"), 1)


def read_pl(swd):
    count    = swd.read_u32(swd.symbol("g_c1_storage_pl_count"))
    existed  = swd.read_u32(swd.symbol("g_c1_storage_pl_existed"))
    last_err = struct.unpack("<i",
                             swd.read_mem(swd.symbol("g_c1_storage_pl_last_err"), 4))[0]
    return count, existed, last_err


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual!r:<6} expected={expected!r}")
    return ok


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    fails = 0

    print(f"[start] {rounds} reset cycles to verify counter monotonic")
    print()

    # Read initial state.
    with MokyaSwd() as swd:
        c0, e0, err0 = read_pl(swd)
    print(f"  initial : count={c0}  existed={e0}  last_err={err0}")
    if err0 != 0:
        print(f"  [WARN] initial last_err={err0}")
    prev = c0

    # Run reset cycles.
    for i in range(rounds):
        print(f"\n  round {i+1}/{rounds}: watchdog reset request ...")
        with MokyaSwd() as swd:
            request_watchdog_reset(swd)
        # Watchdog fires in 100 ms; full Core 0 + Core 1 cold-boot
        # then takes a few seconds (Meshtastic init + Core 1 deferred
        # launch + bridge_task pl_marker call).
        time.sleep(8.0)
        with MokyaSwd() as swd:
            c, e, err = read_pl(swd)
        print(f"    after   : count={c}  existed={e}  last_err={err}")
        if c == prev + 1:
            print(f"    [PASS] count grew by 1 ({prev} → {c})")
        else:
            print(f"    [FAIL] expected count={prev+1}, got {c}")
            fails += 1
        if e != 1:
            print(f"    [FAIL] expected existed=1, got {e}")
            fails += 1
        if err != 0:
            print(f"    [FAIL] last_err={err}")
            fails += 1
        prev = c

    print()
    if fails == 0:
        print(f"==> C1 storage power-loss survival PASS over {rounds} cycles")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
