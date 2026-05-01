"""C1 flash burst vs. LoRa RX — peer COM7 broadcasts during C1 stress.

Validates that the SIO-spinlock-protected wrap doesn't drop LoRa RX
packets even when C1 is hammering flash with stress writes.

Workload:
  - Peer node on COM7 (TNGBpicoC-ebe7) broadcasts 20 timestamped
    text messages over 60 s (3-second gap = comfortable LoRa duty).
  - During the same window, C1 issues capacity stress rounds (each
    round = 8 files × 1 KB write+read+delete = ~50 ms × 24 ops).
  - Both run via threads from this single host script.
  - Sample MokyaLora's g_dbg_pn_text counter pre/post.

Two passes:
  1. CONTROL — 20 broadcasts, no C1 stress (baseline RF success rate).
  2. STRESSED — 20 broadcasts, with C1 stress in parallel.

Pass criteria: STRESSED count ≥ CONTROL count - 1 (allow 1 packet
slip due to RF noise difference between runs).

If STRESSED count drops more than 1 below CONTROL, the park window
is correlated with packet loss and needs mitigation (shorter wraps,
defer flash ops during active LoRa traffic, etc.)
"""
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from test_c1_storage_stress import encode_request  # type: ignore

PEER_PORT = "COM7"
N_BROADCASTS = 20
INTER_TX_GAP_S = 3.0     # ~3 s between sends; LongFast safe
WINDOW_PAD_S = 5.0       # extra wait at end for in-flight packets

_stop_stress = threading.Event()
_stress_rounds = 0


def c1_stress_thread():
    """SWD-trigger C1 stress in a tight loop until told to stop."""
    global _stress_rounds
    request_a = encode_request(8, 1024)
    request_b = encode_request(8, 1280)
    while not _stop_stress.is_set():
        try:
            with MokyaSwd() as swd:
                req = request_a if (_stress_rounds & 1) == 0 else request_b
                swd.write_u32(swd.symbol("g_c1_storage_stress_request"), req)
                a_done = swd.symbol("g_c1_storage_stress_done")
                start = time.time()
                while time.time() - start < 5.0:
                    if swd.read_u32(a_done) == req: break
                    time.sleep(0.05)
            _stress_rounds += 1
        except Exception as e:
            print(f"  [WARN] stress swd err: {e}")
        time.sleep(0.2)


def read_pn_text():
    """Sample MokyaLora's received-text counter."""
    with MokyaSwd() as swd:
        return swd.read_u32(swd.symbol("g_dbg_pn_text"))


def broadcast_loop():
    """Send N broadcasts from COM7 via meshtastic Python library."""
    from meshtastic.serial_interface import SerialInterface
    iface = SerialInterface(devPath=PEER_PORT)
    try:
        # Wait for connection up before first send.
        time.sleep(2.0)
        sent = 0
        for i in range(N_BROADCASTS):
            try:
                iface.sendText(f"flashtest_{i}")
                sent += 1
            except Exception as e:
                print(f"  [WARN] sendText {i}: {e}")
            time.sleep(INTER_TX_GAP_S)
        return sent
    finally:
        iface.close()


def run_pass(label, with_stress):
    print(f"\n=== {label} ===")
    pre = read_pn_text()
    print(f"  pre  g_dbg_pn_text = {pre}")

    stress_thread = None
    if with_stress:
        global _stress_rounds
        _stop_stress.clear()
        _stress_rounds = 0
        stress_thread = threading.Thread(target=c1_stress_thread, daemon=True)
        stress_thread.start()

    sent = broadcast_loop()
    time.sleep(WINDOW_PAD_S)   # let last packets propagate

    if stress_thread is not None:
        _stop_stress.set()
        stress_thread.join(timeout=10)

    post = read_pn_text()
    received = post - pre
    print(f"  post g_dbg_pn_text = {post}")
    print(f"  sent     = {sent}")
    print(f"  received = {received}")
    if with_stress:
        print(f"  stress rounds completed = {_stress_rounds}")
    return sent, received


def main():
    print(f"[start] live RF stress: {N_BROADCASTS} broadcasts × 2 passes")
    print(f"  peer port = {PEER_PORT}")
    print(f"  TX gap    = {INTER_TX_GAP_S} s")

    sent_ctrl, recv_ctrl = run_pass("CONTROL (no C1 stress)", with_stress=False)
    sent_stress, recv_stress = run_pass("STRESSED (with C1 stress)", with_stress=True)

    print()
    print("=" * 50)
    print(f"control:  {recv_ctrl}/{sent_ctrl} received")
    print(f"stressed: {recv_stress}/{sent_stress} received")
    print()

    if recv_ctrl == 0:
        print("[FAIL] CONTROL received 0 packets — RF link broken / "
              "PKI mismatch / wrong port")
        sys.exit(1)
    if recv_stress < recv_ctrl - 1:
        print(f"[FAIL] stressed loss > 1 packet beyond control "
              f"({recv_stress} vs {recv_ctrl}) — park window dropping LoRa RX")
        sys.exit(1)
    print(f"[PASS] stressed within 1 packet of control "
          f"({recv_ctrl - recv_stress} packet diff)")
    sys.exit(0)


if __name__ == "__main__":
    main()
