"""Detailed serial latency profiler — logs exact timestamps per event."""
import serial
import struct
import time
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM16"
BAUD = 115200
START1, START2, HEADER_LEN = 0x94, 0xC3, 4

def make_want_config(config_id=42):
    payload = bytes([0x18, config_id & 0x7F])
    length = len(payload)
    return bytes([START1, START2, (length >> 8) & 0xFF, length & 0xFF]) + payload

def main():
    t_start = time.perf_counter()
    ser = serial.Serial(PORT, BAUD, timeout=0.01)
    t_open = time.perf_counter()
    print(f"[{(t_open-t_start)*1000:8.1f} ms] Serial port opened")

    # Drain any pending data from device (log lines etc)
    pre_bytes = 0
    while True:
        chunk = ser.read(4096)
        if not chunk:
            break
        pre_bytes += len(chunk)
    t_drain = time.perf_counter()
    print(f"[{(t_drain-t_start)*1000:8.1f} ms] Drained {pre_bytes} bytes of pre-existing data")

    # Send want_config_id
    frame = make_want_config()
    ser.write(frame)
    ser.flush()
    t_send = time.perf_counter()
    print(f"[{(t_send-t_start)*1000:8.1f} ms] Sent want_config_id ({len(frame)} bytes)")

    # Read responses with detailed timing
    buf = bytearray()
    frames = []
    total_payload = 0
    t_first_byte = None
    t_first_frame = None
    read_count = 0

    deadline = t_send + 8.0
    while time.perf_counter() < deadline:
        waiting = ser.in_waiting
        if waiting > 0:
            data = ser.read(waiting)
            now = time.perf_counter()
            read_count += 1
            if t_first_byte is None:
                t_first_byte = now
                print(f"[{(now-t_start)*1000:8.1f} ms] First byte received ({len(data)} bytes, in_waiting was {waiting})")
            buf.extend(data)

            # Parse frames
            while len(buf) >= HEADER_LEN:
                idx = -1
                for i in range(len(buf) - 1):
                    if buf[i] == START1 and buf[i+1] == START2:
                        idx = i
                        break
                if idx < 0:
                    buf = buf[-1:]
                    break
                if idx > 0:
                    # Non-frame bytes (log lines)
                    skipped = bytes(buf[:idx])
                    buf = buf[idx:]
                if len(buf) < HEADER_LEN:
                    break
                payload_len = (buf[2] << 8) | buf[3]
                total_frame_len = HEADER_LEN + payload_len
                if len(buf) < total_frame_len:
                    break

                now2 = time.perf_counter()
                frames.append((now2, payload_len))
                total_payload += payload_len
                if t_first_frame is None:
                    t_first_frame = now2
                    print(f"[{(now2-t_start)*1000:8.1f} ms] First protobuf frame (payload={payload_len} B)")
                buf = buf[total_frame_len:]
        else:
            # Check for gap (end of burst)
            if frames and len(frames) > 5:
                time.sleep(0.02)
                if ser.in_waiting == 0:
                    break
            else:
                time.sleep(0.001)

    t_end = time.perf_counter()

    print(f"\n=== Summary ===")
    print(f"  Serial open:          {(t_open-t_start)*1000:.1f} ms")
    print(f"  Pre-drain:            {pre_bytes} bytes in {(t_drain-t_open)*1000:.1f} ms")
    print(f"  Send → first byte:    {(t_first_byte-t_send)*1000:.1f} ms" if t_first_byte else "  First byte: N/A")
    print(f"  Send → first frame:   {(t_first_frame-t_send)*1000:.1f} ms" if t_first_frame else "  First frame: N/A")
    print(f"  First frame → last:   {(frames[-1][0]-t_first_frame)*1000:.1f} ms" if len(frames) > 1 and t_first_frame else "")
    print(f"  Total frames:         {len(frames)}")
    print(f"  Total payload:        {total_payload} B ({total_payload/1024:.2f} KB)")
    print(f"  Total elapsed:        {(t_end-t_send)*1000:.1f} ms")
    print(f"  Read syscalls:        {read_count}")

    if len(frames) > 1:
        show_all = "--all" in sys.argv
        if show_all:
            print(f"\n  Frame timing (all {len(frames)} frames):")
            for i, (t, plen) in enumerate(frames):
                dt = (t - t_send) * 1000
                gap = (t - frames[i-1][0]) * 1000 if i > 0 else 0
                print(f"    #{i+1:3d}: +{dt:8.1f} ms  (gap {gap:6.1f} ms)  payload={plen}")
        else:
            print(f"\n  Frame timing (first 5):")
            for i, (t, plen) in enumerate(frames[:5]):
                dt = (t - t_send) * 1000
                gap = (t - frames[i-1][0]) * 1000 if i > 0 else 0
                print(f"    #{i+1:3d}: +{dt:8.1f} ms  (gap {gap:6.1f} ms)  payload={plen}")
            if len(frames) > 8:
                print(f"    ...")
            print(f"  Frame timing (last 3):")
            for i in range(max(5, len(frames)-3), len(frames)):
                t, plen = frames[i]
                dt = (t - t_send) * 1000
                gap = (t - frames[i-1][0]) * 1000
                print(f"    #{i+1:3d}: +{dt:8.1f} ms  (gap {gap:6.1f} ms)  payload={plen}")

    ser.close()

if __name__ == "__main__":
    main()
