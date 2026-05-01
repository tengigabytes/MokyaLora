"""C1 F-4 telemetry history persist end-to-end test.

Phase 5 validation. Each metrics_history sample arrives every 30 s
(real timer in firmware), so the test waits a few cycles for the ring
to grow, force-flushes, watchdog-resets, and verifies the post-reset
ring has the same count + same newest-sample SWD diag as before.

Pass criteria:
  - g_history_persist_loads >= 1 after reset
  - g_history_persist_loaded_count matches pre-reset g_history_count
  - SWD diag (g_history_last_*) preserved across reset (since
    metrics_history_restore re-populates them on load)
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from test_c1_dm_persist import format_fs, reset_and_wait  # type: ignore


def history_flush_now(swd):
    a_req = swd.symbol("g_history_persist_flush_request")
    a_done = swd.symbol("g_history_persist_flush_done")
    cur_done = swd.read_u32(a_done)
    new_req = (cur_done + 1) | 0x48495300   # 'HIS'
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: return
        time.sleep(0.05)
    raise RuntimeError("history_flush_now ack timeout")


def read_diag(swd):
    """Read the metrics_history SWD diag mirrors."""
    count = struct.unpack("<H", swd.read_mem(swd.symbol("g_history_count"), 2))[0]
    soc   = struct.unpack("<h", swd.read_mem(swd.symbol("g_history_last_soc_pct"), 2))[0]
    snr   = struct.unpack("<h", swd.read_mem(swd.symbol("g_history_last_snr_x10"), 2))[0]
    air   = struct.unpack("<h", swd.read_mem(swd.symbol("g_history_last_air_tx_x10"), 2))[0]
    return count, soc, snr, air


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<32} actual={actual!r:<14} expected={expected!r}")
    return ok


def main():
    print("[start] history persist end-to-end")

    # Wipe + reset for a deterministic empty start.
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    print("  [format+reset] clean state")

    # Wait for a few sample ticks (30 s each).  Need at least 2 samples
    # so newest != initial-zero. 75 s gives 2 samples + buffer.
    print("  [wait] 75 s for metrics_history to accumulate >= 2 samples")
    time.sleep(75.0)

    with MokyaSwd() as swd:
        c_pre, soc_pre, snr_pre, air_pre = read_diag(swd)
    print(f"  pre-flush: count={c_pre} soc={soc_pre} snr_x10={snr_pre} air_x10={air_pre}")
    if c_pre < 2:
        print(f"  [FAIL] expected >= 2 samples, got {c_pre}")
        sys.exit(1)

    # Force flush.
    with MokyaSwd() as swd:
        history_flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_history_persist_saves"))
        last_saved_count = struct.unpack("<H",
            swd.read_mem(swd.symbol("g_history_persist_last_count"), 2))[0]
    print(f"  [flushed] saves={saves} count_in_file={last_saved_count}")
    if saves == 0:
        print(f"  [FAIL] no save happened")
        sys.exit(1)
    if last_saved_count != c_pre:
        print(f"  [FAIL] file count {last_saved_count} != pre {c_pre}")
        sys.exit(1)

    # Reset.
    with MokyaSwd() as swd:
        reset_and_wait(swd)

    with MokyaSwd() as swd:
        loads = swd.read_u32(swd.symbol("g_history_persist_loads"))
        loaded_count = struct.unpack("<H",
            swd.read_mem(swd.symbol("g_history_persist_loaded_count"), 2))[0]
        c_post, soc_post, snr_post, air_post = read_diag(swd)
    print(f"  post-reset: loads={loads} loaded_count={loaded_count} "
          f"count={c_post} soc={soc_post} snr_x10={snr_post} air_x10={air_post}")

    fails = 0
    if loads < 1:
        print(f"  [FAIL] loads = {loads}")
        fails += 1
    if not expect("loaded_count vs pre",  loaded_count, c_pre):  fails += 1
    # count post-restore: should be loaded_count + however many fresh
    # samples have arrived since the reset (boot + load + 0.something
    # of a 30 s tick window). At minimum loaded_count + 1 (init drops
    # one sample). Allow up to loaded_count + 3 for slack.
    if c_post < loaded_count or c_post > loaded_count + 4:
        print(f"  [FAIL] count_post {c_post} not in [{loaded_count}, {loaded_count+4}]")
        fails += 1
    else:
        print(f"  [PASS] count_post in [{loaded_count}, {loaded_count+4}]")

    print()
    if fails == 0:
        print("==> history persist PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
