"""Read g_key_event_log (16-byte ring) + counters via SWD to see what
events the firmware actually saw."""
import sys, time
sys.path.insert(0, "scripts")
from mokya_swd import MokyaSwd

with MokyaSwd() as swd:
    log_addr = 0x200693c8
    idx_addr = 0x200693d8
    pushed_addr = 0x200693dc
    dropped_addr = 0x200693c4
    rejected_addr = 0x200693e0

    log = swd.read_mem(log_addr, 16)
    idx = swd.read_u32(idx_addr)
    pushed = swd.read_u32(pushed_addr)
    dropped = swd.read_u32(dropped_addr)
    rejected = swd.read_u32(rejected_addr)

    # ring: oldest at slot (idx - 16) % 16, newest at slot (idx - 1) % 16
    print(f'idx={idx} pushed={pushed} dropped={dropped} rejected={rejected}')
    print('ring (oldest → newest):')
    for i in range(16):
        slot = (idx + i) % 16
        b = log[slot]
        pressed = bool(b & 0x80)
        kc = b & 0x7F
        names = {0x06:'FUNC',0x12:'BACK',0x1F:'UP',0x20:'DOWN',0x21:'LEFT',0x22:'RIGHT',0x23:'OK'}
        print(f'  [{slot:2d}] 0x{b:02x} {"↓" if pressed else "↑"} {names.get(kc, hex(kc))}')
