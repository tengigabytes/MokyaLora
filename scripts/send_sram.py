import serial
import sys
import time
from serial.tools import list_ports


def find_mokya_port():
    for p in list_ports.comports():
        if p.vid == 0x2E8A:
            return p.device
    return None


delay = float(sys.argv[1]) if len(sys.argv) > 1 else 7.0
time.sleep(delay)

port_name = find_mokya_port()
if port_name is None:
    print("ERROR: No RP2350B serial port found (VID 0x2E8A).")
    sys.exit(1)

port = serial.Serial(port_name, 115200, timeout=2)
time.sleep(0.3)
port.write(b'sram\r\n')
time.sleep(3)
port.close()
print(f"sram command sent on {port_name}")
