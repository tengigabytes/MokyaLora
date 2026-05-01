"""C1 waypoint persist end-to-end test.

Mirror of test_c1_dm_persist.py for the Phase 4 waypoint cache. Drives
the cascade WAYPOINT_APP decoder via SWD-injected packets, force-
flushes via dm_persist_flush_now-style trigger, watchdog-resets,
verifies the waypoints are reloaded byte-perfect.

Modes:
  single — 1 waypoint, byte-perfect lat/lon/name verification
  multi  — 5 distinct waypoints, all reload
  ringfill — 8 waypoints (cap), all 8 survive
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

# Reuse SWD utilities from dm test for format / reset / cascade injection.
from test_c1_dm_persist import (  # type: ignore
    format_fs, reset_and_wait, inject_serial_bytes)

from meshtastic.protobuf import mesh_pb2, portnums_pb2  # type: ignore


def waypoint_flush_now(swd):
    """SWD-trigger waypoint_persist_flush_now."""
    a_req = swd.symbol("g_waypoint_persist_flush_request")
    a_done = swd.symbol("g_waypoint_persist_flush_done")
    cur_done = swd.read_u32(a_done)
    new_req = (cur_done + 1) | 0x57500000   # 0x5750 = 'WP'
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: return
        time.sleep(0.05)
    raise RuntimeError("waypoint_flush_now ack timeout")


def build_waypoint_frame(peer_id, wp_id, lat_e7, lon_e7, name, seq,
                          expire=0, locked_to=0, icon=0x1F4CD,
                          description="", rx_time=None):
    """Build a cascade frame carrying a Waypoint protobuf. Defaults
    preserve previous-call shape; new kwargs are for T1.A1 allfields."""
    wp = mesh_pb2.Waypoint()
    wp.id = wp_id
    wp.latitude_i = lat_e7
    wp.longitude_i = lon_e7
    wp.name = name
    wp.expire = expire
    wp.locked_to = locked_to
    wp.icon = icon
    if description:
        wp.description = description
    wp_bytes = wp.SerializeToString()

    mp = mesh_pb2.MeshPacket()
    mp.id = 0xE0000000 | seq
    setattr(mp, "from", peer_id)
    mp.to = 0xFFFFFFFF
    mp.channel = 0
    mp.rx_time = rx_time if rx_time is not None else int(time.time())
    mp.priority = mesh_pb2.MeshPacket.Priority.DEFAULT
    mp.decoded.portnum = portnums_pb2.PortNum.WAYPOINT_APP
    mp.decoded.payload = wp_bytes

    fr = mesh_pb2.FromRadio()
    fr.packet.CopyFrom(mp)
    fr_bytes = fr.SerializeToString()
    L = len(fr_bytes)
    return bytes([0x94, 0xC3, (L >> 8) & 0xFF, L & 0xFF]) + fr_bytes


def read_diag(swd):
    last_id   = swd.read_u32(swd.symbol("g_waypoint_persist_last_loaded_id"))
    last_lat  = struct.unpack("<i", swd.read_mem(
        swd.symbol("g_waypoint_persist_last_loaded_lat_e7"), 4))[0]
    last_lon  = struct.unpack("<i", swd.read_mem(
        swd.symbol("g_waypoint_persist_last_loaded_lon_e7"), 4))[0]
    name_addr = swd.symbol("g_waypoint_persist_last_loaded_name")
    name_raw  = bytes(swd.read_mem(name_addr, 31))
    nul = name_raw.find(b"\x00")
    name = name_raw[:nul].decode("utf-8", "replace") if nul >= 0 else name_raw.decode("utf-8", "replace")
    return last_id, last_lat, last_lon, name


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<30} actual={actual!r:<25} expected={expected!r}")
    return ok


def round_test(waypoints, label):
    """Inject N waypoints, flush, reset, verify reload."""
    print(f"\n=== {label}: {len(waypoints)} waypoints ===")
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    print(f"  [format+reset] clean state")

    # Inject all waypoints.
    for i, w in enumerate(waypoints):
        frame = build_waypoint_frame(
            peer_id=0x538EEBE7,
            wp_id=w["id"],
            lat_e7=w["lat"],
            lon_e7=w["lon"],
            name=w["name"],
            seq=i)
        inject_serial_bytes(0x40 + i, frame)
        time.sleep(0.5)
    time.sleep(2.0)

    with MokyaSwd() as swd:
        d3_total = swd.read_u32(swd.symbol("g_d3_total"))
    if d3_total < len(waypoints):
        print(f"  [FAIL] cascade decoder saw {d3_total}/{len(waypoints)} waypoints")
        return 1
    print(f"  [inject] d3_total={d3_total}")

    with MokyaSwd() as swd:
        waypoint_flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_waypoint_persist_saves"))
        wcount = swd.read_u32(swd.symbol("g_waypoint_persist_count"))
    print(f"  [flushed] saves={saves} count_in_file={wcount}")
    if saves == 0:
        print(f"  [FAIL] no save happened")
        return 1
    if wcount != len(waypoints):
        print(f"  [FAIL] saved count {wcount} != injected {len(waypoints)}")
        return 1

    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        loads = swd.read_u32(swd.symbol("g_waypoint_persist_loads"))
        last_id, last_lat, last_lon, last_name = read_diag(swd)
    print(f"  [reset] loads={loads}  first: id=0x{last_id:08X} "
          f"lat={last_lat} lon={last_lon} name={last_name!r}")

    fails = 0
    if not expect("loads",        loads,    len(waypoints)):  fails += 1
    # The "first loaded" is the lowest-index in the saved file. Order
    # in the saved file is the take_at order which is by epoch_seen
    # newest-first. So first-loaded = latest-injected.
    expected = waypoints[-1]   # last-injected = newest-epoch_seen
    if not expect("first.id",    last_id,   expected["id"]):    fails += 1
    if not expect("first.lat",   last_lat,  expected["lat"]):   fails += 1
    if not expect("first.lon",   last_lon,  expected["lon"]):   fails += 1
    if not expect("first.name",  last_name, expected["name"]):  fails += 1
    return fails


def round_allfields():
    """T1.A1 — single waypoint with distinctive values for all 11
    phoneapi_waypoint_t fields. Verifies byte-perfect round-trip via
    the extended SWD diag globals."""
    print("\n=== allfields: 1 waypoint, all 11 fields verified ===")
    SENDER = 0x538EEBE7
    WP = dict(
        id=0xDEAD0001,
        lat=251234567,
        lon=1219876543,
        name="alpha",                  # 5 chars
        expire=0x12345678,
        locked_to=0xABCDEF01,
        icon=0x0001F525,                 # 🔥
        description="hot spring near hike",
        rx_time=0x60000042,              # epoch_seen sentinel
    )
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    print("  [format+reset] clean state")
    frame = build_waypoint_frame(
        peer_id=SENDER,
        wp_id=WP["id"], lat_e7=WP["lat"], lon_e7=WP["lon"],
        name=WP["name"], seq=0,
        expire=WP["expire"], locked_to=WP["locked_to"],
        icon=WP["icon"], description=WP["description"],
        rx_time=WP["rx_time"])
    inject_serial_bytes(0x40, frame)
    time.sleep(2.0)

    with MokyaSwd() as swd:
        waypoint_flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_waypoint_persist_saves"))
        wcount = swd.read_u32(swd.symbol("g_waypoint_persist_count"))
    print(f"  [flushed] saves={saves} count={wcount}")
    if saves == 0 or wcount != 1:
        print(f"  [FAIL] save state wrong")
        return 1

    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        loads = swd.read_u32(swd.symbol("g_waypoint_persist_loads"))
        # Re-read the basic fields.
        last_id, last_lat, last_lon, last_name = read_diag(swd)
        # Read the 7 new fields.
        last_expire = swd.read_u32(swd.symbol("g_waypoint_persist_last_loaded_expire"))
        last_locked = swd.read_u32(swd.symbol("g_waypoint_persist_last_loaded_locked_to"))
        last_icon   = swd.read_u32(swd.symbol("g_waypoint_persist_last_loaded_icon"))
        last_sender = swd.read_u32(swd.symbol("g_waypoint_persist_last_loaded_sender_id"))
        last_epoch  = swd.read_u32(swd.symbol("g_waypoint_persist_last_loaded_epoch_seen"))
        last_local  = struct.unpack("<B", swd.read_mem(
            swd.symbol("g_waypoint_persist_last_loaded_is_local"), 1))[0]
        desc_raw = bytes(swd.read_mem(
            swd.symbol("g_waypoint_persist_last_loaded_desc"), 32))
    nul = desc_raw.find(b"\x00")
    last_desc = desc_raw[:nul].decode("utf-8", "replace") if nul >= 0 else desc_raw.decode("utf-8", "replace")

    fails = 0
    if loads != 1:
        print(f"  [FAIL] loads = {loads}, expected 1")
        return 1
    if not expect("id",         last_id,     WP["id"]):         fails += 1
    if not expect("lat_e7",     last_lat,    WP["lat"]):        fails += 1
    if not expect("lon_e7",     last_lon,    WP["lon"]):        fails += 1
    if not expect("name",       last_name,   WP["name"]):       fails += 1
    if not expect("expire",     last_expire, WP["expire"]):     fails += 1
    if not expect("locked_to",  last_locked, WP["locked_to"]):  fails += 1
    if not expect("icon",       last_icon,   WP["icon"]):       fails += 1
    if not expect("description",last_desc,   WP["description"]): fails += 1
    if not expect("sender_id",  last_sender, SENDER):           fails += 1
    if not expect("epoch_seen", last_epoch,  WP["rx_time"]):    fails += 1
    if not expect("is_local",   last_local,  0):                fails += 1
    return fails


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "single"
    if mode == "allfields":
        fails = round_allfields()
        print()
        if fails == 0:
            print(f"==> Waypoint persist allfields PASS")
            sys.exit(0)
        else:
            print(f"==> FAIL ({fails} criteria)")
            sys.exit(1)
    if mode == "single":
        waypoints = [
            dict(id=0xCAFEBABE, lat=250521030, lon=1215740390, name="Taipei101"),
        ]
    elif mode == "multi":
        waypoints = [
            dict(id=0xC0FFEE01, lat=250000000, lon=1210000000, name="alpha"),
            dict(id=0xC0FFEE02, lat=250100000, lon=1210100000, name="beta"),
            dict(id=0xC0FFEE03, lat=250200000, lon=1210200000, name="gamma"),
            dict(id=0xC0FFEE04, lat=250300000, lon=1210300000, name="delta"),
            dict(id=0xC0FFEE05, lat=250400000, lon=1210400000, name="epsilon"),
        ]
    elif mode == "ringfill":
        waypoints = [
            dict(id=0xD0000000 | i,
                 lat=250000000 + i * 1000,
                 lon=1210000000 + i * 1000,
                 name=f"wp_{i}")
            for i in range(8)
        ]
    else:
        print(f"unknown mode {mode}"); sys.exit(2)

    fails = round_test(waypoints, mode)
    print()
    if fails == 0:
        print(f"==> Waypoint persist {mode} PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
