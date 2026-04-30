"""Phase 3 verification — T-4 packet sniffer.

Strategy: have peer COM7 broadcast a unique-token TEXT message;
poll g_t4_total via SWD until it advances; then verify newest
packet's payload contains the token, portnum=1 (TEXT), from=ebe7.
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

VIEW_ID_BOOT_HOME  = 0
VIEW_ID_TOOLS      = 18
VIEW_ID_T4_SNIFFER = 29

EBE7_NODE_NUM = 1401875431  # !538eebe7 from memory project_test_peer_ebe7_usb


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

    # Build a unique token so we can spot it among any concurrent
    # broadcast traffic.
    token = f"T4audit{int(time.time()) & 0xFFFF:04x}"
    print(f"Test token: {token}")

    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! BOOT_HOME unreachable"); sys.exit(1)

        # Pre-snapshot
        a_total = swd.symbol('g_t4_total')
        a_count = swd.symbol('g_t4_count')
        a_newest_from = swd.symbol('g_t4_newest_from')
        a_newest_pn   = swd.symbol('g_t4_newest_pn')
        a_payload     = swd.symbol('g_t4_newest_payload')
        a_payload_len = swd.symbol('g_t4_newest_payload_len')

        # Navigate to T-4 first so render() runs and refreshes the diag
        # globals (they're updated only on render()).
        k(swd, KEYMAP['MOKYA_KEY_FUNC'], 400)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_UP'], 60)
        for _ in range(3): k(swd, KEYMAP['MOKYA_KEY_LEFT'], 60)
        k(swd, KEYMAP['MOKYA_KEY_DOWN'], 80)
        for _ in range(2): k(swd, KEYMAP['MOKYA_KEY_RIGHT'], 80)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        for _ in range(11): k(swd, KEYMAP['MOKYA_KEY_UP'], 40)
        for _ in range(3):  k(swd, KEYMAP['MOKYA_KEY_DOWN'], 60)
        k(swd, KEYMAP['MOKYA_KEY_OK'], 400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_T4_SNIFFER:
            print(f"!! expected T4_SNIFFER (29), got {v}"); sys.exit(1)
        time.sleep(0.5)

        pre_total = swd.read_u32(a_total)
        print(f"  pre-test g_t4_total = {pre_total}, g_t4_count = {swd.read_u32(a_count)}")

        # Trigger from peer COM7 — broadcast on primary (no --dest).
        print(f"  sending from peer COM7: '{token}'")
        sub = subprocess.run(
            ['python', '-m', 'meshtastic', '--port', 'COM7', '--sendtext', token],
            capture_output=True, text=True, timeout=30)
        if 'Sending' not in sub.stdout and 'sent' not in sub.stdout.lower():
            print(f"  CLI may have failed; stdout tail:")
            print(sub.stdout[-200:])

        # Poll g_t4_total — expect it to advance once the LoRa packet
        # reaches MokyaLora's cascade.
        deadline = time.time() + 30.0
        post_total = pre_total
        while time.time() < deadline:
            post_total = swd.read_u32(a_total)
            if post_total > pre_total:
                break
            time.sleep(0.5)
        print(f"  post-test g_t4_total = {post_total} (delta = {post_total - pre_total})")

        if post_total == pre_total:
            print("  !! no packets observed within 30 s — RF path? PKI?")
            print("     Skipping payload verification.")
            fails += 1
        else:
            # Read all 16 newest entries' payloads and search for the
            # token, since concurrent NodeInfo broadcasts can race
            # the test message into the ring.
            found_idx = -1
            found_from = 0
            found_pn = 0
            for retry in range(8):
                # Cycle through visible packets so the view re-renders
                # each as the newest in the diag globals — but easier:
                # just walk the ring directly via SWD-call-equivalent.
                # For v1, we just trust newest = recent, look at it.
                k(swd, KEYMAP['MOKYA_KEY_UP'], 100)   # cursor up — render fires
                time.sleep(0.3)
                pl_len = swd.read_mem(a_payload_len, 1)[0]
                pl = bytes(swd.read_mem(a_payload, 16))
                f = swd.read_u32(a_newest_from)
                pn = swd.read_u32(a_newest_pn)
                # Decode utf8 if possible
                try:
                    txt = pl[:pl_len].decode('utf-8')
                except Exception:
                    txt = repr(pl[:pl_len])
                print(f"  newest ({retry}): from=!{f:08x} pn={pn} len={pl_len} payload={txt!r}")
                if token.encode('utf-8') in pl:
                    found_idx = retry
                    found_from = f
                    found_pn   = pn
                    break

            if found_idx < 0:
                print(f"  [FAIL] token {token!r} not found in any newest snapshot")
                fails += 1
            else:
                print(f"  [PASS] token found in newest entry (try #{found_idx})")
                if found_pn == 1:
                    print(f"  [PASS] portnum = 1 (TEXT_MESSAGE_APP)")
                else:
                    print(f"  [FAIL] portnum = {found_pn} (expected 1)")
                    fails += 1
                if found_from == EBE7_NODE_NUM:
                    print(f"  [PASS] from = !{EBE7_NODE_NUM:08x} (ebe7)")
                else:
                    print(f"  [WARN] from = !{found_from:08x} (expected !{EBE7_NODE_NUM:08x}); "
                          f"may be relay")

        k(swd, KEYMAP['MOKYA_KEY_BACK'], 400)
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    print()
    if fails == 0:
        print("==> Phase 3 (T-4) PASS")
        sys.exit(0)
    else:
        print(f"==> Phase 3 FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
