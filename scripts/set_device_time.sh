#!/usr/bin/env bash
# set_device_time.sh — push host wall clock to MokyaLora over USB CDC.
#
# Why this exists:
#   - MokyaLora has no battery-backed RTC, so wall clock is lost on every
#     reboot or chip erase.
#   - P2-18 fixed the upstream Meshtastic readFromRTC clobber so setTime
#     "sticks" once delivered, but client.meshtastic.org does NOT
#     auto-send AdminMessage.set_time_only on connect — only the official
#     phone apps and `python -m meshtastic`'s SerialInterface do.
#   - Without this, neighbour nodes' last_heard, message timestamps, and
#     position freshness all render as 1970 in the web console.
#
# Long-term fix is M5 GPS time wiring (Core 1 → Core 0 IpcGpsBuf, scheduled
# routine 2026-05-02). Until then, run this once after each device reboot.
#
# Usage:
#   bash scripts/set_device_time.sh                 # auto-detect COM port
#   bash scripts/set_device_time.sh COM10           # explicit port

set -euo pipefail

PORT="${1:-}"

if [ -z "$PORT" ]; then
    PORT="$(powershell -NoProfile -ExecutionPolicy Bypass \
        -Command ". scripts/_mokya-port.ps1; Resolve-MokyaPort" 2>/dev/null \
        | tail -1 | tr -d '\r' || true)"
fi

if [ -z "$PORT" ]; then
    echo "ERROR: could not auto-detect MokyaLora COM port (VID 0x2E8A)." >&2
    echo "Pass the port explicitly: bash scripts/set_device_time.sh COMxx" >&2
    exit 1
fi

echo "Setting time on $PORT ..."
python -c "
import meshtastic.serial_interface, time, sys
try:
    iface = meshtastic.serial_interface.SerialInterface('$PORT')
except Exception as e:
    print(f'ERROR: could not open $PORT: {e}', file=sys.stderr)
    print('Hint: disconnect any web console / IDE serial monitor first.', file=sys.stderr)
    sys.exit(1)
now = int(time.time())
iface.localNode.setTime(now)
print(f'OK: sent setTime epoch={now} ({time.strftime(\"%Y-%m-%d %H:%M:%S %z\", time.localtime(now))})')
time.sleep(1)
iface.close()
"
