"""Live-RF DM persist round-trip test.

End-to-end: peer COM7 (TNGBpicoC-ebe7, !538EEBE7) sends a real DM via
LoRa to MokyaLora; we wait for cascade reception, force flush, watchdog
reset, then verify the DM was reloaded byte-perfect from disk.

This is the strongest validation — exercises the full pipeline:
  RF link → SX1262 → Core 0 LoRa stack → cascade FromRadio →
  Core 1 phoneapi_session decoder → dm_store_ingest_inbound →
  dirty bit → flush_now → /.dm_<peer>.bin →
  watchdog reset → boot → c1_storage_init → load_all →
  dm_store_restore_peer → diag global

Pass criterion: post-reset diag matches the actual text we sent.
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from test_c1_dm_persist import (  # type: ignore
    format_fs, flush_now, reset_and_wait, read_diag, MOKYA_NODE_NUM)

PEER_PORT = "COM7"
PEER_NODE_ID = 0x538EEBE7   # TNGBpicoC-ebe7
TEST_TEXT = f"liverf_persist_{int(time.time()) & 0xFFFF}"


def main():
    print(f"[start] peer COM7 → live LoRa DM → reset → verify reload")
    print(f"  test text = {TEST_TEXT!r}")

    # Wipe + reset to deterministic empty FS / dm_store.
    with MokyaSwd() as swd:
        format_fs(swd)
        reset_and_wait(swd)
    print(f"  [format+reset] clean state")

    # Send DM from peer COM7 via meshtastic Python lib.
    print(f"[send] DM from {PEER_PORT} → !{MOKYA_NODE_NUM:08X}")
    from meshtastic.serial_interface import SerialInterface
    iface = SerialInterface(devPath=PEER_PORT)
    try:
        time.sleep(2.0)   # let connection settle
        iface.sendText(TEST_TEXT, destinationId=f"!{MOKYA_NODE_NUM:08x}")
        # Wait for the packet to traverse LoRa air + reach C1.
        time.sleep(8.0)
    finally:
        iface.close()

    # Verify Core 1 saw it via cascade decoder counter.
    with MokyaSwd() as swd:
        pn_text = swd.read_u32(swd.symbol("g_dbg_pn_text"))
    print(f"  pn_text after RF send = {pn_text}")
    if pn_text == 0:
        print(f"  [FAIL] cascade decoder didn't receive any text packet — "
              "RF link or PKI sync issue")
        sys.exit(1)
    print(f"  [PASS] cascade RX confirmed")

    # Flush + reset.
    with MokyaSwd() as swd:
        flush_now(swd)
        saves = swd.read_u32(swd.symbol("g_dm_persist_saves"))
    print(f"  [flush] saves = {saves}")
    if saves == 0:
        print(f"  [FAIL] flush_now ran but saves=0 — no peer was dirty")
        sys.exit(1)
    print(f"  [PASS] flushed peer to disk")

    with MokyaSwd() as swd:
        reset_and_wait(swd)

    # Verify reload + content.
    with MokyaSwd() as swd:
        loads = swd.read_u32(swd.symbol("g_dm_persist_loads"))
        peer, count, outb, tlen, text = read_diag(swd)
    print(f"  post-reset: loads={loads}  peer=!{peer:08X}  count={count}  "
          f"text_len={tlen}  text={text.decode('utf-8', 'replace')!r}")

    fails = 0
    if loads < 1:
        print(f"  [FAIL] loads={loads} (expected ≥ 1)"); fails += 1
    if peer != PEER_NODE_ID:
        print(f"  [FAIL] peer expected !{PEER_NODE_ID:08X}, got !{peer:08X}"); fails += 1
    if outb != 0:
        print(f"  [FAIL] expected inbound (outb=0), got outb={outb}"); fails += 1
    if tlen != len(TEST_TEXT):
        print(f"  [FAIL] text_len expected {len(TEST_TEXT)}, got {tlen}"); fails += 1
    if text.decode("utf-8", "replace") != TEST_TEXT:
        print(f"  [FAIL] text mismatch: {text!r} != {TEST_TEXT!r}"); fails += 1

    if fails == 0:
        print(f"\n==> Live-RF DM persist PASS — {TEST_TEXT!r} byte-perfect")
        sys.exit(0)
    else:
        print(f"\n==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
