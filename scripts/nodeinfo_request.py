#!/usr/bin/env python3
"""Request full NodeInfo (with pubkey) from a peer node — replicates the
Meshtastic mobile app's "tap to add contact" flow. Sends a NodeInfo-app
packet with want_response=true; the peer replies with its full User
record including public_key, which lands in our local NodeDB.

Usage:
    python nodeinfo_request.py <port> <dest-id>
        e.g. python nodeinfo_request.py COM16 '!538eebe7'
"""

import sys
import time

import meshtastic.serial_interface
from meshtastic.protobuf import portnums_pb2


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    port, dest = sys.argv[1], sys.argv[2]
    iface = meshtastic.serial_interface.SerialInterface(port)
    print(f"connected to {port}; requesting NodeInfo from {dest}")
    iface.sendData(
        b"",
        destinationId=dest,
        portNum=portnums_pb2.NODEINFO_APP,
        wantResponse=True,
    )
    # Hold the connection open briefly so the reply has time to land.
    time.sleep(8)
    iface.close()
    print("done")


if __name__ == "__main__":
    main()
