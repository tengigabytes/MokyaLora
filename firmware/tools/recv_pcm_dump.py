#!/usr/bin/env python3
"""
recv_pcm_dump.py — trigger mic_dump on the bringup shell, receive raw PCM,
                   save as a 16-bit mono WAV file for analysis in Audacity.

Usage:
    python recv_pcm_dump.py [--port COM4] [--out mic_dump.wav]

Requires: pyserial  (pip install pyserial)

Protocol emitted by mic_dump() on the firmware side:
    PCMDUMP_START <sample_rate> <num_samples>\\n
    <raw int16_t bytes, little-endian, num_samples × 2 bytes>
    PCMDUMP_END\\n
"""

import argparse
import sys
import time
import wave

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed.  Run: pip install pyserial")
    sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Receive mic_dump PCM from bringup shell")
    parser.add_argument("--port", default="COM4", help="Serial port (default: COM4)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--out",  default="mic_dump.wav", help="Output WAV file (default: mic_dump.wav)")
    parser.add_argument("--no-send", action="store_true",
                        help="Don't send mic_dump command; just listen (useful if already triggered)")
    args = parser.parse_args()

    print(f"Opening {args.port} at {args.baud} baud...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=5)
    except serial.SerialException as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    time.sleep(0.3)
    ser.reset_input_buffer()

    if not args.no_send:
        print("Sending 'mic_dump' command...")
        ser.write(b"mic_dump\r")

    # --- Wait for PCMDUMP_START ---
    print("Waiting for PCMDUMP_START marker...")
    sample_rate = None
    num_samples = None
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        raw = ser.readline()
        line = raw.decode("ascii", errors="ignore").strip()
        if line:
            print(f"  < {line}")
        if line.startswith("PCMDUMP_START"):
            parts = line.split()
            if len(parts) < 3:
                print("ERROR: malformed PCMDUMP_START line")
                ser.close()
                sys.exit(1)
            sample_rate = int(parts[1])
            num_samples  = int(parts[2])
            break
    else:
        print("ERROR: timed out waiting for PCMDUMP_START")
        ser.close()
        sys.exit(1)

    total_bytes = num_samples * 2
    duration_s  = num_samples / sample_rate
    print(f"Receiving {num_samples} samples @ {sample_rate} Hz "
          f"({total_bytes} bytes, {duration_s:.2f} s)...")

    # --- Read binary payload ---
    ser.timeout = 30
    data = bytearray()
    while len(data) < total_bytes:
        chunk = ser.read(total_bytes - len(data))
        if not chunk:
            print(f"\nERROR: serial timeout — got {len(data)}/{total_bytes} bytes")
            ser.close()
            sys.exit(1)
        data.extend(chunk)
        pct = len(data) / total_bytes * 100
        print(f"  {len(data)}/{total_bytes} bytes  ({pct:.0f}%)", end="\r")
    print()

    # --- Wait for PCMDUMP_END ---
    ser.timeout = 5
    end_line = ser.readline().decode("ascii", errors="ignore").strip()
    print(f"  < {end_line}")
    if not end_line.startswith("PCMDUMP_END"):
        print("WARN: expected PCMDUMP_END but got something else — file may be OK")

    ser.close()

    # --- Save WAV ---
    with wave.open(args.out, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)           # int16 = 2 bytes
        wf.setframerate(sample_rate)
        wf.writeframes(bytes(data))

    print(f"Saved: {args.out}  ({duration_s:.2f} s, {sample_rate} Hz, mono 16-bit)")
    print("Open in Audacity: File → Import → Audio, or drag-and-drop the .wav file.")


if __name__ == "__main__":
    main()
