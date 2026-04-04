import serial, time, sys
delay = float(sys.argv[1]) if len(sys.argv) > 1 else 7.0
time.sleep(delay)
port = serial.Serial('COM4', 115200, timeout=2)
time.sleep(0.3)
port.write(b'sram\r\n')
time.sleep(3)
port.close()
print("sram command sent")
