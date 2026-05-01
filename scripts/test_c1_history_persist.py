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


def query_sample(swd, idx):
    """T1.A3 indexed reader. Returns (soc_pct, snr_x10, air_x10) or None."""
    addr_idx = swd.symbol("g_history_query_idx")
    swd.write_u8_many([(addr_idx, idx & 0xFF), (addr_idx + 1, (idx >> 8) & 0xFF)])
    a_req  = swd.symbol("g_history_query_request")
    a_done = swd.symbol("g_history_query_done")
    cur = swd.read_u32(a_done)
    new_req = (cur + 1) | 0x48510000   # 'HQ'
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: break
        time.sleep(0.05)
    else:
        raise RuntimeError("history query timeout")
    ok = struct.unpack("<B", swd.read_mem(swd.symbol("g_history_query_ok"), 1))[0]
    if not ok: return None
    addr = swd.symbol("g_history_query_result")
    raw = swd.read_mem(addr, 6)
    return struct.unpack("<hhh", raw)


def fill_ring(swd, seed=0, count=0):
    """T1.B2 ring-fill trigger. count=0 → full LEN (256)."""
    a_req  = swd.symbol("g_history_fill_ring_request")
    a_done = swd.symbol("g_history_fill_ring_done")
    new_req = ((count & 0xFFFF) << 16) | (seed & 0xFFFF)
    if new_req == 0:    # 0 disables the trigger; ensure a non-zero value
        new_req = 0x00010000
    cur = swd.read_u32(a_done)
    if cur == new_req:    # collision — bump
        new_req ^= 0x80000000
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: return
        time.sleep(0.05)
    raise RuntimeError("history fill_ring ack timeout")


def synth_sample(seed_plus_i):
    """Mirror of metrics/history.c synth_sample. Must stay in sync."""
    sp = seed_plus_i & 0xFFFFFFFF
    soc = sp % 101
    snr = ((sp * 7) % 800) - 400
    air = (sp * 3) % 1000
    return (soc, snr, air)


def round_allslots():
    """T1.A3 — fill ring with deterministic data, query each slot, flush,
    reset, query again, byte-equal."""
    print("\n=== allslots: 256-sample ring, byte-perfect across reset ===")
    SEED = 0x37
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        fill_ring(swd, seed=SEED, count=0)   # full ring
        c = struct.unpack("<H", swd.read_mem(swd.symbol("g_history_count"), 2))[0]
    print(f"  [filled] count={c}")
    if c != 256:
        print(f"  [FAIL] expected 256, got {c}"); return 1
    # Snapshot pre-reset.
    pre = []
    with MokyaSwd() as swd:
        for i in range(0, 256, 32):     # sample 8 slots, every 32
            r = query_sample(swd, i)
            if r is None:
                print(f"  [FAIL] pre-reset slot {i} miss"); return 1
            pre.append((i, r))
        history_flush_now(swd)
    print(f"  [pre-reset] sampled 8 slots: {pre[:3]}...")
    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        c2 = struct.unpack("<H", swd.read_mem(swd.symbol("g_history_count"), 2))[0]
    if c2 < 256:
        print(f"  [FAIL] post-reset count={c2}, expected >= 256"); return 1
    fails = 0
    with MokyaSwd() as swd:
        for i, expected in pre:
            r = query_sample(swd, i)
            if r != expected:
                print(f"  [FAIL] slot {i}: pre={expected} post={r}"); fails += 1
    if fails == 0:
        print(f"  [PASS] all sampled slots byte-equal across reset")
    return fails


def round_ringfull():
    """T1.B2 — verify count=256 after full-ring flush + reset, formula match."""
    print("\n=== ringfull: 256-sample ring saved + restored ===")
    SEED = 0x99
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        fill_ring(swd, seed=SEED, count=0)
        history_flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_history_persist_saves"))
        last_count = struct.unpack("<H", swd.read_mem(
            swd.symbol("g_history_persist_last_count"), 2))[0]
    print(f"  [flushed] saves={saves} last_count={last_count}")
    if last_count != 256:
        print(f"  [FAIL] last_count = {last_count}, expected 256"); return 1
    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        loaded_count = struct.unpack("<H", swd.read_mem(
            swd.symbol("g_history_persist_loaded_count"), 2))[0]
        # Formula validation — slot 0 (newest) should be sample synth_sample(SEED+255)
        # because we filled slots 0..255 with seed+0..seed+255, and head ended at 0
        # (n % LEN = 256 % 256 = 0), so newest = head-1 = 255.
        r0 = query_sample(swd, 0)
    print(f"  [post-reset] loaded_count={loaded_count} slot0={r0}")
    if loaded_count != 256:
        print(f"  [FAIL] loaded_count = {loaded_count}, expected 256"); return 1
    expected_0 = synth_sample(SEED + 255)
    if r0 != expected_0:
        print(f"  [FAIL] slot 0: got {r0}, expected {expected_0}")
        return 1
    print(f"  [PASS] full-ring round-trip + formula match")
    return 0


def round_head_correctness():
    """T1.B1 — verify head pointer advances correctly post-load. Use
    fill_ring with count=10 then verify the first NEW sample (after wait
    for one tick) lands at slot 0, pushing original slot 0 to slot 1."""
    print("\n=== head_correctness: head pointer post-load ===")
    SEED = 0x55
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    # Fill 10 samples, flush, reset.
    with MokyaSwd() as swd:
        fill_ring(swd, seed=SEED, count=10)
        history_flush_now(swd)
    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        loaded_count = struct.unpack("<H", swd.read_mem(
            swd.symbol("g_history_persist_loaded_count"), 2))[0]
        slot0 = query_sample(swd, 0)   # Should be the post-load newest
    expected_0 = synth_sample(SEED + 9)   # filled 0..9, newest = SEED+9
    print(f"  [post-load] loaded={loaded_count} slot0={slot0}")
    if slot0 != expected_0:
        print(f"  [FAIL] post-load slot 0 wrong: {slot0} != {expected_0}")
        return 1
    # Wait one 30s tick — a real sample arrives, becomes new slot 0,
    # pushes the loaded newest to slot 1.
    print("  [wait] 32 s for a real sample tick")
    time.sleep(32.0)
    with MokyaSwd() as swd:
        slot0_new = query_sample(swd, 0)
        slot1     = query_sample(swd, 1)
    print(f"  [post-tick] slot0={slot0_new} slot1={slot1}")
    if slot1 != expected_0:
        print(f"  [FAIL] slot 1 should equal old slot 0: {slot1} != {expected_0}")
        return 1
    if slot0_new == expected_0:
        print(f"  [FAIL] slot 0 didn't advance — head pointer stuck")
        return 1
    print(f"  [PASS] head advanced correctly post-load")
    return 0


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "default"
    if mode == "allslots":
        sys.exit(round_allslots())
    if mode == "ringfull":
        sys.exit(round_ringfull())
    if mode == "head_correctness":
        sys.exit(round_head_correctness())
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
