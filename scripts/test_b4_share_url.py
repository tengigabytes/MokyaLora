"""Phase 5a B-4 verification — channel-share URL ground-truth match.

Drives the channel_share_view rendering path:
  BOOT_HOME -> launcher -> Chan -> slot 0 (PRIMARY occupied) -> OK ->
  CHANNEL_EDIT -> SET key -> CHANNEL_SHARE

Then SWD-reads the rendered URL out of channel_share_view's static
`s.url_buf` and compares against `meshtastic --info` Primary channel
URL. Match criteria: structural equivalence (decoded ChannelSet
fields agree where both sides emit them) — not byte-exact, because
v1 cache only captures a subset of LoRaConfig fields.

Lessons applied from B-3 audit (commit cebef0d):
  - Field numbers cross-checked against generated proto headers.
  - Behavioural verification (decode my URL + decode CLI URL +
    compare) is the canonical "correct on the wire" test for any
    protobuf encoder. Structural-only nav routing is necessary but
    not sufficient.
"""
import argparse
import base64
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
for m in re.finditer(
        r'#define\s+(MOKYA_KEY_\w+)\s+\(\(mokya_keycode_t\)(0x[0-9A-Fa-f]+)\)',
        Path('firmware/mie/include/mie/keycode.h').read_text(encoding='utf-8')):
    KEYMAP[m.group(1)] = int(m.group(2), 16)

VIEW_ID_BOOT_HOME       = 0
VIEW_ID_CHANNELS        = 16
VIEW_ID_CHANNEL_EDIT    = 17
VIEW_ID_CHANNEL_SHARE   = 26   # post-Phase-5a enum

# channel_share_view::s layout (Phase 5b — qr field shifted offsets)
#   +0  header*  +4 qr*  +8 url*  +12 status*
#   +16 active_idx (uint8_t)
#   +20 last_change_seq (uint32_t)
#   +24 url_buf[256]
#   +280 url_len (size_t = uint32 on 32-bit)
CSHARE_OFF_URL_BUF = 24
CSHARE_OFF_URL_LEN = 280


def find_static(elf, source_basename, name='s'):
    out = subprocess.check_output([ARM_OBJDUMP, "-t", elf], text=True)
    in_block = False
    for line in out.splitlines():
        if 'df *ABS*' in line and source_basename in line:
            in_block = True; continue
        if in_block:
            if 'df *ABS*' in line: break
            # match either .bss or .psram_bss (B-4 puts s in PSRAM)
            m = re.match(rf'^([0-9a-fA-F]+)\s+l\s+O\s+\.\S*bss\s+\S+\s+{re.escape(name)}$',
                         line)
            if m: return int(m.group(1), 16)
    return None


def to_uncached_psram(addr):
    """SWD reads of PSRAM cached aliases (0x11xxxxxx) bypass the XIP
    cache and may return stale data; use the 0x15xxxxxx uncached alias
    for coherent reads (memory: project_psram_swd_cache_coherence)."""
    if (addr & 0xFF000000) == 0x11000000:
        return (addr & 0x00FFFFFF) | 0x15000000
    return addr


def send_press_release(swd, kc, hold_ms=30, gap_ms=200):
    pb = (kc & 0x7F) | 0x80
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([pb, 0])))
    time.sleep(hold_ms / 1000.0)
    rb = (kc & 0x7F)
    swd.rtt_send_frame(build_frame(TYPE_KEY_EVENT, bytes([rb, 0])))
    time.sleep(gap_ms / 1000.0)


def back_home(swd):
    for _ in range(10):
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v == VIEW_ID_BOOT_HOME: return True
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=300)
    return swd.read_u32(swd.symbol('s_view_router_active')) == VIEW_ID_BOOT_HOME


# ── Minimal ChannelSet protobuf parser (no nanopb dependency) ─────────

def _read_varint(buf, pos):
    v = 0
    shift = 0
    while True:
        b = buf[pos]; pos += 1
        v |= (b & 0x7F) << shift
        if (b & 0x80) == 0: break
        shift += 7
    return v, pos


def _read_field(buf, pos):
    tag, pos = _read_varint(buf, pos)
    f = tag >> 3
    w = tag & 7
    if w == 0:
        v, pos = _read_varint(buf, pos)
        return f, w, v, pos
    if w == 2:
        ln, pos = _read_varint(buf, pos)
        v = bytes(buf[pos:pos + ln])
        return f, w, v, pos + ln
    if w == 5:
        v = struct.unpack('<I', bytes(buf[pos:pos + 4]))[0]
        return f, w, v, pos + 4
    raise ValueError(f"unsupported wire type {w} at {pos}")


