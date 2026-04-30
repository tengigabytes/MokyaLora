"""F-3 NeighborInfo cascade decoder — synthetic injection test.

Bypasses the 14400 s broadcast clamp by SWD-injecting a synthetic
NEIGHBORINFO_APP MeshPacket into the c0_to_c1 byte ring, framed
exactly as Core 0's cascade tx would emit it. Core 1's
phoneapi_framing → on_frame() → FR_TAG_PACKET hook → NeighborInfo
decoder fires; SWD diag globals (g_f3_*) capture the result.

Wire structure:
  cascade frame  = 0x94 0xC3 LEN_HI LEN_LO <payload>
  <payload>      = serialized meshtastic.FromRadio protobuf
                   (FromRadio.packet = MeshPacket with
                     decoded.portnum = NEIGHBORINFO_APP (71)
                     decoded.payload = serialized NeighborInfo)

Slot envelope on c0_to_c1_slots[head%32]:
  +0  msg_id u8       = IPC_MSG_SERIAL_BYTES (0x06)
  +1  seq u8
  +2  payload_len u16 LE
  +4  payload bytes (the cascade frame above)
Then bump c0_to_c1_ctrl.head by 1.

Verification: g_f3_total advances; g_f3_last_from / _last_count /
_last_first_node / _last_first_snr_x4 reflect the injected payload.
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

from meshtastic.protobuf import mesh_pb2, portnums_pb2  # type: ignore

IPC_SHARED_ORIGIN  = 0x2007A000
C0_TO_C1_CTRL      = 0x2007A020
C0_TO_C1_SLOTS     = 0x2007A080
SLOT_STRIDE        = 264
IPC_MSG_SERIAL_BYTES = 0x06

JLINK = "C:/Program Files/SEGGER/JLink_V932/JLink.exe"

# Test fixture: peer COM7 + two synthetic neighbours.
TEST_FROM     = 0x538EEBE7   # peer COM7 (real one)
TEST_NBR1_NUM = 0xAABBCC01
TEST_NBR2_NUM = 0xAABBCC02
TEST_SNR1     = 8.5          # +8.5 dB → snr_x4 = 34
TEST_SNR2     = -3.25        # -3.25 dB → snr_x4 = -13


def build_cascade_frame() -> bytes:
    """Build NeighborInfo → MeshPacket → FromRadio → cascade frame."""
    # 1. NeighborInfo with 2 neighbours
    ni = mesh_pb2.NeighborInfo()
    ni.node_id = TEST_FROM           # source self-id
    ni.last_sent_by_id = TEST_FROM
    ni.node_broadcast_interval_secs = 21600
    n1 = ni.neighbors.add(); n1.node_id = TEST_NBR1_NUM; n1.snr = TEST_SNR1
    n2 = ni.neighbors.add(); n2.node_id = TEST_NBR2_NUM; n2.snr = TEST_SNR2
    ni_bytes = ni.SerializeToString()

    # 2. MeshPacket wrapping it ("from" is a Python keyword — use setattr)
    mp = mesh_pb2.MeshPacket()
    mp.id = 0x12345678
    setattr(mp, "from", TEST_FROM)
    mp.to = 0xFFFFFFFF        # broadcast
    mp.channel = 0
    mp.rx_time = int(time.time())
    mp.rx_snr = float(TEST_SNR1)
    mp.rx_rssi = -42
    mp.hop_limit = 3
    mp.hop_start = 3
    mp.priority = mesh_pb2.MeshPacket.Priority.BACKGROUND
    mp.decoded.portnum = portnums_pb2.PortNum.NEIGHBORINFO_APP
    mp.decoded.payload = ni_bytes

    # 3. FromRadio wrapping the MeshPacket
    fr = mesh_pb2.FromRadio()
    fr.packet.CopyFrom(mp)
    fr_bytes = fr.SerializeToString()

    # 4. Cascade framing: 0x94 0xC3 LEN_HI LEN_LO PAYLOAD
    L = len(fr_bytes)
    return bytes([0x94, 0xC3, (L >> 8) & 0xFF, L & 0xFF]) + fr_bytes


def jlink_run(script_text: str) -> str:
    """Run a J-Link Commander script and return stdout."""
    p = Path("/tmp") / f"jlink_f3_{int(time.time()*1000) & 0xFFFFFFFF:x}.jlink"
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
    """Halt Core 0, read c0_to_c1_ctrl.head, resume."""
    out = jlink_run("connect\nh\n"
                    f"mem32 0x{C0_TO_C1_CTRL:X} 1\n"
                    "g\nqc\n")
    # J-Link format: "2007A020 = 00000041"
    target = f"{C0_TO_C1_CTRL:X}".upper()
    for line in out.splitlines():
        if line.upper().startswith(target):
            parts = line.split()
            # parts = ["2007A020", "=", "00000041"]
            if len(parts) >= 3 and parts[1] == "=":
                return int(parts[2].rstrip(','), 16)
    raise RuntimeError(f"couldn't parse head from:\n{out}")


def inject_serial_bytes(seq: int, payload: bytes) -> int:
    """Halt Core 0, write a c0_to_c1 slot + bump head, resume.

    The slot carries IPC_MSG_SERIAL_BYTES + the cascade frame as the
    raw payload — Core 1's bridge reads this, feeds it to the
    phoneapi_framing parser, which reassembles and dispatches."""
    head = read_head()
    slot_idx = head % 32
    slot_addr = C0_TO_C1_SLOTS + slot_idx * SLOT_STRIDE

    plen = len(payload)
    if plen > 256:
        raise ValueError(f"cascade frame too large: {plen} > 256")

    lines = ["connect", "h"]
    # Header: msg_id, seq, payload_len LE
    lines.append(f"w1 0x{slot_addr:X} 0x{IPC_MSG_SERIAL_BYTES:02X}")
    lines.append(f"w1 0x{slot_addr+1:X} 0x{seq & 0xFF:02X}")
    lines.append(f"w1 0x{slot_addr+2:X} 0x{plen & 0xFF:02X}")
    lines.append(f"w1 0x{slot_addr+3:X} 0x{(plen >> 8) & 0xFF:02X}")
    # Payload bytes
    for i, b in enumerate(payload):
        lines.append(f"w1 0x{slot_addr+4+i:X} 0x{b:02X}")
    # Bump head with release semantics (just a u32 store; Core 1's
    # ringbuf consumer does the acquire on its tail<head check).
    lines.append(f"w4 0x{C0_TO_C1_CTRL:X} 0x{head+1:X}")
    lines.append("g")
    lines.append("qc")
    jlink_run("\n".join(lines))
    return head + 1


def main():
    print("[build] crafting synthetic cascade frame")
    frame = build_cascade_frame()
    print(f"  cascade frame len = {len(frame)} B")
    if len(frame) > 256:
        print(f"  [FAIL] frame > 256 B (single slot can't hold)"); sys.exit(1)

    fails = 0

    with MokyaSwd() as swd:
        a_total            = swd.symbol("g_f3_total")
        a_last_from        = swd.symbol("g_f3_last_from")
        a_last_count       = swd.symbol("g_f3_last_count")
        a_last_first_node  = swd.symbol("g_f3_last_first_node")
        a_last_first_snr   = swd.symbol("g_f3_last_first_snr_x4")

        pre_total = swd.read_u32(a_total)
        print(f"  pre-test g_f3_total = {pre_total}")

    print("[inject] writing slot via halt+inject+resume")
    new_head = inject_serial_bytes(0x55, frame)
    print(f"  c0_to_c1 head -> {new_head}")

    # Wait for Core 1 bridge to consume + cascade decoder to fire.
    # Bridge polls every ~tick; 1 s is generous.
    time.sleep(1.5)

    with MokyaSwd() as swd:
        a_total            = swd.symbol("g_f3_total")
        a_last_from        = swd.symbol("g_f3_last_from")
        a_last_count       = swd.symbol("g_f3_last_count")
        a_last_first_node  = swd.symbol("g_f3_last_first_node")
        a_last_first_snr   = swd.symbol("g_f3_last_first_snr_x4")

        post_total = swd.read_u32(a_total)
        delta = post_total - pre_total
        print(f"  post-test g_f3_total = {post_total} (delta={delta})")

        last_from   = swd.read_u32(a_last_from)
        last_count  = struct.unpack("<B", swd.read_mem(a_last_count, 1))[0]
        first_node  = swd.read_u32(a_last_first_node)
        first_snr   = struct.unpack("<b", swd.read_mem(a_last_first_snr, 1))[0]

    print()
    if delta >= 1:
        print(f"  [PASS] g_f3_total advanced (delta={delta})")
    else:
        print(f"  [FAIL] g_f3_total did NOT advance — decoder didn't fire?")
        fails += 1

    if last_from == TEST_FROM:
        print(f"  [PASS] from = 0x{last_from:08X}")
    else:
        print(f"  [FAIL] expected from=0x{TEST_FROM:08X}, got 0x{last_from:08X}")
        fails += 1

    if last_count == 2:
        print(f"  [PASS] count = 2")
    else:
        print(f"  [FAIL] expected count=2, got {last_count}")
        fails += 1

    if first_node == TEST_NBR1_NUM:
        print(f"  [PASS] first.node_num = 0x{first_node:08X}")
    else:
        print(f"  [FAIL] expected first.node_num=0x{TEST_NBR1_NUM:08X}, "
              f"got 0x{first_node:08X}")
        fails += 1

    expected_snr_x4 = round(TEST_SNR1 * 4)
    if first_snr == expected_snr_x4:
        print(f"  [PASS] first.snr_x4 = {first_snr} (= +{first_snr/4:.2f} dB)")
    else:
        print(f"  [FAIL] expected snr_x4={expected_snr_x4}, got {first_snr}")
        fails += 1

    print()
    if fails == 0:
        print("==> F-3 NeighborInfo synthetic injection PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
