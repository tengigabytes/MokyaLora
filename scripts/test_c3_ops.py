"""A1 — fire traceroute / position from C-3 ops menu via SWD ring inject,
capture RTT up-channel trace events in the same pylink session.

Usage:
    python scripts/_a1_traceroute.py traceroute   # cursor DOWN x4 + OK
    python scripts/_a1_traceroute.py position     # cursor DOWN x5 + OK
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, "scripts")
from mokya_swd import MokyaSwd

# Mirror inject_keys.py constants.
RING_EVENTS  = 32
EVENT_SIZE   = 2

# Keycodes (from firmware/mie/include/mie/keycode.h).
KEY_FUNC  = 0x06
KEY_BACK  = 0x12
KEY_UP    = 0x1F
KEY_DOWN  = 0x20
KEY_LEFT  = 0x21
KEY_RIGHT = 0x22
KEY_OK    = 0x23

# View IDs from firmware/core1/src/ui/view_router.h.
V_BOOT_HOME       = 0
V_LAUNCHER        = 1
V_MESSAGES        = 2
V_MESSAGES_CHAT   = 3
V_MESSAGE_DETAIL  = 4   # A3 — added between MESSAGES_CHAT and NODES
V_NODES           = 5
V_NODE_DETAIL     = 6
V_NODE_OPS        = 7
V_MY_NODE         = 8

# Launcher 3x3 grid (row, col) — (firmware/core1/src/ui/launcher_view.c:47).
#   (0,0) Msg     (0,1) Chan    (0,2) Nodes
#   (1,0) Map     (1,1) Tele    (1,2) Tools
#   (2,0) Set     (2,1) Me      (2,2) Power
LAUNCH_NODES_DR = 0
LAUNCH_NODES_DC = 2  # from default (0,0)


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


def navigate_to_nodes_via_launcher(swd, addrs):
    """L-1 launcher: FUNC opens it as modal; arrow keys move; OK picks."""
    # FUNC opens launcher.
    queue_events_swd(swd, addrs, press(KEY_FUNC))
    time.sleep(0.30)
    v = get_view_active(swd)
    if v != V_LAUNCHER:
        raise RuntimeError(f"FUNC did not open launcher (active={v})")
    # Move cursor to (0, 2) = Nodes.
    for _ in range(LAUNCH_NODES_DC):
        queue_events_swd(swd, addrs, press(KEY_RIGHT))
        time.sleep(0.10)
    # OK commits launcher modal → launcher_done_cb → navigate(NODES).
    queue_events_swd(swd, addrs, press(KEY_OK))
    time.sleep(0.30)
    v = get_view_active(swd)
    if v != V_NODES:
        raise RuntimeError(f"launcher OK did not navigate to NODES (active={v})")


# ── RTT up-channel reader (channel 0) ────────────────────────────────────

def rtt_locate_up(swd):
    cb = swd.symbol('_SEGGER_RTT')
    # Per SEGGER_RTT.h:
    #   id[16] | max_up u32 | max_down u32 | (UpDesc[max_up]) | (DownDesc[max_down])
    # We want UpDesc[0]: name_ptr, buf_ptr, size, wroff, rdoff, flags
    d_off = cb + 24  # past id + max_up + max_down
    d = swd._jl.memory_read32(d_off, 6)
    name_ptr, buf_ptr, size, wroff, rdoff, flags = d
    if size == 0 or buf_ptr == 0:
        raise RuntimeError("RTT up-channel 0 not configured")
    return {'buf': buf_ptr, 'size': size,
            'wroff_addr': d_off + 12, 'rdoff_addr': d_off + 16}


def rtt_drain_up(swd, ctx, sink_bytes):
    """Read everything up-channel has produced since last drain; advance rd."""
    wr = swd._jl.memory_read32(ctx['wroff_addr'], 1)[0]
    rd = swd._jl.memory_read32(ctx['rdoff_addr'], 1)[0]
    if wr == rd:
        return 0
    if wr > rd:
        chunk = swd.read_mem(ctx['buf'] + rd, wr - rd)
        sink_bytes.extend(chunk)
    else:
        first = swd.read_mem(ctx['buf'] + rd, ctx['size'] - rd)
        sink_bytes.extend(first)
        if wr > 0:
            sink_bytes.extend(swd.read_mem(ctx['buf'], wr))
    swd._jl.memory_write32(ctx['rdoff_addr'], [wr])
    return (wr - rd) % ctx['size']


# ── Main ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('op', choices=['traceroute', 'position', 'favorite',
                                   'ignore', 'navonly'])
    ap.add_argument('--peer', default=None,
                    help='hex node_num to verify exists in cache (optional)')
    ap.add_argument('--out', default='/tmp/rtt_a1.log')
    args = ap.parse_args()

    sink = bytearray()

    with MokyaSwd() as swd:
        addrs = buf_addrs(swd)
        magic = swd.read_u32(addrs['inject_magic'])
        if magic != 0x4B45494A:
            raise RuntimeError(f"key_inject magic mismatch 0x{magic:08X}")
        rtt = rtt_locate_up(swd)
        # Drain whatever was sitting in the ring before we start.
        rtt_drain_up(swd, rtt, bytearray())
        print(f"[init] view={get_view_active(swd)} "
              f"rtt buf=0x{rtt['buf']:08x} size={rtt['size']}", flush=True)

        # Step 1: launcher → Nodes.
        print('[step] FUNC + RIGHT*2 + OK → NODES', flush=True)
        navigate_to_nodes_via_launcher(swd, addrs)
        rtt_drain_up(swd, rtt, sink)

        # Step 2: OK enters NODE_DETAIL (also stashes active_num).
        print('[step] OK → NODE_DETAIL', flush=True)
        queue_events_swd(swd, addrs, press(KEY_OK))
        time.sleep(0.30)
        v = get_view_active(swd)
        rtt_drain_up(swd, rtt, sink)
        if v != V_NODE_DETAIL:
            raise RuntimeError(f"NODES OK did not navigate to NODE_DETAIL (active={v}); "
                               "likely no nodes in cache")

        # Step 3: OK enters NODE_OPS.
        print('[step] OK → NODE_OPS', flush=True)
        queue_events_swd(swd, addrs, press(KEY_OK))
        time.sleep(0.30)
        v = get_view_active(swd)
        rtt_drain_up(swd, rtt, sink)
        if v != V_NODE_OPS:
            raise RuntimeError(f"DETAIL OK did not navigate to NODE_OPS (active={v})")

        # Step 4: DOWN to target row.
        # OP_DM=0, OP_ALIAS=1, OP_FAVORITE=2, OP_IGNORE=3,
        # OP_TRACEROUTE=4, OP_REQUEST_POS=5, OP_REMOTE_ADMIN=6
        if args.op == 'favorite':
            target_idx = 2
            # P0-3: tx_app uses label=admin_set_fav or admin_clr_fav
            # depending on cached toggle direction.
            label_match = b'_fav'
        elif args.op == 'ignore':
            target_idx = 3
            label_match = b'_ign'
        elif args.op == 'traceroute':
            target_idx = 4
            label_match = b'label=traceroute'
        elif args.op == 'position':
            target_idx = 5
            label_match = b'label=pos_req'
        else:
            target_idx = -1
            label_match = None

        if target_idx >= 0:
            # NODE_OPS view is LRU-cached — its cursor persists across
            # navigations. Reset to row 0 with UP×N (clamps to 0).
            for _ in range(7):
                queue_events_swd(swd, addrs, press(KEY_UP))
                time.sleep(0.05)
            time.sleep(0.10)
            for _ in range(target_idx):
                queue_events_swd(swd, addrs, press(KEY_DOWN))
                time.sleep(0.10)
            rtt_drain_up(swd, rtt, sink)

            # Step 5: OK fires the encoder.
            print(f'[step] OK → fire {args.op} (cursor row {target_idx})',
                  flush=True)
            queue_events_swd(swd, addrs, press(KEY_OK))
            # Drain aggressively for 5 s — cover both tx_app + any peer reply.
            for _ in range(50):
                time.sleep(0.1)
                rtt_drain_up(swd, rtt, sink)
        else:
            for _ in range(10):
                time.sleep(0.1)
                rtt_drain_up(swd, rtt, sink)

    Path(args.out).write_bytes(bytes(sink))
    text = sink.decode('utf-8', errors='replace')
    print('\n=== RTT capture (last 60 lines) ===')
    for line in text.splitlines()[-60:]:
        print(line)
    if label_match:
        hits = [l for l in text.splitlines()
                if label_match.decode() in l]
        print(f'\n=== tx_app matches for {args.op}: {len(hits)} ===')
        for h in hits:
            print(h)
        rx = [l for l in text.splitlines() if 'rx_packet' in l]
        print(f'\n=== rx_packet lines: {len(rx)} ===')
        for h in rx:
            print(h)


if __name__ == '__main__':
    main()
