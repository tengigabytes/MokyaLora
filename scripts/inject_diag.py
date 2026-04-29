"""A1 diag — show key_inject ring state + g_key_inject_mode + view active."""
import sys
sys.path.insert(0, "scripts")
from mokya_swd import MokyaSwd

with MokyaSwd() as swd:
    base = swd.symbol('g_key_inject_buf')
    magic    = swd.read_u32(base + 0)
    producer = swd.read_u32(base + 4)
    consumer = swd.read_u32(base + 8)
    pushed   = swd.read_u32(base + 12)
    rejected = swd.read_u32(base + 16)
    print(f"key_inject_buf @ 0x{base:08x}")
    print(f"  magic     = 0x{magic:08X} (KEYJ)")
    print(f"  producer  = {producer}")
    print(f"  consumer  = {consumer}")
    print(f"  pushed    = {pushed}")
    print(f"  rejected  = {rejected}")

    try:
        mode_addr = swd.symbol('g_key_inject_mode')
        mode = swd._jl.memory_read8(mode_addr, 1)[0]
        print(f"  mode      = {mode} ({'SWD' if mode == 0 else 'RTT'})")
    except Exception as e:
        print(f"  mode      = (lookup failed: {e})")

    va = swd.read_u32(swd.symbol('s_view_router_active'))
    print(f"\nview_router s_active = {va}")

    for sym in ('g_key_event_pushed', 'g_key_event_dropped',
                'g_key_event_rejected', 'g_key_event_log_idx'):
        try:
            v = swd.read_u32(swd.symbol(sym))
            print(f"  {sym} = {v}")
        except Exception as e:
            print(f"  {sym} = (lookup failed: {e})")

    try:
        s_modal = swd.read_u32(swd.symbol('s_modal_caller'))
        print(f"  s_modal_caller = 0x{s_modal:08X} "
              f"({'no modal' if s_modal == 0xFFFFFFFF else f'caller={s_modal}'})")
    except Exception as e:
        print(f"  s_modal_caller (lookup failed: {e})")

    # Dump full key_event_log
    try:
        log_base = swd.symbol('g_key_event_log')
        idx = swd.read_u32(swd.symbol('g_key_event_log_idx'))
        bytes_buf = swd.read_mem(log_base, 64)
        print(f"  key_event_log raw (idx={idx}, base=0x{log_base:08x}):")
        for i in range(0, 64, 16):
            print(f"    {i:02d}: " + ' '.join(f'{b:02x}' for b in bytes_buf[i:i+16]))
    except Exception as e:
        print(f"  key_event_log (failed: {e})")

    # Try reading view_observer queue depth (FreeRTOS QueueHandle_t)
    try:
        s_q = swd.read_u32(swd.symbol('s_view_queue'))
        s_main = swd.read_u32(swd.symbol('s_queue'))
        print(f"  s_view_queue = 0x{s_q:08x}")
        print(f"  s_queue (main) = 0x{s_main:08x}")
        # FreeRTOS Queue_t starts with int8_t *pcHead; UBaseType_t uxMessagesWaiting at offset ~36 typically
        # but layout varies. Skip detailed inspection.
    except Exception as e:
        print(f"  queue lookup failed: {e}")
