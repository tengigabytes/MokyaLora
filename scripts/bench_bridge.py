"""Benchmark bridge timing — fine-grained phase breakdown."""
import time
import serial
import random
import sys

from meshtastic.protobuf import mesh_pb2

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM16"
START1, START2 = 0x94, 0xC3

t0 = time.perf_counter()

ser = serial.Serial(PORT, 115200, timeout=0.05)  # 50ms read timeout
t_open = time.perf_counter()
print(f"[{t_open-t0:.3f}s] Port opened ({PORT})")

# Send wake-up (same as CLI, but NO Win11 sleep)
ser.write(bytes([START2] * 32))
ser.flush()
t_wake = time.perf_counter()
print(f"[{t_wake-t0:.3f}s] Wake-up sent")

time.sleep(0.1)

# Send want_config_id
config_id = random.randint(0, 0xFFFFFFFF)
to_radio = mesh_pb2.ToRadio()
to_radio.want_config_id = config_id
payload = to_radio.SerializeToString()
header = bytes([START1, START2, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF])
ser.write(header + payload)
ser.flush()
t_send = time.perf_counter()
print(f"[{t_send-t0:.3f}s] want_config_id sent")

# Read with 50ms timeout, print every chunk
total = 0
first_byte_t = None
empty_count = 0
while empty_count < 20:  # 20 consecutive empties = 1s silence = done
    data = ser.read(4096)
    if data:
        now = time.perf_counter()
        if first_byte_t is None:
            first_byte_t = now
            print(f"[{now-t0:.3f}s] ** First data: {len(data)} bytes (latency {now-t_send:.3f}s)")
        else:
            print(f"[{now-t0:.3f}s]    Chunk: {len(data)} bytes (total {total+len(data)})")
        total += len(data)
        empty_count = 0
    else:
        empty_count += 1

t_done = time.perf_counter()
print(f"\n[{t_done-t0:.3f}s] Done: {total} bytes total")
if first_byte_t:
    xfer = t_done - first_byte_t - empty_count * 0.05  # subtract final timeout waits
    print(f"  First-byte latency: {first_byte_t - t_send:.3f}s")
    print(f"  Data transfer: {xfer:.3f}s")
    print(f"  Throughput: {total / max(0.001, xfer):.0f} B/s")
ser.close()
