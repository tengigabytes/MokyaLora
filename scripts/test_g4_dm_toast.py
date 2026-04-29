"""G-4 DM toast close-loop test.

1. Reset mokya view to BOOT_HOME (so user_already_on_chat suppression
   does NOT fire — we want to see the toast).
2. Drain RTT up-buffer to ignore prior history.
3. From peer ebe7 (COM7), `sendtext` a known string DM to mokya.
4. Wait for the cascade RX + dm_store ingest + global_alert tick.
5. Drain RTT, look for `galert,dm_toast peer=...,unread=...` trace.

Run from project root:
    python scripts/test_g4_dm_toast.py
"""
import sys, time
sys.path.insert(0, "scripts")
from mokya_swd import MokyaSwd

KEY_BACK = 0x12

EBE7_PORT     = 'COM7'
MOKYA_NODE_ID = '!b15db862'
TEST_TEXT     = "g4-toast-test " + str(int(time.time()))


def buf_addrs(swd):
    base = swd.symbol('g_key_inject_buf')
    return {'inject_producer': base + 4, 'inject_consumer': base + 8,
            'inject_events': base + 20}


def queue_press(swd, addrs, kc):
    producer = swd.read_u32(addrs['inject_producer'])
    for kb in (0x80 | kc, 0x00 | kc):
        slot = producer % 32
        slot_addr = addrs['inject_events'] + slot * 2
        swd.write_u8_many([(slot_addr, kb), (slot_addr + 1, 0)])
        producer += 1
    swd.write_u32(addrs['inject_producer'], producer)


def view_active(swd):
    return swd.read_u32(swd.symbol('s_view_router_active'))


def rtt_locate_up(swd):
    cb = swd.symbol('_SEGGER_RTT')
    d_off = cb + 24
    d = swd._jl.memory_read32(d_off, 6)
    _, buf_ptr, size, _, _, _ = d
    return {'buf': buf_ptr, 'size': size,
            'wroff_addr': d_off + 12, 'rdoff_addr': d_off + 16}


def rtt_drain_up(swd, ctx, sink):
    wr = swd._jl.memory_read32(ctx['wroff_addr'], 1)[0]
    rd = swd._jl.memory_read32(ctx['rdoff_addr'], 1)[0]
    if wr == rd: return
    if wr > rd:
        sink.extend(swd.read_mem(ctx['buf'] + rd, wr - rd))
    else:
        sink.extend(swd.read_mem(ctx['buf'] + rd, ctx['size'] - rd))
        if wr > 0:
            sink.extend(swd.read_mem(ctx['buf'], wr))
    swd._jl.memory_write32(ctx['rdoff_addr'], [wr])


def main():
    sink = bytearray()

    print('[1] connect mokya SWD, drain inject ring + RTT')
    with MokyaSwd() as swd:
        addrs = buf_addrs(swd)
        rtt = rtt_locate_up(swd)
        # Drain pending inject events
        swd.write_u32(addrs['inject_consumer'],
                      swd.read_u32(addrs['inject_producer']))
        # Discard RTT backlog so we only see post-test events
        rtt_drain_up(swd, rtt, bytearray())
        time.sleep(0.05)
        print(f'    init view={view_active(swd)}')

        # Walk back to BOOT_HOME so toast isn't suppressed
        for _ in range(8):
            v = view_active(swd)
            if v == 0:
                break
            queue_press(swd, addrs, KEY_BACK)
            time.sleep(0.30)
        print(f'    after BACK*N view={view_active(swd)}')

        # Disconnect SWD before opening meshtastic to avoid contention
        # (pylink uses the only J-Link channel; meshtastic uses a
        # different USB CDC, so this should be fine, but close anyway).

    print('\n[2a] open mokya CDC (COM16) to force cascade FORWARD mode')
    import meshtastic.serial_interface
    mokya_iface = meshtastic.serial_interface.SerialInterface(devPath='COM16')
    print('    mokya CDC connected; cascade should now be in FORWARD')
    time.sleep(2)

    print(f'\n[2b] open ebe7 ({EBE7_PORT}) and sendtext to mokya')
    print(f'    text="{TEST_TEXT}"')
    ebe7_iface = meshtastic.serial_interface.SerialInterface(devPath=EBE7_PORT)
    # First try broadcast (channel-key, no PKI) so we isolate any PKI
     # cache-mismatch from the cascade RX path. If broadcast works but
     # DM doesn't, the issue is encryption.
    print('    sending BROADCAST first to verify cascade RX path...')
    ebe7_iface.sendText(TEST_TEXT, wantAck=False)  # default destination = broadcast
    time.sleep(6)
    print('    now sending DM to mokya')
    ebe7_iface.sendText(TEST_TEXT + " (DM)",
                        destinationId=MOKYA_NODE_ID,
                        wantAck=True)

    print('\n[3] re-open mokya SWD + continuously drain RTT for 15 s')
    with MokyaSwd() as swd:
        rtt2 = rtt_locate_up(swd)
        for _ in range(150):
            time.sleep(0.1)
            rtt_drain_up(swd, rtt2, sink)
    ebe7_iface.close()
    mokya_iface.close()

    log_path = '/tmp/rtt_g4_toast.log'
    with open(log_path, 'w', encoding='utf-8', errors='replace') as f:
        f.write(sink.decode('utf-8', errors='replace'))
    print(f'\n[4] saved RTT log → {log_path} ({len(sink)} B)')

    print('\n=== relevant lines ===')
    text = sink.decode('utf-8', errors='replace')
    keys = ('galert,', 'phapi,rx_packet', 'dm_store,ingest', 'phapi,rx_text',
            'phapi,set_my_info')
    found_galert = False
    for line in text.splitlines():
        if any(k in line for k in keys):
            print(line)
            if 'galert,' in line:
                found_galert = True

    print()
    if found_galert:
        print('PASS: galert toast TRACE present')
    else:
        print('FAIL: no galert TRACE — inspect log to see if RX path fired')


if __name__ == '__main__':
    main()
