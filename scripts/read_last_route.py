"""Read phoneapi_cache.nodes[N].last_route via SWD for a target node_num."""
import argparse, sys, time
sys.path.insert(0, "scripts")
from mokya_swd import MokyaSwd

NODES_BASE = 0x11707a3c
NODE_SIZE  = 188
NODES_CAP  = 32
OFF_IN_USE      = 0
OFF_NUM         = 4
OFF_LAST_ROUTE  = 0x78  # 120
OFF_LAST_POS    = 0xa8  # 168


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--target', type=lambda s: int(s, 0), required=True)
    args = ap.parse_args()

    with MokyaSwd() as swd:
        slot_idx = None
        for i in range(NODES_CAP):
            slot = NODES_BASE + i * NODE_SIZE
            if (swd.read_u32(slot + OFF_IN_USE) & 0xFF) and \
               swd.read_u32(slot + OFF_NUM) == args.target:
                slot_idx = i
                break
        if slot_idx is None:
            print(f'target 0x{args.target:08x} not in cache')
            return
        slot_base = NODES_BASE + slot_idx * NODE_SIZE
        lr = swd.read_mem(slot_base + OFF_LAST_ROUTE, 48)
        # phoneapi_last_route_t layout (48 B):
        #   uint8 hop_count           +0
        #   uint8 hops_back_count     +1
        #   uint16 _pad               +2
        #   uint32 hops_full[4]       +4..+19
        #   uint32 hops_back_full[4]  +20..+35
        #   int8   snr_fwd[4]         +36..+39
        #   int8   snr_back[4]        +40..+43
        #   uint32 epoch              +44
        import struct
        hop_count, hops_back_count = lr[0], lr[1]
        hops_full      = struct.unpack_from('<4I', lr, 4)
        hops_back_full = struct.unpack_from('<4I', lr, 20)
        snr_fwd        = struct.unpack_from('<4b', lr, 36)
        snr_back       = struct.unpack_from('<4b', lr, 40)
        epoch          = struct.unpack_from('<I', lr, 44)[0]

        print(f'slot {slot_idx}: 0x{args.target:08x} last_route:')
        print(f'  hop_count      = {hop_count}')
        print(f'  hops_back_count= {hops_back_count}')
        print(f'  hops_full      = {[f"0x{h:08x}" for h in hops_full]}')
        print(f'  hops_back_full = {[f"0x{h:08x}" for h in hops_back_full]}')
        print(f'  snr_fwd        = {list(snr_fwd)}  (×4 dB)')
        print(f'  snr_back       = {list(snr_back)}')
        print(f'  epoch          = {epoch}')

        lp = swd.read_mem(slot_base + OFF_LAST_POS, 16)
        lat, lon, alt, p_epoch = struct.unpack('<iiiI', lp)
        print(f'\nlast_position:')
        print(f'  lat_e7={lat}  lon_e7={lon}  alt={alt}m  epoch={p_epoch}')


if __name__ == '__main__':
    main()
