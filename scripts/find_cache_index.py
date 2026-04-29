"""Walk phoneapi_cache.nodes[0..31] via SWD and report which cursor index
maps to a given node_num.

Used by P1-#7 multi-hop traceroute test to locate AD-9527 (0xfca06ed6) in
the cache without manually probing.

Caveat: phoneapi_cache lives in PSRAM (.psram_bss); SWD reads bypass the
RP2350 cache (project_psram_swd_cache_coherence.md). For the
infrequently-written `num` field this should be fine, but if you see
0xffffffff or wildly wrong values, retry after a `git status`-style noop
that lets cache lines settle.
"""
import argparse
import sys

sys.path.insert(0, "scripts")
from mokya_swd import MokyaSwd

NODES_BASE = 0x11707a3c   # &s_cache.nodes[0] from objdump
NODES_CAP  = 32
NODE_SIZE  = 188          # sizeof(phoneapi_node_t)
OFF_IN_USE = 0
OFF_NUM    = 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--target', type=lambda s: int(s, 0), default=None,
                    help='hex node_num to find (e.g. 0xfca06ed6)')
    args = ap.parse_args()

    with MokyaSwd() as swd:
        cursor = 0
        target_cursor = None
        for i in range(NODES_CAP):
            slot = NODES_BASE + i * NODE_SIZE
            in_use = swd.read_u32(slot + OFF_IN_USE) & 0xFF
            num    = swd.read_u32(slot + OFF_NUM)
            if in_use:
                marker = ''
                if args.target is not None and num == args.target:
                    marker = ' ← TARGET'
                    target_cursor = cursor
                print(f'  cursor={cursor:2d}  slot={i:2d}  num=0x{num:08x}{marker}')
                cursor += 1
        print(f'\ntotal in_use = {cursor}')
        if args.target is not None:
            if target_cursor is None:
                print(f'target 0x{args.target:08x} NOT FOUND in cache')
                sys.exit(1)
            print(f'target 0x{args.target:08x} → cursor={target_cursor}')


if __name__ == '__main__':
    main()
