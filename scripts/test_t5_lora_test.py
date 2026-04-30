"""Phase 4 verification — T-5 LoRa passive metrics.

Strategy:
  1. Read pre-test g_t5_rx_count.
  2. Trigger TEXT broadcast from peer COM7.
  3. Poll g_t5_rx_count until it advances (or timeout).
  4. Verify delta = 1, g_t5_last_snr_x4 populated, and the LoRa
     test view renders without crash.
"""
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from mokya_rtt import build_frame, TYPE_KEY_EVENT  # type: ignore

KEYMAP = {}
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME    = 0
VIEW_ID_TOOLS        = 18
VIEW_ID_T5_LORA_TEST = 30


def k(swd, kc, gap=200):
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([(kc & 0x7F) | 0x80, 0])))
    time.sleep(0.03)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([kc & 0x7F, 0])))
    time.sleep(gap / 1000)


def back_home(swd):
    for _ in range(8):
        if swd.read_u32(swd.symbol('s_view_router_active')) == VIEW_ID_BOOT_HOME:
            return True
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 300)
    return False


def main():
    fails = 0
    token = f"T5audit{int(time.time()) & 0xFFFF:04x}"
    print(f"Test token: {token}")

    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! BOOT_HOME unreachable"); sys.exit(1)

        # Symbol resolution
        a_rx        = swd.symbol('g_t5_rx_count')
        a_ack       = swd.symbol('g_t5_ack_count')
        a_nack      = swd.symbol('g_t5_nack_count')
        a_snr       = swd.symbol('g_t5_last_snr_x4')
        a_rssi      = swd.symbol('g_t5_last_rssi')
        a_qfree     = swd.symbol('g_t5_queue_free')
        a_qmax      = swd.symbol('g_t5_queue_max')

        # Navigate to T-5 so render() pushes initial diag values.
        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 400)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'], 60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        k(swd, KEYMAP['MOKYA_KEY_DOWN'], 80)
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        for _ in range(11): k(swd, KEYMAP['MOKYA_KEY_UP'], 40)
        for _ in range(4):  k(swd, KEYMAP['MOKYA_KEY_DOWN'], 60)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_T5_LORA_TEST:
            print(f"!! expected T5 ({VIEW_ID_T5_LORA_TEST}), got {v}")
            sys.exit(1)
        time.sleep(0.5)

        pre_rx = swd.read_u32(a_rx)
        print(f"  pre-test rx_count = {pre_rx}")

        # Broadcast from peer.
        print(f"  sending from COM7: {token!r}")
        subprocess.run(
            ['python', '-m', 'meshtastic', '--port', 'COM7', '--sendtext', token],
            capture_output=True, text=True, timeout=30)

        # Poll until rx_count advances (forces re-render via change_seq).
        deadline = time.time() + 30.0
        post_rx = pre_rx
        while time.time() < deadline:
            post_rx = swd.read_u32(a_rx)
            if post_rx > pre_rx:
                break
            time.sleep(0.5)
            # Tickle the view so refresh fires (cursor up = no-op render).
            k(swd, KEYMAP['MOKYA_KEY_UP'], 100)

        delta = post_rx - pre_rx
        print(f"  post-test rx_count = {post_rx} (delta = {delta})")

        # Re-read snapshot.
        time.sleep(0.5)
        snr_x4 = struct.unpack('<b', swd.read_mem(a_snr, 1))[0]
        rssi   = struct.unpack('<h', swd.read_mem(a_rssi, 2))[0]
        ack    = swd.read_u32(a_ack)
        nack   = swd.read_u32(a_nack)
        qfree  = swd.read_u32(a_qfree)
        qmax   = swd.read_u32(a_qmax)
        print(f"  ack={ack}  nack={nack}  queue={qfree}/{qmax}")
        print(f"  last RX SNR/RSSI: snr_x4={snr_x4} rssi={rssi}")

        # Cleanup
        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if delta >= 1:
        print(f"  [PASS] rx_count advanced (delta={delta})")
    else:
        print(f"  [FAIL] no RX captured within 30 s — RF path?")
        fails += 1

    if snr_x4 != -128:           # not INT8_MIN sentinel
        print(f"  [PASS] last SNR populated ({snr_x4/4:+.2f} dB)")
    else:
        print(f"  [FAIL] last SNR sentinel — RX hook didn't fire?")
        fails += 1

    if qmax > 0:
        print(f"  [PASS] queue_max populated ({qfree}/{qmax}) — QueueStatus hook fires")
    else:
        print(f"  [WARN] queue_max=0; either no TX traffic seen yet or hook miss")

    print()
    if fails == 0:
        print("==> Phase 4 (T-5) PASS")
        sys.exit(0)
    else:
        print(f"==> Phase 4 FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
