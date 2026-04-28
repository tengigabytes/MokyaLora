#!/usr/bin/env python3
"""Phase 1+2 verification harness.

Drives the eight automated checks listed in chat:
  1. Boot view = L-0 (BOOT_HOME = 0)
  2. FUNC short → LAUNCHER (1)
  3. OK in launcher → tile target (MESSAGES = 2 by default)
  4. BACK in launcher cancels back to caller
  5. FUNC long ≥2 s → s_func_long_consumed = 1
  6. Router runs 8 s with no fault trace
  7. draft_store partition writable (proxied via direct SWD write)
  8. Draft persists across reset (write magic+id+text, reset, read back)

Reuses inject_keys helpers for symbol lookup + queue_events.
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

import inject_keys as ik  # noqa: E402

ELF        = str(ROOT / 'build' / 'core1_bridge' / 'core1_bridge.elf')
DEVICE     = 'RP2350_M33_1'
JLINK      = ik.JLINK
KEY        = ik.load_keycode_map()

VIEW_BOOT_HOME    = 0
VIEW_LAUNCHER     = 1
VIEW_MESSAGES     = 2

DRAFT_PARTITION   = 0x10C10000
DRAFT_OFFSET      = 0x00C10000
DRAFT_SLOT_SIZE   = 4096
DRAFT_MAGIC       = 0x54465244  # 'DRFT'

PASS = 'PASS'
FAIL = 'FAIL'

results = []

def report(name, ok, detail=''):
    results.append((name, ok, detail))
    mark = PASS if ok else FAIL
    line = f"  [{mark}] {name}"
    if detail:
        line += f"  -- {detail}"
    print(line)


def jlink(cmds, timeout=30):
    return ik.jlink_script(DEVICE, cmds)


def reset_target():
    """Hardware reset via SYSRESETREQ; takes ~1 s."""
    jlink(['r', 'g'])
    time.sleep(2.0)


def flash_erase_draft_partition():
    """Wipe slot 0 of the draft partition by loadbin'ing a 4 KB 0xFF blob.
    J-Link's flash loader auto-erases the touched sector before program."""
    tmp = ROOT / 'build' / 'draft_wipe.bin'
    tmp.write_bytes(b'\xFF' * DRAFT_SLOT_SIZE)
    win_path = str(tmp).replace('/', '\\')
    jlink([f'loadbin "{win_path}" 0x{DRAFT_PARTITION:08X}'])


def read_active():
    return ik.read_u32(DEVICE, ik.buf_addrs(ELF)['s_active'])


def read_long_consumed():
    addr = ik.nm_symbol(ELF, 's_func_long_consumed')
    return ik.read_mem(DEVICE, addr, 1)[0]


def check_inject():
    ik.check_inject_alive(DEVICE, ELF)


def inject(seq):
    ik.queue_events(DEVICE, ELF, seq)
    time.sleep(0.10)


# ── Tests ──────────────────────────────────────────────────────────────

def t1_boot_view():
    print("\n--- T1: Boot view = L-0 ---")
    reset_target()
    av = read_active()
    report("active = BOOT_HOME (0)", av == VIEW_BOOT_HOME, f"got {av}")


def t2_func_short_to_launcher():
    print("\n--- T2: FUNC short -> LAUNCHER ---")
    check_inject()
    inject(ik.press(KEY['KEY_FUNC']))
    time.sleep(0.4)
    av = read_active()
    report("active = LAUNCHER (1)", av == VIEW_LAUNCHER, f"got {av}")


def t3_ok_launches_messages():
    print("\n--- T3: OK in launcher -> MESSAGES ---")
    # Assumed precondition: in LAUNCHER (left over from T2).
    av = read_active()
    if av != VIEW_LAUNCHER:
        report("precondition: active=LAUNCHER", False, f"got {av}")
        return
    inject(ik.press(KEY['KEY_OK']))
    time.sleep(0.4)
    av = read_active()
    report("active = MESSAGES (2)", av == VIEW_MESSAGES, f"got {av}")


def t4_back_cancels_modal():
    print("\n--- T4: BACK in launcher cancels back to caller ---")
    # Start fresh: reset → BOOT_HOME → FUNC → LAUNCHER → BACK → BOOT_HOME
    reset_target()
    inject(ik.press(KEY['KEY_FUNC']))
    time.sleep(0.4)
    av = read_active()
    if av != VIEW_LAUNCHER:
        report("step: FUNC -> LAUNCHER", False, f"got {av}")
        return
    inject(ik.press(KEY['KEY_BACK']))
    time.sleep(0.4)
    av = read_active()
    report("active = BOOT_HOME (0) after BACK", av == VIEW_BOOT_HOME, f"got {av}")


def t5_func_long_press():
    print("\n--- T5: FUNC long >= 2 s ---")
    reset_target()
    # Press only.
    ik.queue_events(DEVICE, ELF, ik.press_only(KEY['KEY_FUNC']))
    time.sleep(2.4)        # exceed 2000 ms threshold
    consumed = read_long_consumed()
    # Release.
    ik.queue_events(DEVICE, ELF, ik.release_only(KEY['KEY_FUNC']))
    time.sleep(0.2)
    report("s_func_long_consumed == 1", consumed == 1, f"got {consumed}")


