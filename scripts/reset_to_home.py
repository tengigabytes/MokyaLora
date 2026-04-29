"""Press BACK repeatedly via SWD inject to drive view_router back to BOOT_HOME."""
import sys
import time

sys.path.insert(0, "scripts")
from mokya_swd import MokyaSwd

KEY_BACK  = 0x12
RING_EVENTS = 32
EVENT_SIZE = 2


def queue_events_swd(swd, base_addrs, events):
    producer = swd.read_u32(base_addrs['inject_producer'])
    ops = []
    for (kb, fb) in events:
        slot = producer % RING_EVENTS
        slot_addr = base_addrs['inject_events'] + slot * EVENT_SIZE
        ops.append((slot_addr, kb & 0xFF))
        ops.append((slot_addr + 1, fb & 0xFF))
        producer += 1
    swd.write_u8_many(ops)
    swd.write_u32(base_addrs['inject_producer'], producer)


def press(kc):
    return [(0x80 | (kc & 0x7F), 0), (0x00 | (kc & 0x7F), 0)]


def buf_addrs(swd):
    base = swd.symbol('g_key_inject_buf')
    return {
        'inject_magic'    : base + 0,
        'inject_producer' : base + 4,
        'inject_consumer' : base + 8,
        'inject_pushed'   : base + 12,
        'inject_rejected' : base + 16,
        'inject_events'   : base + 20,
    }


def get_view_active(swd):
    addr = swd.symbol('s_view_router_active')
    return swd.read_u32(addr)


with MokyaSwd() as swd:
    addrs = buf_addrs(swd)
    for i in range(8):
        v = get_view_active(swd)
        print(f'iter {i}: active={v}')
        if v == 0:
            break
        queue_events_swd(swd, addrs, press(KEY_BACK))
        time.sleep(0.30)
    v = get_view_active(swd)
    print(f'final: active={v}')