def parse_channel_set(buf):
    """Returns dict { 'settings': [...ChannelSettings...], 'lora_config': {...} }."""
    out = {'settings': [], 'lora_config': {}}
    pos = 0
    while pos < len(buf):
        f, w, v, pos = _read_field(buf, pos)
        if f == 1 and w == 2:
            out['settings'].append(parse_channel_settings(v))
        elif f == 2 and w == 2:
            out['lora_config'] = parse_lora_config(v)
    return out


def parse_channel_settings(buf):
    out = {}
    pos = 0
    while pos < len(buf):
        f, w, v, pos = _read_field(buf, pos)
        if f == 2 and w == 2:
            out['psk'] = v
        elif f == 3 and w == 2:
            out['name'] = v.decode('utf-8', errors='replace')
        elif f == 7 and w == 2:
            out['module_settings'] = parse_module_settings(v)
    return out


def parse_module_settings(buf):
    out = {}
    pos = 0
    while pos < len(buf):
        f, w, v, pos = _read_field(buf, pos)
        if f == 1 and w == 0: out['position_precision'] = v
        elif f == 2 and w == 0: out['is_muted'] = bool(v)
    return out


def parse_lora_config(buf):
    out = {}
    pos = 0
    while pos < len(buf):
        f, w, v, pos = _read_field(buf, pos)
        if   f == 1 and w == 0: out['use_preset']     = bool(v)
        elif f == 2 and w == 0: out['modem_preset']   = v
        elif f == 3 and w == 0: out['bandwidth']      = v
        elif f == 4 and w == 0: out['spread_factor']  = v
        elif f == 5 and w == 0: out['coding_rate']    = v
        elif f == 7 and w == 0: out['region']         = v
        elif f == 8 and w == 0: out['hop_limit']      = v
        elif f == 9 and w == 0: out['tx_enabled']     = bool(v)
        elif f == 10 and w == 0: out['tx_power']      = v
        elif f == 11 and w == 0: out['channel_num']   = v
        elif f == 13 and w == 0: out['sx126x_rx_boosted_gain'] = bool(v)
        elif f == 14 and w == 5: out['override_frequency_raw'] = v
    return out


def decode_url(url):
    PREFIX = 'https://meshtastic.org/e/#'
    assert url.startswith(PREFIX), f"bad prefix: {url[:30]}..."
    body = url[len(PREFIX):]
    # url-safe base64; pad to multiple of 4
    body += '=' * ((4 - len(body) % 4) % 4)
    raw = base64.urlsafe_b64decode(body)
    return parse_channel_set(raw), raw


# ── main ─────────────────────────────────────────────────────────────

