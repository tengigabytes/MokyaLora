"""Close-loop multi-hop test: from peer ebe7 (COM7), craft a RouteDiscovery
reply with non-empty hops/snr arrays and send to mokya. Mokya's cascade
phoneapi_session FR_TAG_PACKET dispatch decodes portnum 70 and writes
phoneapi_node_t.last_route — the same path that real multi-hop traceroute
replies would take.

This is the only way to live-test the packed-array decoder when no real
≥2-hop peer is responding on the mesh.

Usage:
    python scripts/inject_fake_route_reply.py
    # then on mokya side:
    python scripts/read_last_route.py --target 0x538eebe7
"""
import meshtastic.serial_interface
from meshtastic.protobuf import mesh_pb2, portnums_pb2

EBE7_PORT      = 'COM7'
MOKYA_NODE_ID  = '!b15db862'

# Synthetic multi-hop route data — arbitrary intermediate node IDs and
# SNR×4 values. Mokya cache should reflect these exact values after
# decode.
FAKE_HOPS_FWD       = [0x12345678, 0x9abcdef0]    # forward route nodes
FAKE_SNR_FWD_X4     = [40, 36]                     # 10.0 dB, 9.0 dB
FAKE_HOPS_BACK      = [0xdeadbeef]                 # return-path nodes
FAKE_SNR_BACK_X4    = [44]                         # 11.0 dB


def main():
    print(f'connecting to ebe7 on {EBE7_PORT}...')
    iface = meshtastic.serial_interface.SerialInterface(devPath=EBE7_PORT)

    rd = mesh_pb2.RouteDiscovery()
    rd.route.extend(FAKE_HOPS_FWD)
    rd.snr_towards.extend(FAKE_SNR_FWD_X4)
    rd.route_back.extend(FAKE_HOPS_BACK)
    rd.snr_back.extend(FAKE_SNR_BACK_X4)
    payload = rd.SerializeToString()
    print(f'RouteDiscovery payload: {len(payload)} bytes')
    print(f'  route={[hex(x) for x in FAKE_HOPS_FWD]}')
    print(f'  snr_towards={FAKE_SNR_FWD_X4}')
    print(f'  route_back={[hex(x) for x in FAKE_HOPS_BACK]}')
    print(f'  snr_back={FAKE_SNR_BACK_X4}')

    print(f'sending to mokya ({MOKYA_NODE_ID}), portnum=TRACEROUTE_APP=70, '
          f'wantResponse=False (= reply)')
    iface.sendData(
        data=payload,
        destinationId=MOKYA_NODE_ID,
        portNum=portnums_pb2.PortNum.TRACEROUTE_APP,
        wantAck=False,
        wantResponse=False,
    )
    print('sent. Sleeping 8 s for over-the-air delivery + cascade decode.')
    import time
    time.sleep(8)

    iface.close()
    print('\nNow run on mokya side:')
    print('  python scripts/read_last_route.py --target 0x538eebe7')
    print('Expected:')
    print('  hop_count      = 2')
    print('  hops_back_count= 1')
    print(f'  hops_full      = [{hex(FAKE_HOPS_FWD[0])}, {hex(FAKE_HOPS_FWD[1])}, 0, 0]')
    print(f'  hops_back_full = [{hex(FAKE_HOPS_BACK[0])}, 0, 0, 0]')
    print(f'  snr_fwd        = [{FAKE_SNR_FWD_X4[0]}, {FAKE_SNR_FWD_X4[1]}, -128, -128]')
    print(f'  snr_back       = [{FAKE_SNR_BACK_X4[0]}, -128, -128, -128]')


if __name__ == '__main__':
    main()
