"""C1 storage mid-write power-loss survival test.

Stronger than test_c1_storage_powerloss.py (which only validates reset
between writes — clean close + sync). This one resets DURING a stress
write to verify LFS CoW actually protects incomplete transactions.

Per-round protocol:
  1. Read /.pl_marker counter (committed before this boot's stress).
  2. SWD-trigger c1_storage_stress_test (8 files × 1 KB = 24 ops).
  3. Sleep a random 50–300 ms — reset somewhere mid-write phase.
  4. SWD-trigger watchdog reset.
  5. Wait 8 s for cold boot + Core 1 init + pl_marker update.
  6. Verify mount succeeded (existed=1, last_err=0).
  7. Verify pl_marker counter incremented exactly +1 — proves the
     committed file from the previous boot survived a partial-write
     scenario unrelated to it.
  8. Verify a clean stress run can still complete post-reset.

Pass = all rounds satisfy 6/7/8. Fail = mount corruption, marker
loss, or post-reset stress regression.
"""
import random
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from test_c1_storage_stress import encode_request  # type: ignore


def trigger_stress(swd, n_files=8, bytes_per=1024):
    req = encode_request(n_files, bytes_per)
    swd.write_u32(swd.symbol("g_c1_storage_stress_request"), req)
    return req


def trigger_reset(swd):
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
    print(f"  [{tag}] {label:<40} actual={actual!r:<8} expected={expected!r}")
    return ok


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    rng = random.Random(42)   # deterministic delay sequence for reproducibility
    fails = 0

    print(f"[start] {rounds} mid-write reset cycles")
    print()

    with MokyaSwd() as swd:
        c0, e0, err0 = read_pl(swd)
    print(f"  initial pl_count = {c0}")
    prev_count = c0

    for i in range(rounds):
        delay_ms = rng.randint(50, 300)
        print(f"\n  round {i+1}/{rounds}: stress + reset after {delay_ms} ms")
        with MokyaSwd() as swd:
            # Trigger 8x1KB stress (writes ~50 ms × 8 files = 400 ms total)
            trigger_stress(swd, 8, 1024)
            # Wait inside the write phase
            time.sleep(delay_ms / 1000.0)
            # Reset chip
            trigger_reset(swd)

        # Cold-boot recovery time (watchdog 100 ms + Core 0 init + Core 1
        # deferred launch + bridge_task pl_marker update).
        time.sleep(8.0)

        with MokyaSwd() as swd:
            c, e, err = read_pl(swd)
        print(f"    after  pl_count={c} existed={e} last_err={err}")

        if c != prev_count + 1:
            print(f"    [FAIL] pl_count expected {prev_count+1}, got {c}")
            fails += 1
        else:
            print(f"    [PASS] pl_count incremented +1")
        if e != 1:
            print(f"    [FAIL] pl marker not found after reset (existed={e})")
            fails += 1
        if err != 0:
            print(f"    [FAIL] pl_last_err={err}")
            fails += 1

        # Re-run a fresh stress to verify FS still works after the
        # partial-write reset.
        with MokyaSwd() as swd:
            req = trigger_stress(swd, 4, 512)   # smaller cleanup verify
            a_done = swd.symbol("g_c1_storage_stress_done")
            start = time.time()
            while time.time() - start < 10.0:
                if swd.read_u32(a_done) == req: break
                time.sleep(0.1)
            else:
                print(f"    [FAIL] post-reset stress timed out — FS broken?")
                fails += 1
                continue
            sf = swd.read_u32(swd.symbol("g_c1_storage_stress_failures"))
            le = struct.unpack("<i",
                               swd.read_mem(swd.symbol("g_c1_storage_stress_last_err"),
                                            4))[0]
        if le != 0:
            print(f"    [FAIL] post-reset stress last_err={le}")
            fails += 1
        else:
            print(f"    [PASS] post-reset stress clean")

        prev_count = c

    print()
    if fails == 0:
        print(f"==> Mid-write power-loss survival PASS over {rounds} cycles")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria failed)")
        sys.exit(1)


if __name__ == "__main__":
    main()
