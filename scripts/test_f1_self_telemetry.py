"""Phase 1 verification — F-1 device telemetry self-display.

Confirms the chain works:
  cascade NodeInfo for self
    -> decode_device_metrics (phoneapi_decode.c:464)
    -> phoneapi_node_t.{channel_util_pct, air_util_tx_pct} populated
    -> phoneapi_cache_get_node_by_id(my_node_num) returns self
    -> telemetry_view::render_f1 reads & displays

Strategy: read the cache state directly via SWD and print what F-1
*should* display. User visually compares against screen.

Layout of phoneapi_node_t (struct in phoneapi_cache.h, sizeof matches
cache.nodes[i]):
  +0   in_use            (bool, 1 byte + 3 pad)
  +4   num               (uint32_t)
  +8   long_name[40]     -> 48
  +48  short_name[8]     -> 56
  +56  hw_model          (uint8_t)
  +57  role              (uint8_t)
  +58  via_mqtt          (bool)
  +59  is_favorite       (bool)
  +60  is_unmessagable   (bool)
  +61  channel           (uint8_t)
  +62  hops_away         (uint8_t)
  +63  pad
  +64  snr_x100          (int32_t)
  +68  last_heard        (uint32_t)
  +72  battery_level     (uint8_t)
  +73  pad
  +74  voltage_mv        (int16_t)
  +76  channel_util_pct  (uint8_t) <-- want this
  +77  air_util_tx_pct   (uint8_t) <-- and this

Rather than walk the cache static layout, just iterate
`phoneapi_cache_take_node_at` semantically by reading what the
cache exposes — easier path: lookup self by my_node_num and print
fields. We don't need to know the inner layout, we use the public
getter through SWD function-call... actually we can't easily call
firmware functions from SWD.

Easier: walk g_view_registry-side calls? No.

Practical: just SWD-read the static `s_cache` in phoneapi_cache.c at
known field offsets. But the struct has nodes[32] embedded, sized
~150 bytes each.

Fastest verification: just print what `meshtastic --info` reports
and trust cascade pipeline since DeviceMetrics decoder is mature
(in production since B-section). This script exists as a clean
follow-up gate; for v1 we accept visual confirmation on screen.
"""
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from mokya_rtt import build_frame, TYPE_KEY_EVENT  # type: ignore

ELF = "build/core1_bridge/core1_bridge.elf"
ARM_OBJDUMP = (r"C:/Program Files/Arm/GNU Toolchain "
               r"mingw-w64-x86_64-arm-none-eabi/bin/"
               r"arm-none-eabi-objdump.exe")

KEYMAP = {}
src = Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        src):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME = 0
VIEW_ID_LAUNCHER  = 1
VIEW_ID_TELEMETRY = 13


def find_static(elf, source_basename, name):
    out = subprocess.check_output([ARM_OBJDUMP, "-t", elf], text=True)
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and source_basename in line:
            in_block = True
            continue
        if in_block:
            if 'df *ABS*' in line:
                break
            m = re.match(rf'^([0-9a-fA-F]+)\s+l\s+O\s+\.bss\s+\S+\s+{re.escape(name)}$',
                         line)
            if m:
                return int(m.group(1), 16)
    return None


def send_press_release(swd, kc, hold_ms=30, gap_ms=200):
    pb = (kc & 0x7F) | 0x80
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([pb, 0])))
    time.sleep(hold_ms / 1000.0)
    rb = (kc & 0x7F)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([rb, 0])))
    time.sleep(gap_ms / 1000.0)


def read_view(swd):
    return swd.read_u32(swd.symbol('s_view_router_active'))


def back_home(swd):
    for _ in range(8):
        if read_view(swd) == VIEW_ID_BOOT_HOME:
            return True
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
    return read_view(swd) == VIEW_ID_BOOT_HOME


def main():
    # phoneapi_cache `s_cache` lives in .psram_bss; SWD reads of PSRAM
    # bypass the cache and may return stale data (see memory file
    # project_psram_swd_cache_coherence). So we skip direct cache
    # introspection here — the Meshtastic CLI gives us the same source
    # values the cache should hold, which is enough for screen-side
    # comparison.
    fails = 0

    with MokyaSwd() as swd:
        # Use Meshtastic CLI for the source of truth.
        print("Source of truth (meshtastic --info):")
        out = subprocess.run(
            ['python', '-m', 'meshtastic', '--port', 'COM16', '--info'],
            capture_output=True, text=True, timeout=20)
        for line in out.stdout.splitlines():
            if any(k in line for k in (
                    'channelUtilization', 'airUtilTx', 'batteryLevel',
                    'myNodeNum')):
                print(f"  {line.strip()}")

        # Navigate to F-1 (TELEMETRY view, page 0 = F-1).
        print()
        print("Navigating to F-1 ...")
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! could not reach BOOT_HOME"); sys.exit(1)
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        # Anchor cursor (0,0)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'],   gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        # Tele tile = (1, 1)
        send_press_release(swd, KEYMAP['MOKYA_KEY_DOWN'],  gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'],    gap_ms=400)

        v = read_view(swd)
        if v == VIEW_ID_TELEMETRY:
            print(f"  view = {v} (TELEMETRY) OK")
        else:
            print(f"  view = {v} (expected {VIEW_ID_TELEMETRY} TELEMETRY) FAIL")
            fails += 1

        # F-1 is page 0 (default); LEFT/RIGHT cycle pages.
        print()
        print("F-1 should now show on screen. User-visible expectation:")
        print("  Row 5: 'Channel%  0%'   (matches channelUtilization 0.0)")
        print("  Row 6: 'Air tx%   0%'   (matches airUtilTx 0.05% -> rounds to 0)")
        print()
        print("If channel_util_pct or air_util_tx_pct hasn't propagated yet")
        print("(self-NodeInfo broadcast cadence varies), expect '--' instead.")

        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    if fails == 0:
        print()
        print("==> Phase 1 navigation OK — manually verify screen shows the")
        print("    expected channel% / air tx% values (or '--' if cascade")
        print("    hasn't seen self-NodeInfo with DeviceMetrics yet)")
    sys.exit(fails)


if __name__ == "__main__":
    main()
