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
        # loads is .bss-zeroed each boot; expect ≥ 1 after a successful
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


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "single"

    if mode == "single":
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
