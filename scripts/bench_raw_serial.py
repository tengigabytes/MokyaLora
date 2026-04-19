"""Raw serial throughput benchmark — bypasses Meshtastic CLI entirely.

Measures time from sending want_config_id to receiving CONFIG_COMPLETE_ID.
This isolates IPC bridge latency from Python CLI overhead.
"""
import serial
import struct
import time
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM16"
BAUD = 115200

# Meshtastic serial framing constants
START1 = 0x94
START2 = 0xC3
HEADER_LEN = 4  # start1 + start2 + MSB + LSB

def make_want_config(config_id=0):
    """Build a minimal ToRadio { want_config_id: config_id } protobuf."""
    # ToRadio field 3 (want_config_id) = varint
    # protobuf: field 3, wire type 0 = tag 0x18, value = config_id
    payload = bytes([0x18, config_id & 0x7F])
    # Meshtastic serial frame: START1 START2 LEN_MSB LEN_LSB PAYLOAD
    length = len(payload)
    frame = bytes([START1, START2, (length >> 8) & 0xFF, length & 0xFF]) + payload
    return frame

def read_frames(ser, timeout=5.0):
    """Read Meshtastic serial frames until timeout. Returns (frame_count, total_bytes, elapsed)."""
    buf = bytearray()
    frames = 0
    total_payload = 0
    start = time.perf_counter()
    deadline = start + timeout
    first_frame_time = None

    while time.perf_counter() < deadline:
        waiting = ser.in_waiting
        if waiting > 0:
            data = ser.read(waiting)
            buf.extend(data)
        else:
            time.sleep(0.001)
            continue

        # Parse complete frames from buffer
        while len(buf) >= HEADER_LEN:
            # Scan for START1 START2
            idx = -1
            for i in range(len(buf) - 1):
                if buf[i] == START1 and buf[i + 1] == START2:
                    idx = i
                    break
            if idx < 0:
                buf = buf[-1:]  # keep last byte in case it's START1
                break
            if idx > 0:
                buf = buf[idx:]  # discard bytes before start marker

            if len(buf) < HEADER_LEN:
                break
            payload_len = (buf[2] << 8) | buf[3]
            total_frame_len = HEADER_LEN + payload_len
            if len(buf) < total_frame_len:
                break

            # Complete frame
            payload = bytes(buf[HEADER_LEN:total_frame_len])
            buf = buf[total_frame_len:]
            frames += 1
            total_payload += payload_len
            if first_frame_time is None:
                first_frame_time = time.perf_counter()

        # Stop after a gap of no data (config exchange done)
        if frames > 5 and ser.in_waiting == 0:
            time.sleep(0.05)  # small gap check
            if ser.in_waiting == 0:
                break

    elapsed = time.perf_counter() - start
    first_frame_lat = (first_frame_time - start) if first_frame_time else None
    return frames, total_payload, elapsed, first_frame_lat

def main():
    print(f"Opening {PORT} @ {BAUD}...")
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    ser.reset_input_buffer()
    time.sleep(0.1)

    # Send want_config_id
    frame = make_want_config(42)
    print(f"Sending want_config_id ({len(frame)} bytes)...")

    t0 = time.perf_counter()
    ser.write(frame)
    ser.flush()

    # Read all response frames
    frames, total_bytes, elapsed, first_lat = read_frames(ser, timeout=8.0)
    t1 = time.perf_counter()

    print(f"\nResults:")
    print(f"  Frames received:     {frames}")
    print(f"  Total payload bytes: {total_bytes} ({total_bytes/1024:.2f} KB)")
    print(f"  First frame latency: {first_lat*1000:.1f} ms" if first_lat else "  First frame: N/A")
    print(f"  Total elapsed:       {elapsed*1000:.1f} ms")
    if total_bytes > 0 and elapsed > 0:
        print(f"  Throughput:          {total_bytes/elapsed/1024:.2f} KB/s")

    ser.close()

if __name__ == "__main__":
    main()
