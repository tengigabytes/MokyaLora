"""C1 triple-consumer persist integration test (T1.D1).

Validates that DM, waypoint, and history persisters all work in concert:
inject one of each, flush all, reset, verify each consumer's loads >= 1
and content survives byte-equal where SWD-readable. Also asserts the
power-loss marker (g_c1_storage_pl_count) increments by exactly 1, so
we know we did exactly one chip-wide reset.
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from test_c1_dm_persist import (  # type: ignore
    format_fs, reset_and_wait, inject_serial_bytes, flush_now,
    build_text_frame_from)
from test_c1_waypoint_persist import (  # type: ignore
    waypoint_flush_now, build_waypoint_frame, read_diag as read_wp_diag)
from test_c1_history_persist import (  # type: ignore
    history_flush_now, fill_ring, query_sample, synth_sample)


PEER = 0xCAFE0099
WP_ID = 0xBEEF0033
WP = dict(id=WP_ID, lat=251000000, lon=1213000000, name="integ_wp")
SEED = 0x77


def main():
    print("=== integration: DM + waypoint + history triple-consumer ===")

    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
        pl_pre = swd.read_u32(swd.symbol("g_c1_storage_pl_count"))
    print(f"  [format+reset] pl_count_pre={pl_pre}")

    # Inject one DM via cascade.
    frame = build_text_frame_from(PEER, "integ_dm_text", 0)
    inject_serial_bytes(0x80, frame)
    time.sleep(0.5)
    # Inject one waypoint via cascade.
    wp_frame = build_waypoint_frame(
        peer_id=PEER,
        wp_id=WP["id"], lat_e7=WP["lat"], lon_e7=WP["lon"],
        name=WP["name"], seq=1)
    inject_serial_bytes(0x81, wp_frame)
    time.sleep(0.5)
    # Synthesise 16 history samples.
    with MokyaSwd() as swd:
        fill_ring(swd, seed=SEED, count=16)
    print(f"  [injected] DM + waypoint + 16 history samples")

    # Flush all three persisters.
    with MokyaSwd() as swd:
        flush_now(swd)
        waypoint_flush_now(swd)
        history_flush_now(swd)
        dm_saves = swd.read_u32(swd.symbol("g_dm_persist_saves"))
        wp_saves = swd.read_u32(swd.symbol("g_waypoint_persist_saves"))
        h_saves  = swd.read_u32(swd.symbol("g_history_persist_saves"))
    print(f"  [flushed] dm={dm_saves} wp={wp_saves} hist={h_saves}")
    if dm_saves == 0 or wp_saves == 0 or h_saves == 0:
        print(f"  [FAIL] missing flush(es)")
        sys.exit(1)

    # Reset, verify pl_count increments by exactly 1.
    with MokyaSwd() as swd:
        reset_and_wait(swd)

    fails = 0
    with MokyaSwd() as swd:
        pl_post = swd.read_u32(swd.symbol("g_c1_storage_pl_count"))
        dm_loads = swd.read_u32(swd.symbol("g_dm_persist_loads"))
        wp_loads = swd.read_u32(swd.symbol("g_waypoint_persist_loads"))
        h_loads  = swd.read_u32(swd.symbol("g_history_persist_loads"))
        # DM diag.
        dm_peer  = swd.read_u32(swd.symbol("g_dm_persist_last_peer"))
        dm_tlen  = struct.unpack("<H", swd.read_mem(
            swd.symbol("g_dm_persist_last_text_len"), 2))[0]
        dm_text  = bytes(swd.read_mem(swd.symbol("g_dm_persist_last_text"),
                                       max(dm_tlen, 1)))[:dm_tlen]
        # Waypoint diag.
        wp_id, wp_lat, wp_lon, wp_name = read_wp_diag(swd)
        # History.
        h_count = struct.unpack("<H", swd.read_mem(
            swd.symbol("g_history_count"), 2))[0]
        h_loaded = struct.unpack("<H", swd.read_mem(
            swd.symbol("g_history_persist_loaded_count"), 2))[0]
        # Sample slot 1 (slot 0 may be a fresh post-load sample,
        # depending on timer alignment).
        slot1 = query_sample(swd, 1)
    print(f"  [post-reset] pl={pl_post} dm_loads={dm_loads} wp_loads={wp_loads} h_loads={h_loads}")
    print(f"              dm_peer={dm_peer:#x} dm_text={dm_text!r}")
    print(f"              wp_id={wp_id:#x} wp_name={wp_name!r}")
    print(f"              h_count={h_count} h_loaded={h_loaded} slot1={slot1}")

    if pl_post != pl_pre + 1:
        print(f"  [FAIL] pl_count {pl_pre}→{pl_post}, expected exactly +1"); fails += 1
    if dm_loads < 1:
        print(f"  [FAIL] dm_loads = {dm_loads}"); fails += 1
    if wp_loads < 1:
        print(f"  [FAIL] wp_loads = {wp_loads}"); fails += 1
    if h_loads < 1:
        print(f"  [FAIL] h_loads = {h_loads}"); fails += 1
    if dm_peer != PEER:
        print(f"  [FAIL] dm_peer wrong"); fails += 1
    if dm_text.decode("utf-8", "replace") != "integ_dm_text":
        print(f"  [FAIL] dm_text wrong: {dm_text!r}"); fails += 1
    if wp_id != WP_ID:
        print(f"  [FAIL] wp_id wrong"); fails += 1
    if wp_name != WP["name"]:
        print(f"  [FAIL] wp_name wrong: {wp_name!r}"); fails += 1
    if h_loaded < 16:
        print(f"  [FAIL] h_loaded = {h_loaded}, expected >= 16"); fails += 1
    # Slot 1 from-newest, after at most one new sample lands, should
    # be one of: synth_sample(SEED+15) (no new sample yet, slot1 = old
    # newest's predecessor = SEED+14), synth_sample(SEED+14) (one new
    # sample landed), synth_sample(SEED+13) (two landed). Tolerate +/-2.
    expected_window = [synth_sample(SEED + i) for i in (13, 14, 15)]
    if slot1 not in expected_window:
        print(f"  [FAIL] slot 1 = {slot1}, not in window {expected_window}")
        fails += 1
    else:
        print(f"  [PASS] slot 1 within expected window")

    print()
    if fails == 0:
        print("==> integration PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