def t6_router_runs_no_fault():
    print("\n--- T6: 8 s RTT capture, no fault traces ---")
    reset_target()
    log = ROOT / 'build' / 'rtt_t6.log'
    if log.exists():
        log.unlink()
    # JLinkRTTLogger writes to file given on cmd line; on Windows it sometimes
    # writes to %TEMP%/rtt.log instead. Try both.
    alt = Path.home() / 'AppData' / 'Local' / 'Temp' / 'rtt.log'
    if alt.exists():
        try: alt.unlink()
        except OSError: pass
    proc = subprocess.Popen(
        ['C:/Program Files/SEGGER/JLink_V932/JLinkRTTLogger.exe',
         '-Device', DEVICE, '-If', 'SWD', '-Speed', '4000',
         '-RTTSearchRanges', '0x20000000 0x80000', '-RTTChannel', '0',
         str(log)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(8.0)
    proc.terminate()
    try: proc.wait(timeout=2)
    except subprocess.TimeoutExpired: proc.kill()
    text = ''
    for p in (log, alt):
        if p.exists():
            try: text += p.read_text(errors='ignore')
            except OSError: pass
    fault_markers = [',fault,', ',hardfault,', ',panic,', 'pm,c1_last,cause=2']
    found = [m for m in fault_markers if m in text]
    bytes_captured = len(text)
    report("no fault markers in 8 s window",
           not found and bytes_captured > 100,
           f"bytes={bytes_captured} found={found}")


def _craft_slot(draft_id, text):
    text_bytes = text.encode('utf-8')
    text_len = len(text_bytes)
    blob = bytearray(b'\xFF' * DRAFT_SLOT_SIZE)
    struct.pack_into('<IIHH', blob, 0,
                     DRAFT_MAGIC, draft_id, text_len, 0)
    # crc32 left as 0
    blob[12:16] = b'\x00\x00\x00\x00'
    blob[16:16 + text_len] = text_bytes
    return bytes(blob)


def write_draft_via_swd(draft_id, text):
    """Use J-Link loadbin to write a crafted slot to flash. Erase first
    via the JLink erase command, then loadbin the slot."""
    blob = _craft_slot(draft_id, text)
    tmp = ROOT / 'build' / f'draft_slot_{draft_id:08x}.bin'
    tmp.write_bytes(blob)
    win_path = str(tmp).replace('/', '\\')
    # loadbin auto-erases the affected 4 KB sector before programming.
    return jlink([f'loadbin "{win_path}" 0x{DRAFT_PARTITION:08X}'])


def t7_8_draft_persistence():
    print("\n--- T7+T8: Draft round-trip across reset ---")
    test_id   = 0xC0FFEE01
    test_text = "phase2_persist_marker"

    # Pre-clean.
    flash_erase_draft_partition()
    time.sleep(0.3)

    # Write a crafted slot via SWD (proves T7: partition is programmable).
    write_draft_via_swd(test_id, test_text)
    time.sleep(0.3)

    # Read back BEFORE reset.
    pre_buf = ik.read_mem(DEVICE, DRAFT_PARTITION, 64)
    pre_magic, pre_id, pre_len = struct.unpack_from('<IIH', pre_buf, 0)
    pre_text = pre_buf[16:16 + pre_len].decode('utf-8', errors='replace')

    pre_ok = (pre_magic == DRAFT_MAGIC and pre_id == test_id
              and pre_text == test_text)
    report("T7 partition write+read pre-reset",
           pre_ok,
           f"magic=0x{pre_magic:08x} id=0x{pre_id:08x} text={pre_text!r}")

    if not pre_ok:
        report("T8 cross-reset persistence", False, "skipped (T7 failed)")
        return

    # Reset target via JLink.
    reset_target()

    # Read back AFTER reset.
    post_buf = ik.read_mem(DEVICE, DRAFT_PARTITION, 64)
    post_magic, post_id, post_len = struct.unpack_from('<IIH', post_buf, 0)
    post_text = post_buf[16:16 + post_len].decode('utf-8', errors='replace')

    post_ok = (post_magic == DRAFT_MAGIC and post_id == test_id
               and post_text == test_text)
    report("T8 cross-reset persistence",
           post_ok,
           f"magic=0x{post_magic:08x} id=0x{post_id:08x} text={post_text!r}")

    # Cleanup.
    flash_erase_draft_partition()


# ── Main ───────────────────────────────────────────────────────────────

def main():
    print(f"ELF: {ELF}")
    print(f"Device: {DEVICE}")

    t1_boot_view()
    t2_func_short_to_launcher()
    t3_ok_launches_messages()
    t4_back_cancels_modal()
    t5_func_long_press()
    t6_router_runs_no_fault()
    t7_8_draft_persistence()

    print("\n=== Summary ===")
    failed = [r for r in results if not r[1]]
    for n, ok, det in results:
        mark = PASS if ok else FAIL
        print(f"  [{mark}] {n}")
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    sys.exit(0 if not failed else 1)


if __name__ == '__main__':
    main()