def main():
    fails = 0

    cshare_addr = find_static(ELF, 'channel_share_view.c', 's')
    if cshare_addr is None:
        print("!! could not locate channel_share_view::s — abort"); sys.exit(1)
    print(f"channel_share_view::s @ 0x{cshare_addr:08x}")

    # 1. Pull CLI ground-truth URL.
    print()
    print("Fetching meshtastic --info Primary URL ...")
    cli_out = subprocess.run(
        ['python', '-m', 'meshtastic', '--port', 'COM16', '--info'],
        capture_output=True, text=True, timeout=30)
    cli_url = None
    for line in cli_out.stdout.splitlines():
        if 'Primary channel URL' in line:
            cli_url = line.split(':', 1)[1].strip()
            break
    if cli_url is None:
        print("!! couldn't extract Primary URL from --info — abort")
        sys.exit(1)
    print(f"  CLI URL ({len(cli_url)} chars): {cli_url}")

    cli_decoded, cli_raw = decode_url(cli_url)
    print(f"  CLI ChannelSet: settings={len(cli_decoded['settings'])}, "
          f"lora_config={len(cli_decoded['lora_config'])} fields")

    # 2. Drive UI: BOOT_HOME → launcher → Chan → slot 0 OK → CHANNEL_EDIT → SET → CHANNEL_SHARE
    print()
    print("Driving channel_share_view via key inject ...")
    with MokyaSwd() as swd:
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_RTT)
        if not back_home(swd):
            print("!! couldn't reach BOOT_HOME"); sys.exit(1)

        # FUNC + anchor (0,0) + RIGHT to (0,1) Chan tile
        send_press_release(swd, KEYMAP['MOKYA_KEY_FUNC'], gap_ms=400)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'], gap_ms=60)
        for _ in range(3):
            send_press_release(swd, KEYMAP['MOKYA_KEY_LEFT'], gap_ms=60)
        send_press_release(swd, KEYMAP['MOKYA_KEY_RIGHT'], gap_ms=80)
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_CHANNELS:
            print(f"!! expected CHANNELS, got {v}"); sys.exit(1)

        # Anchor channels_view cursor at slot 0 (PRIMARY).
        for _ in range(8):
            send_press_release(swd, KEYMAP['MOKYA_KEY_UP'], gap_ms=40)
        # OK on slot 0 (occupied PRIMARY) → CHANNEL_EDIT
        send_press_release(swd, KEYMAP['MOKYA_KEY_OK'], gap_ms=400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_CHANNEL_EDIT:
            print(f"!! expected CHANNEL_EDIT, got {v}"); sys.exit(1)

        # SET → CHANNEL_SHARE
        send_press_release(swd, KEYMAP['MOKYA_KEY_SET'], gap_ms=400)
        v = swd.read_u32(swd.symbol('s_view_router_active'))
        if v != VIEW_ID_CHANNEL_SHARE:
            print(f"!! expected CHANNEL_SHARE ({VIEW_ID_CHANNEL_SHARE}), got {v}")
            sys.exit(1)
        print(f"  view = {v} (CHANNEL_SHARE) — entered B-4")

        # Wait for first refresh to populate the URL.
        time.sleep(1.2)

        # 3. SWD-read s.url_len + s.url_buf via uncached PSRAM alias.
        addr_uc = to_uncached_psram(cshare_addr)
        if addr_uc != cshare_addr:
            print(f"  PSRAM uncached alias 0x{addr_uc:08x}")
        url_len_bytes = swd.read_mem(addr_uc + CSHARE_OFF_URL_LEN, 4)
        url_len = struct.unpack('<I', url_len_bytes)[0]
        if url_len == 0 or url_len > 256:
            print(f"!! url_len = {url_len} (zero or out of range)")
            sys.exit(1)
        url_bytes = swd.read_mem(addr_uc + CSHARE_OFF_URL_BUF, url_len)
        my_url = bytes(url_bytes).decode('utf-8', errors='replace')
        print(f"  my URL  ({len(my_url)} chars): {my_url}")

        # Cleanup
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        send_press_release(swd, KEYMAP['MOKYA_KEY_BACK'], gap_ms=400)
        back_home(swd)
        swd.set_key_inject_mode(swd.KEY_INJECT_MODE_SWD)

    # 4. Decode mine + structural compare.
    my_decoded, my_raw = decode_url(my_url)
    print()
    print("Decoded structures:")
    print(f"  CLI:")
    for s in cli_decoded['settings']:
        print(f"    settings: psk={s.get('psk',b'').hex()} "
              f"name={s.get('name','')!r} mod={s.get('module_settings',{})}")
    print(f"    lora_config: {cli_decoded['lora_config']}")
    print(f"  MINE:")
    for s in my_decoded['settings']:
        print(f"    settings: psk={s.get('psk',b'').hex()} "
              f"name={s.get('name','')!r} mod={s.get('module_settings',{})}")
    print(f"    lora_config: {my_decoded['lora_config']}")

    # 5. Field-by-field comparison.
    print()
    print("Comparison (mine == cli on shared fields):")

    def check(label, mine, cli):
        nonlocal fails
        if mine == cli:
            print(f"  [PASS] {label:<40} {mine}")
        else:
            print(f"  [FAIL] {label:<40} mine={mine!r} cli={cli!r}")
            fails += 1

    if cli_decoded['settings'] and my_decoded['settings']:
        c = cli_decoded['settings'][0]
        m = my_decoded['settings'][0]
        check("settings[0].psk",  m.get('psk', b''), c.get('psk', b''))
        check("settings[0].name", m.get('name', ''), c.get('name', ''))
        cm = c.get('module_settings', {})
        mm = m.get('module_settings', {})
        check("module_settings.position_precision",
              mm.get('position_precision'), cm.get('position_precision'))
        check("module_settings.is_muted",
              mm.get('is_muted'), cm.get('is_muted'))
    else:
        print(f"  [FAIL] settings count: mine={len(my_decoded['settings'])} "
              f"cli={len(cli_decoded['settings'])}")
        fails += 1

    cl = cli_decoded['lora_config']
    ml = my_decoded['lora_config']
    for k in ('use_preset', 'modem_preset', 'region', 'hop_limit', 'tx_power'):
        check(f"lora_config.{k}", ml.get(k), cl.get(k))

    print()
    if fails == 0:
        print("==> Phase 5a PASS — URL structurally matches CLI ground truth")
        sys.exit(0)
    else:
        print(f"==> Phase 5a FAIL ({fails} mismatches)")
        sys.exit(1)


if __name__ == "__main__":
    main()
