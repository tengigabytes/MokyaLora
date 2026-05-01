"""C1 DM persist end-to-end test.

Drives the dm_store mutation path via SWD-injected DM packets, forces
flush, watchdog-resets, then verifies the previously-saved DMs are
loaded back into dm_store on the next boot.

Method (per round):
  1. Snapshot baseline: g_dm_persist_loads, dm_store peer_count, etc.
  2. SWD-inject a synthetic TEXT_MESSAGE_APP cascade frame with unique
     text "dm_persist_test_<round>_<seq>" from a designated test peer
     id. This routes through phoneapi_session → dm_store_ingest_inbound
     and marks the peer dirty.
  3. Force-flush via dm_persist_flush_now (SWD-trigger).
  4. SWD-trigger watchdog reset.
  5. Wait 8 s for cold boot + load_all in init path.
  6. Verify: g_dm_persist_loads incremented, dm_store has the peer
     restored with the same text.

Pass criterion: post-reset dm_store contains all DMs we sent before
the reset, with byte-perfect text content.
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

from meshtastic.protobuf import mesh_pb2, portnums_pb2  # type: ignore

C0_TO_C1_CTRL  = 0x2007A020
C0_TO_C1_SLOTS = 0x2007A080
SLOT_STRIDE    = 264
IPC_MSG_SERIAL_BYTES = 0x06

import subprocess
JLINK = "C:/Program Files/SEGGER/JLink_V932/JLink.exe"

TEST_PEER_ID = 0xCAFEBABE   # unique sender id for the test
MOKYA_NODE_NUM = 2975709282 # b862 — destination = self


def build_text_frame(text: str, seq: int) -> bytes:
    """Build a cascade frame for an inbound TEXT_MESSAGE_APP DM."""
    mp = mesh_pb2.MeshPacket()
    mp.id = 0xD0000000 | seq
    setattr(mp, "from", TEST_PEER_ID)
    mp.to = MOKYA_NODE_NUM
    mp.channel = 0
    mp.rx_time = int(time.time())
    mp.rx_snr = 5.0
    mp.rx_rssi = -60
    mp.hop_limit = 3
    mp.hop_start = 3
    mp.priority = mesh_pb2.MeshPacket.Priority.DEFAULT
    mp.decoded.portnum = portnums_pb2.PortNum.TEXT_MESSAGE_APP
    mp.decoded.payload = text.encode("utf-8")

    fr = mesh_pb2.FromRadio()
    fr.packet.CopyFrom(mp)
    fr_bytes = fr.SerializeToString()
    L = len(fr_bytes)
    return bytes([0x94, 0xC3, (L >> 8) & 0xFF, L & 0xFF]) + fr_bytes


def jlink_run(script_text: str) -> str:
    p = Path("/tmp") / f"jlink_dmp_{int(time.time()*1000) & 0xFFFFFFFF:x}.jlink"
    p.write_text(script_text)
    rc = subprocess.run(
        [JLINK, "-device", "RP2350_M33_0", "-if", "SWD", "-speed", "4000",
         "-autoconnect", "1", "-CommanderScript",
         subprocess.run(["cygpath", "-w", str(p)],
                        capture_output=True, text=True).stdout.strip()],
        capture_output=True, text=True, timeout=30)
    p.unlink(missing_ok=True)
    return rc.stdout


def read_head() -> int:
    out = jlink_run("connect\nh\n"
                    f"mem32 0x{C0_TO_C1_CTRL:X} 1\n"
                    "g\nqc\n")
    target = f"{C0_TO_C1_CTRL:X}".upper()
    for line in out.splitlines():
        if line.upper().startswith(target):
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "=":
                return int(parts[2].rstrip(','), 16)
    raise RuntimeError(f"couldn't parse head from:\n{out}")


def inject_serial_bytes(seq: int, payload: bytes) -> int:
    head = read_head()
    slot_idx = head % 32
    slot_addr = C0_TO_C1_SLOTS + slot_idx * SLOT_STRIDE
    plen = len(payload)
    if plen > 256:
        raise ValueError(f"frame too large: {plen}")
    lines = ["connect", "h",
             f"w1 0x{slot_addr:X} 0x{IPC_MSG_SERIAL_BYTES:02X}",
             f"w1 0x{slot_addr+1:X} 0x{seq & 0xFF:02X}",
             f"w1 0x{slot_addr+2:X} 0x{plen & 0xFF:02X}",
             f"w1 0x{slot_addr+3:X} 0x{(plen >> 8) & 0xFF:02X}"]
    for i, b in enumerate(payload):
        lines.append(f"w1 0x{slot_addr+4+i:X} 0x{b:02X}")
    lines.append(f"w4 0x{C0_TO_C1_CTRL:X} 0x{head+1:X}")
    lines.append("g")
    lines.append("qc")
    jlink_run("\n".join(lines))
    return head + 1


def read_diag(swd):
    """Read the SRAM-coherent DM persist diag block."""
    peer    = swd.read_u32(swd.symbol("g_dm_persist_last_peer"))
    count   = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_count"), 1))[0]
    outb    = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_outbound"), 1))[0]
    tlen    = struct.unpack("<H", swd.read_mem(
        swd.symbol("g_dm_persist_last_text_len"), 2))[0]
    text    = bytes(swd.read_mem(swd.symbol("g_dm_persist_last_text"),
                                 max(tlen, 1)))[:tlen]
    return peer, count, outb, tlen, text


def reset_and_wait(swd):
    swd.write_u32(swd.symbol("g_c1_storage_reset_request"), 1)
    time.sleep(8.0)


def format_fs(swd):
    """SWD-trigger c1_storage_format_now; wait for ack."""
    a_req = swd.symbol("g_c1_storage_format_request")
    a_done = swd.symbol("g_c1_storage_format_done")
    cur_done = swd.read_u32(a_done)
    new_req = (cur_done + 1) | 0x42000000   # any unique non-zero value
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 30.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: return
        time.sleep(0.2)
    raise RuntimeError("format_fs ack timeout")


def flush_now(swd):
    """SWD-trigger dm_persist_flush_now; wait for ack. Faster than
    waiting 30 s for the FreeRTOS timer."""
    a_req = swd.symbol("g_dm_persist_flush_request")
    a_done = swd.symbol("g_dm_persist_flush_done")
    cur_done = swd.read_u32(a_done)
    new_req = (cur_done + 1) | 0x21000000
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: return
        time.sleep(0.05)
    raise RuntimeError("flush_now ack timeout")


def expect(label, actual, expected):
    ok = actual == expected
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={actual!r:<20} expected={expected!r}")
    return ok


def round_single_peer(peer_id, n_dms, label):
    """Inject N DMs to one peer, flush, reset, verify byte-perfect."""
    print(f"\n=== {label}: peer=!{peer_id:08X}  {n_dms} DMs ===")
    # Wipe any stale /.dm_* files; then watchdog-reset to also clear the
    # in-memory dm_store ring (PSRAM .psram_bss zero only happens in
    # main()'s psram zero loop on cold boot).
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    print(f"  [format+reset] FS wiped, dm_store cleared")
    with MokyaSwd() as swd:
        saves_pre = swd.read_u32(swd.symbol("g_dm_persist_saves"))
    sent = []
    for i in range(n_dms):
        text = f"dmp_{label}_{i:02d}"
        sent.append(text)
        frame = build_text_frame_from(peer_id, text, i)
        inject_serial_bytes(0x80 + i, frame)
        time.sleep(0.5)
    time.sleep(2.0)
    # SWD-trigger explicit flush — bypasses 30 s timer.
    print(f"  [inject] {n_dms} DMs → flush_now")
    with MokyaSwd() as swd:
        flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_dm_persist_saves"))
    if saves <= saves_pre:
        print(f"  [FAIL] save did not happen (saves stayed at {saves})")
        return 1
    print(f"  [flushed] saves {saves_pre}→{saves}")
    with MokyaSwd() as swd:
        reset_and_wait(swd)

    with MokyaSwd() as swd:
        # loads is .bss-zeroed each boot; expect >= 1 after a successful
        # post-reset load_all.
        loads = swd.read_u32(swd.symbol("g_dm_persist_loads"))
        peer, count, outb, tlen, text = read_diag(swd)

    fails = 0
    expected_oldest = sent[0]   # ring fills oldest-first; oldest = first sent
    if loads < 1:
        print(f"  [FAIL] no peer loaded post-reset (loads={loads})")
        fails += 1
    elif not expect("diag.peer", peer, peer_id):
        fails += 1
    elif not expect("diag.count", count, min(n_dms, 8)):
        fails += 1
    elif not expect("diag.outbound", outb, 0):    # we injected inbound
        fails += 1
    elif not expect("diag.text_len", tlen, len(expected_oldest)):
        fails += 1
    elif text.decode("utf-8", "replace") != expected_oldest:
        print(f"  [FAIL] diag.text mismatch: {text!r} != {expected_oldest!r}")
        fails += 1
    else:
        print(f"  [PASS] byte-perfect round-trip: {text.decode()!r}")

    return fails


def build_text_frame_from(peer_id, text, seq):
    """Same as build_text_frame but with explicit peer_id."""
    mp = mesh_pb2.MeshPacket()
    mp.id = 0xD0000000 | seq
    setattr(mp, "from", peer_id)
    mp.to = MOKYA_NODE_NUM
    mp.channel = 0
    mp.rx_time = int(time.time())
    mp.rx_snr = 5.0
    mp.rx_rssi = -60
    mp.hop_limit = 3
    mp.hop_start = 3
    mp.priority = mesh_pb2.MeshPacket.Priority.DEFAULT
    mp.decoded.portnum = portnums_pb2.PortNum.TEXT_MESSAGE_APP
    mp.decoded.payload = text.encode("utf-8")
    fr = mesh_pb2.FromRadio()
    fr.packet.CopyFrom(mp)
    fr_bytes = fr.SerializeToString()
    L = len(fr_bytes)
    return bytes([0x94, 0xC3, (L >> 8) & 0xFF, L & 0xFF]) + fr_bytes


def write_text_to_diag(swd, text_bytes):
    """SWD-write text bytes into g_dm_persist_last_text (shared with the
    outbound-DM injection trigger)."""
    addr = swd.symbol("g_dm_persist_last_text")
    swd.write_u8_many([(addr + i, b) for i, b in enumerate(text_bytes)])


def inject_outbound(swd, peer_id, packet_id, want_ack, text):
    """T1.A2 outbound DM injection trigger."""
    text_bytes = text.encode("utf-8")
    write_text_to_diag(swd, text_bytes)
    swd.write_u32(swd.symbol("g_dm_persist_last_text_len"), 0)  # clear stale flag
    swd.write_u32(swd.symbol("g_dm_inject_outbound_peer"), peer_id)
    swd.write_u32(swd.symbol("g_dm_inject_outbound_packet_id"), packet_id)
    addr_wa = swd.symbol("g_dm_inject_outbound_want_ack")
    swd.write_u8_many([(addr_wa, 1 if want_ack else 0)])
    # u16 text_len
    addr_len = swd.symbol("g_dm_inject_outbound_text_len")
    swd.write_u8_many([(addr_len, len(text_bytes) & 0xFF),
                       (addr_len + 1, (len(text_bytes) >> 8) & 0xFF)])
    a_req  = swd.symbol("g_dm_inject_outbound_request")
    a_done = swd.symbol("g_dm_inject_outbound_done")
    cur = swd.read_u32(a_done)
    new_req = (cur + 1) | 0x4F420000   # 'OB'
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: return
        time.sleep(0.05)
    raise RuntimeError("outbound inject ack timeout")


def inject_ack_update(swd, packet_id, state):
    """T1.A2 ack-state update trigger. state = 0/1/2/3
    (NONE/SENDING/DELIVERED/FAILED)."""
    swd.write_u32(swd.symbol("g_dm_inject_ack_packet_id"), packet_id)
    swd.write_u8_many([(swd.symbol("g_dm_inject_ack_state"), state & 0xFF)])
    a_req  = swd.symbol("g_dm_inject_ack_request")
    a_done = swd.symbol("g_dm_inject_ack_done")
    cur = swd.read_u32(a_done)
    new_req = (cur + 1) | 0x41430000   # 'AC'
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: return
        time.sleep(0.05)
    raise RuntimeError("ack inject ack timeout")


def query_msg(swd, peer_id, idx):
    """T1.B3 indexed reader. Returns dict of read fields, or None on miss."""
    swd.write_u32(swd.symbol("g_dm_query_peer_id"), peer_id)
    swd.write_u8_many([(swd.symbol("g_dm_query_idx"), idx & 0xFF)])
    a_req  = swd.symbol("g_dm_query_request")
    a_done = swd.symbol("g_dm_query_done")
    cur = swd.read_u32(a_done)
    new_req = (cur + 1) | 0x51520000   # 'QR'
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: break
        time.sleep(0.05)
    else:
        raise RuntimeError("query ack timeout")
    ok = struct.unpack("<B", swd.read_mem(swd.symbol("g_dm_query_ok"), 1))[0]
    if not ok: return None
    # Reuses the load-time diag mirror.
    return read_full_diag(swd)


def read_full_diag(swd):
    """Read the full extended dm_persist diag block (T1.A2 fields)."""
    d = {}
    d["peer"]        = swd.read_u32(swd.symbol("g_dm_persist_last_peer"))
    d["count"]       = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_count"), 1))[0]
    d["outbound"]    = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_outbound"), 1))[0]
    d["text_len"]    = struct.unpack("<H", swd.read_mem(
        swd.symbol("g_dm_persist_last_text_len"), 2))[0]
    d["text"]        = bytes(swd.read_mem(swd.symbol("g_dm_persist_last_text"),
                                          max(d["text_len"], 1)))[:d["text_len"]]
    d["seq"]         = swd.read_u32(swd.symbol("g_dm_persist_last_seq"))
    d["epoch"]       = swd.read_u32(swd.symbol("g_dm_persist_last_epoch"))
    d["packet_id"]   = swd.read_u32(swd.symbol("g_dm_persist_last_packet_id"))
    d["ack_state"]   = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_ack_state"), 1))[0]
    d["ack_epoch"]   = swd.read_u32(swd.symbol("g_dm_persist_last_ack_epoch"))
    d["rx_snr_x4"]   = struct.unpack("<h", swd.read_mem(
        swd.symbol("g_dm_persist_last_rx_snr_x4"), 2))[0]
    d["rx_rssi"]     = struct.unpack("<h", swd.read_mem(
        swd.symbol("g_dm_persist_last_rx_rssi"), 2))[0]
    d["hop_limit"]   = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_hop_limit"), 1))[0]
    d["hop_start"]   = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_hop_start"), 1))[0]
    d["want_ack"]    = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_want_ack"), 1))[0]
    d["unread"]      = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_dm_persist_last_unread"), 1))[0]
    return d


def corrupt_file(swd, path, offset, value):
    """T1.C1/C2 file-corruption trigger."""
    path_bytes = path.encode("utf-8") + b"\x00"
    if len(path_bytes) > 32:
        raise ValueError(f"path too long: {path}")
    addr_path = swd.symbol("g_c1_storage_corrupt_path")
    swd.write_u8_many([(addr_path + i, b) for i, b in enumerate(path_bytes)])
    swd.write_u32(swd.symbol("g_c1_storage_corrupt_offset"), offset)
    swd.write_u8_many([(swd.symbol("g_c1_storage_corrupt_value"), value & 0xFF)])
    a_req  = swd.symbol("g_c1_storage_corrupt_request")
    a_done = swd.symbol("g_c1_storage_corrupt_done")
    cur = swd.read_u32(a_done)
    new_req = (cur + 1) | 0x43420000   # 'CB'
    swd.write_u32(a_req, new_req)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if swd.read_u32(a_done) == new_req: break
        time.sleep(0.05)
    else:
        raise RuntimeError("corrupt-file ack timeout")
    ok = struct.unpack("<B", swd.read_mem(
        swd.symbol("g_c1_storage_corrupt_ok"), 1))[0]
    err = struct.unpack("<i", swd.read_mem(
        swd.symbol("g_c1_storage_corrupt_last_err"), 4))[0]
    if not ok:
        raise RuntimeError(f"corrupt_file failed: rc={err}")


def round_outbound():
    """T1.A2 — outbound DM persists with all metadata fields."""
    print("\n=== outbound: 1 outbound DM, byte-perfect verification ===")
    PEER = 0xBEEF1234
    PACKET_ID = 0xDEADCAFE
    TEXT = "outbound test msg"
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    print("  [format+reset] clean state")
    with MokyaSwd() as swd:
        inject_outbound(swd, PEER, PACKET_ID, want_ack=True, text=TEXT)
        # Mark it delivered.
        time.sleep(0.2)
        inject_ack_update(swd, PACKET_ID, state=2)   # DM_ACK_DELIVERED
        time.sleep(0.2)
        flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_dm_persist_saves"))
    print(f"  [flushed] saves={saves}")
    if saves == 0:
        print("  [FAIL] no save"); return 1
    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        loads = swd.read_u32(swd.symbol("g_dm_persist_loads"))
        d = read_full_diag(swd)
    print(f"  [reset] loads={loads} d={d}")
    fails = 0
    if loads != 1:
        print(f"  [FAIL] loads = {loads}, expected 1")
        return 1
    if not expect("peer",       d["peer"],      PEER):           fails += 1
    if not expect("count",      d["count"],     1):              fails += 1
    if not expect("outbound",   d["outbound"],  1):              fails += 1
    if not expect("text_len",   d["text_len"],  len(TEXT)):      fails += 1
    if d["text"].decode("utf-8", "replace") != TEXT:
        print(f"  [FAIL] text mismatch: {d['text']!r} != {TEXT!r}"); fails += 1
    if not expect("packet_id",  d["packet_id"], PACKET_ID):      fails += 1
    if not expect("ack_state",  d["ack_state"], 2):              fails += 1
    if not expect("want_ack",   d["want_ack"],  1):              fails += 1
    if d["ack_epoch"] == 0:
        print(f"  [FAIL] ack_epoch = 0, expected non-zero"); fails += 1
    return fails


def round_inbound_meta():
    """T1.A2 — inbound DM with non-default snr/rssi/hop fields."""
    print("\n=== inbound_meta: 1 inbound DM, radio metadata fields ===")
    PEER = 0xCAFEABCD
    TEXT = "inbound meta test"
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    # Build a frame with distinctive snr/rssi/hop values.
    mp = mesh_pb2.MeshPacket()
    mp.id = 0xD0000111
    setattr(mp, "from", PEER)
    mp.to = MOKYA_NODE_NUM
    mp.channel = 0
    mp.rx_time = int(time.time())
    mp.rx_snr = 5.25                      # → snr_x4 = 21
    mp.rx_rssi = -65                      # → -65 signed
    mp.hop_limit = 2
    mp.hop_start = 4
    mp.priority = mesh_pb2.MeshPacket.Priority.DEFAULT
    mp.decoded.portnum = portnums_pb2.PortNum.TEXT_MESSAGE_APP
    mp.decoded.payload = TEXT.encode("utf-8")
    fr = mesh_pb2.FromRadio()
    fr.packet.CopyFrom(mp)
    fr_bytes = fr.SerializeToString()
    L = len(fr_bytes)
    frame = bytes([0x94, 0xC3, (L >> 8) & 0xFF, L & 0xFF]) + fr_bytes
    inject_serial_bytes(0x80, frame)
    time.sleep(2.0)
    with MokyaSwd() as swd:
        flush_now(swd)
    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        d = read_full_diag(swd)
    print(f"  [reset] d={d}")
    fails = 0
    if not expect("peer",       d["peer"],      PEER):           fails += 1
    if not expect("outbound",   d["outbound"],  0):              fails += 1
    if not expect("rx_snr_x4",  d["rx_snr_x4"], 21):             fails += 1
    if not expect("rx_rssi",    d["rx_rssi"],   -65):            fails += 1
    if not expect("hop_limit",  d["hop_limit"], 2):              fails += 1
    if not expect("hop_start",  d["hop_start"], 4):              fails += 1
    if not expect("unread",     d["unread"],    1):              fails += 1
    return fails


def round_allslots():
    """T1.B3 — 8 distinct DMs to one peer, snapshot all 8 via the
    indexed reader, flush, reset, snapshot again, byte-equal."""
    print("\n=== allslots: 8 DMs to 1 peer, byte-perfect across reset ===")
    PEER = 0xCAFEABCD
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    sent = [f"slot_{i}_{0xC0DE0000 | i:08x}" for i in range(8)]
    for i, t in enumerate(sent):
        frame = build_text_frame_from(PEER, t, i)
        inject_serial_bytes(0x80 + i, frame)
        time.sleep(0.3)
    time.sleep(1.0)
    # Snapshot pre-reset via indexed reader.
    pre = []
    with MokyaSwd() as swd:
        for idx in range(8):
            r = query_msg(swd, PEER, idx)
            if r is None:
                print(f"  [FAIL] pre-reset query idx={idx} returned None"); return 1
            pre.append(r["text"].decode("utf-8", "replace"))
        flush_now(swd)
    print(f"  [pre-reset] {pre}")
    with MokyaSwd() as swd:
        reset_and_wait(swd)
    post = []
    with MokyaSwd() as swd:
        for idx in range(8):
            r = query_msg(swd, PEER, idx)
            if r is None:
                print(f"  [FAIL] post-reset query idx={idx} returned None"); return 1
            post.append(r["text"].decode("utf-8", "replace"))
    print(f"  [post-reset] {post}")
    fails = 0
    for i, (a, b) in enumerate(zip(pre, post)):
        if a != b:
            print(f"  [FAIL] slot {i}: pre={a!r} post={b!r}"); fails += 1
    if fails == 0:
        print(f"  [PASS] all 8 slots byte-equal across reset")
    return fails


def round_corrupt(corrupt_kind):
    """T1.C1/C2 — inject 2 peers, corrupt one's magic/version, reset,
    verify only the un-corrupted peer loads, failures > 0, no crash."""
    print(f"\n=== corrupt_{corrupt_kind}: 2 peers, 1 corrupted, isolated load ===")
    PEER_GOOD = 0xCAFE0001
    PEER_BAD  = 0xCAFE0002
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    # Inject 1 DM each, flush both.
    for i, peer in enumerate([PEER_GOOD, PEER_BAD]):
        frame = build_text_frame_from(peer, f"corr_{i}", i)
        inject_serial_bytes(0x80 + i, frame)
        time.sleep(0.3)
    time.sleep(1.0)
    with MokyaSwd() as swd:
        flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_dm_persist_saves"))
    if saves < 2:
        print(f"  [FAIL] saves = {saves}, expected >= 2"); return 1
    # Corrupt the bad peer's file (offset 0 = magic LSB).
    bad_path = f"/.dm_{PEER_BAD:08x}"
    offset = 0 if corrupt_kind == "magic" else 4   # magic: bytes 0-3, version: 4-7
    with MokyaSwd() as swd:
        corrupt_file(swd, bad_path, offset, 0xFF)
    print(f"  [corrupted] {bad_path} offset={offset}")
    # Reset, verify only PEER_GOOD loads.
    with MokyaSwd() as swd:
        reset_and_wait(swd)
    with MokyaSwd() as swd:
        loads     = swd.read_u32(swd.symbol("g_dm_persist_loads"))
        failures  = swd.read_u32(swd.symbol("g_dm_persist_failures"))
        last_err  = struct.unpack("<i", swd.read_mem(
            swd.symbol("g_dm_persist_last_err"), 4))[0]
    print(f"  [post-reset] loads={loads} failures={failures} last_err={last_err}")
    fails = 0
    if loads != 1:
        print(f"  [FAIL] loads = {loads}, expected exactly 1"); fails += 1
    if failures < 1:
        print(f"  [FAIL] failures = {failures}, expected >= 1"); fails += 1
    return fails


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "single"

    if mode == "outbound":
        fails = round_outbound()
    elif mode == "inbound_meta":
        fails = round_inbound_meta()
    elif mode == "allslots":
        fails = round_allslots()
    elif mode == "corrupt_magic":
        fails = round_corrupt("magic")
    elif mode == "corrupt_version":
        fails = round_corrupt("version")
    elif mode == "single":
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 3
        fails = round_single_peer(TEST_PEER_ID, n, "single")
    elif mode == "multi":
        # 3 different peers, 2 DMs each. Each round: format+reset+inject+
        # flush+reset+verify. Add settle between rounds — back-to-back
        # watchdog resets without breathing room can leave a flush timer
        # in an indeterminate state.
        peers = [0xCAFE0001, 0xCAFE0002, 0xCAFE0003]
        fails = 0
        for i, p in enumerate(peers):
            if i > 0:
                time.sleep(3.0)
            fails += round_single_peer(p, 2, f"multi_{i}")
    elif mode == "evict":
        # Two-phase scenario to exercise on-disk eviction cleanup:
        #   Phase A: inject 8 peers, flush → 8 files on disk
        #   Phase B: inject a 9th peer (in-memory evicts oldest peer 0;
        #            peer 0's file STAYS on disk as orphan)
        #   Phase C: flush → saves new peer (now 8 in dm_store, 9 on disk)
        #   Phase D: reset, verify load_all loads 8 + cleanup unlinks 1
        n_initial = 8
        peers_init = [0xCAFE9000 + i for i in range(n_initial)]
        peer_extra = 0xCAFE9100
        print(f"\n=== evict: {n_initial} peers + 1 extra to trigger cleanup ===")
        with MokyaSwd() as swd:
            format_fs(swd)
            reset_and_wait(swd)
        print(f"  [format+reset] FS wiped")

        # Phase A: 8 peers, flush.
        for i, p in enumerate(peers_init):
            frame = build_text_frame_from(p, f"ev_init_{i}", i)
            inject_serial_bytes(0x80 + i, frame)
            time.sleep(0.2)
        time.sleep(1.0)
        with MokyaSwd() as swd:
            flush_now(swd)
            saves_a = swd.read_u32(swd.symbol("g_dm_persist_saves"))
        print(f"  [phase A] 8 peers saved, saves={saves_a}")

        # Phase B+C: 9th peer evicts oldest in-memory; flush saves new one.
        frame = build_text_frame_from(peer_extra, "ev_extra", 99)
        inject_serial_bytes(0xA0, frame)
        time.sleep(1.0)
        with MokyaSwd() as swd:
            flush_now(swd)
            saves_c = swd.read_u32(swd.symbol("g_dm_persist_saves"))
        print(f"  [phase C] 9th peer injected+flushed, saves={saves_c}")

        # Phase D: reset, verify load+cleanup.
        with MokyaSwd() as swd:
            reset_and_wait(swd)
        with MokyaSwd() as swd:
            loads = swd.read_u32(swd.symbol("g_dm_persist_loads"))
            orphans = swd.read_u32(swd.symbol("g_dm_persist_orphans_unlinked"))
        print(f"  [phase D] post-reset loads={loads} orphans_unlinked={orphans}")
        fails = 0
        # 9 files on disk → load_all attempts to restore all 9 (so loads >= 9
        # since dm_store_restore_peer succeeds even when eviction happens
        # internally).  Cleanup pass must then unlink the 1 orphan whose
        # peer slot got evicted.
        if loads < 9:
            print(f"  [FAIL] loads expected >= 9, got {loads}")
            fails += 1
        else:
            print(f"  [PASS] loads (>= 9) actual={loads}")
        if not expect("orphans_unlinked",    orphans, 1):  fails += 1
    elif mode == "ringoverflow":
        # 12 DMs to one peer; ring cap is 8 → only last 8 survive.
        # Verify diag.count = 8 and diag.text reflects oldest-of-survivors.
        peer = 0xCAFEC001
        n = 12
        print(f"\n=== ringoverflow: peer=!{peer:08X}  {n} DMs (cap 8) ===")
        sent = []
        for i in range(n):
            text = f"dmp_ro_{i:02d}"
            sent.append(text)
            frame = build_text_frame_from(peer, text, i)
            inject_serial_bytes(0x80 + i, frame)
            time.sleep(0.5)
        time.sleep(2.0)
        print(f"  [inject] {n} DMs → 32 s flush wait")
        time.sleep(32.0)
        with MokyaSwd() as swd:
            reset_and_wait(swd)
        with MokyaSwd() as swd:
            peer_v, count, outb, tlen, text = read_diag(swd)
        # After ring overflow, oldest-survivor is sent[n - 8] = sent[4] = "dmp_ro_04"
        oldest_surv = sent[n - 8]
        fails = 0
        if not expect("diag.count (cap)", count, 8):     fails += 1
        if not expect("diag.peer",        peer_v, peer): fails += 1
        if text.decode("utf-8", "replace") != oldest_surv:
            print(f"  [FAIL] expected oldest-survivor {oldest_surv!r}, got {text!r}")
            fails += 1
        else:
            print(f"  [PASS] FIFO ring: oldest = {text.decode()!r} (sent[{n-8}])")
    else:
        print(f"unknown mode: {mode}")
        sys.exit(2)

    print()
    if fails == 0:
        print(f"==> DM persist {mode} mode PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
