#!/usr/bin/env python3
"""stress_p0_2.py — try to reliably reproduce the Core 1 HardFault.

Hypothesis (from postmortem clues):
- bridge_task's call into mokya_pm_test_poll() is on the hot path
  (every loop iter). LR captured at fault matches the instruction
  after the bl mokya_pm_test_poll, but PC lands inside _DoInit at
  0x1020EBEE — clearly a corrupted PC, not a genuine UDF#0 trigger
  via g_mokya_pm_test_force_fault.
- The user's earlier note tied repros to "SWD inject + IME compose
  高負載", boot+27/67/98s windows.

This harness drives that load pattern continuously and polls the
postmortem magic every iteration. On capture (magic=0xDEADC0DE) it
dumps the full slot + stack snapshot to stdout and exits.

Run alongside the build:
    python scripts/stress_p0_2.py

Stop with Ctrl+C if it hasn't repro'd in N iterations.
"""
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import inject_keys as ik  # noqa: E402

ELF    = str(ROOT / 'build' / 'core1_bridge' / 'core1_bridge.elf')
DEVICE = 'RP2350_M33_1'
KEY    = ik.load_keycode_map()

# Postmortem region addresses (per ipc_shared_layout.h after the 64-word
# stack snapshot bump). Keep these in sync with sizeof(mokya_postmortem_t)
# = 384 B, two slots = 768 B at the tail of g_ipc_shared.
PM_C0 = 0x2007FB00
PM_C1 = 0x2007FC80
PM_MAGIC_OK = 0xDEADC0DE
PM_SLOT_BYTES = 384
PM_STACK_OFFSET = 0x80
PM_STACK_WORDS_MAX = 64

PEER_PORT = 'COM7'
MOKYA_NODE_ID = '!b15db862'

CAUSE_NAMES = {
    0: 'NONE', 1: 'WD_SILENT', 2: 'HARDFAULT', 3: 'MEMMANAGE',
    4: 'BUSFAULT', 5: 'USAGEFAULT', 6: 'PANIC', 7: 'GRACEFUL_REBOOT',
}


def jlink_read(addr, n_bytes):
    """Read n_bytes from addr via a one-shot J-Link Commander session."""
    with tempfile.NamedTemporaryFile('w', suffix='.jlink', delete=False,
                                     encoding='ascii') as f:
        f.write(f'connect\nmem8 0x{addr:08X} {n_bytes}\nqc\n')
        path = f.name
    try:
        r = subprocess.run(
            [ik.JLINK, '-device', DEVICE, '-if', 'SWD', '-speed', '4000',
             '-autoconnect', '1', '-CommanderScript', path],
            capture_output=True, text=True, timeout=15)
        return ik.parse_mem_block(r.stdout, addr, n_bytes)
    finally:
        Path(path).unlink(missing_ok=True)


def read_pm_magic(slot_addr):
    b = jlink_read(slot_addr, 4)
    return struct.unpack('<I', b)[0] if len(b) == 4 else 0


def clear_pm_magic(slot_addr):
    """Force pm magic to 0 — for fresh-baseline starts."""
    with tempfile.NamedTemporaryFile('w', suffix='.jlink', delete=False,
                                     encoding='ascii') as f:
        f.write(f'connect\nw4 0x{slot_addr:08X} 0\nqc\n')
        path = f.name
    try:
        subprocess.run([ik.JLINK, '-device', DEVICE, '-if', 'SWD',
                        '-speed', '4000', '-autoconnect', '1',
                        '-CommanderScript', path],
                       capture_output=True, text=True, timeout=15)
    finally:
        Path(path).unlink(missing_ok=True)


def dump_pm(slot_addr, label):
    blob = jlink_read(slot_addr, PM_SLOT_BYTES)
    if len(blob) < PM_SLOT_BYTES:
        print(f"  {label}: short read ({len(blob)} B)")
        return
    magic, cause, ts_us = struct.unpack_from('<III', blob, 0)
    core = blob[0x0C]
    stack_words = struct.unpack_from('<H', blob, 0x0E)[0]
    pc, lr, sp, psr = struct.unpack_from('<IIII', blob, 0x10)
    r0, r1, r2, r3 = struct.unpack_from('<IIII', blob, 0x20)
    r12, exc_ret, cfsr, hfsr = struct.unpack_from('<IIII', blob, 0x30)
    mmfar, bfar, hb, wd_state = struct.unpack_from('<IIII', blob, 0x40)
    silent_max, wd_pause = struct.unpack_from('<II', blob, 0x50)
    task_name = blob[0x60:0x70].split(b'\x00', 1)[0].decode('ascii', 'replace')
    print(f"  {label}: magic=0x{magic:08X} cause={CAUSE_NAMES.get(cause, cause)} "
          f"t={ts_us}us core={core} task={task_name!r}")
    print(f"    pc=0x{pc:08X} lr=0x{lr:08X} sp=0x{sp:08X} psr=0x{psr:08X}")
    print(f"    cfsr=0x{cfsr:08X} hfsr=0x{hfsr:08X} mmfar=0x{mmfar:08X} bfar=0x{bfar:08X}")
    print(f"    r0..3=0x{r0:08X} 0x{r1:08X} 0x{r2:08X} 0x{r3:08X}  r12=0x{r12:08X} exc_ret=0x{exc_ret:08X}")
    print(f"    c0_hb={hb} silent_max={silent_max} wd_pause={wd_pause}")
    if stack_words > 0:
        # When EXC_RETURN.FType (bit 4) = 0 the CPU stacked an extended
        # frame: words 0..7 = basic, 8..23 = S0..S15, 24 = FPSCR, 25 =
        # reserved, 26+ = real caller stack. Annotate so the back-trace
        # is unambiguous.
        ftype_bit = (exc_ret >> 4) & 1
        extended  = (ftype_bit == 0)
        print(f"    stack ({stack_words} words, exc_ret.FType={ftype_bit}, "
              f"frame={'extended' if extended else 'basic'}):")
        for i in range(0, stack_words, 4):
            row = struct.unpack_from(f'<{min(4, stack_words - i)}I',
                                     blob, PM_STACK_OFFSET + i * 4)
            words = '  '.join(f'{w:08X}' for w in row)
            tag = ''
            if i == 0:                   tag = '  ; basic frame: r0..r3'
            elif i == 4:                 tag = '  ; basic frame: r12,lr,pc,psr'
            elif extended and i == 8:    tag = '  ; S0..S3'
            elif extended and i == 12:   tag = '  ; S4..S7'
            elif extended and i == 16:   tag = '  ; S8..S11'
            elif extended and i == 20:   tag = '  ; S12..S15'
            elif extended and i == 24:   tag = '  ; FPSCR, reserved'
            elif (extended and i >= 28) or (not extended and i >= 8):
                tag = '  ; caller stack'
            print(f"      +0x{i*4:02X}: {words}{tag}")


def peer_send(text):
    """Fire a peer→Mokya DM. Best-effort, ignore output.

    capture_output without explicit encoding inherits cp950 on Windows
    and chokes on UTF-8 bytes from meshtastic CLI; force binary capture
    so decode never runs."""
    try:
        subprocess.run(['python', '-m', 'meshtastic', '--port', PEER_PORT,
                        '--sendtext', text, '--dest', MOKYA_NODE_ID],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=15)
    except subprocess.TimeoutExpired:
        pass
    except Exception as e:
        print(f"  peer_send error (continuing): {e!r}")


def cascade_send_via_ui():
    """Drive the full UI: navigate to compose IME and SET to send.

    Assumes target is already in MESSAGES_CHAT (typical after first iter).
    For the first iter, we navigate from BOOT_HOME via FUNC + OK + OK + OK.
    """
    a = ik.buf_addrs(ELF)
    s_active_addr = a['s_active']
    active = ik.read_u32(DEVICE, s_active_addr)
    # Navigate based on current view
    seq = []
    if active == 0:        # BOOT_HOME
        seq += [KEY['KEY_FUNC'], KEY['KEY_OK'],   # → MESSAGES
                KEY['KEY_OK'], KEY['KEY_OK']]     # → CHAT → IME modal
    elif active == 2:      # MESSAGES (chat list)
        seq += [KEY['KEY_OK'], KEY['KEY_OK']]
    elif active == 3:      # MESSAGES_CHAT
        seq += [KEY['KEY_OK']]
    elif active == 7:      # IME modal already open — type + send
        pass
    else:
        # Unknown — try FUNC to bail out
        seq += [KEY['KEY_FUNC'], KEY['KEY_BACK']]
    # Type + commit + send
    seq += [KEY['KEY_SPACE'], KEY['KEY_SET']]
    ik.queue_events(DEVICE, ELF, sum(([(0x80 | k, 0), (k, 0)] for k in seq), []))
    time.sleep(0.5)


def reset_target():
    """SYSRESETREQ via JLink so each round starts in the boot window
    where the historical P0-2 occurrences clustered (boot+27/67/98s).
    Stress-during-cold-boot is the highest-value pattern to test."""
    with tempfile.NamedTemporaryFile('w', suffix='.jlink', delete=False,
                                     encoding='ascii') as f:
        f.write('connect\nr\ng\nqc\n')
        path = f.name
    try:
        subprocess.run([ik.JLINK, '-device', DEVICE, '-if', 'SWD',
                        '-speed', '4000', '-autoconnect', '1',
                        '-CommanderScript', path],
                       capture_output=True, text=True, timeout=15)
    finally:
        Path(path).unlink(missing_ok=True)


def main():
    iters = 0
    rounds = 0
    print(f"stress_p0_2 — RESET-then-stress loop. Ctrl+C to stop.")
    print(f"Polling PM_C0=0x{PM_C0:08X} / PM_C1=0x{PM_C1:08X} for magic 0x{PM_MAGIC_OK:08X}.")
    print(f"Each round: SYSRESETREQ, wait 3s for boot, then stress until either")
    print(f"30 iters elapsed or a fault is captured.")
    print("=" * 60)

    # Don't clear magic at start — if there's an unsurfaced fault from a
    # previous run, we want to see it.
    initial_c0 = read_pm_magic(PM_C0)
    initial_c1 = read_pm_magic(PM_C1)
    if initial_c0 == PM_MAGIC_OK or initial_c1 == PM_MAGIC_OK:
        print("PRE-EXISTING POSTMORTEM:")
        if initial_c0 == PM_MAGIC_OK:
            dump_pm(PM_C0, 'c0')
        if initial_c1 == PM_MAGIC_OK:
            dump_pm(PM_C1, 'c1')
        print("Clearing and continuing stress...")
        clear_pm_magic(PM_C0)
        clear_pm_magic(PM_C1)

    try:
      while True:
        rounds += 1
        print(f"\n--- ROUND {rounds}: reset target ---")
        reset_target()
        time.sleep(3.0)
        # Clear magic in case the prior round captured one we already saw
        clear_pm_magic(PM_C0); clear_pm_magic(PM_C1)
        round_iters = 0
        round_t0 = time.time()
        while round_iters < 30:
            iters += 1; round_iters += 1
            t0 = time.time()
            # Aggressive SWD churn: many small-batch inject reads/writes
            # and cascade compose ops. The hypothesis is the fault correlates
            # with SWD halt/resume cycles colliding with FreeRTOS scheduler
            # state. So we deliberately pile on JLink Commander sessions.
            #
            # Burst A: send peer→Mokya inbound (drives RX path)
            peer_send(f"s{iters}_{int(t0)}")

            # Burst B: 20 SWD-poll cycles of `s_view_router_active` —
            # each one halts the CPU briefly. Mirrors the "SWD inject"
            # load profile from the original repros.
            for _ in range(20):
                try:
                    ik.read_u32(DEVICE, ik.buf_addrs(ELF)['s_active'])
                except Exception:
                    pass

            # Burst C: drive cascade compose — uses queue_events to
            # write the ring buffer (more SWD halts). Mid-iter to
            # interleave with rx_text arrival.
            cascade_send_via_ui()

            # Burst D: rapid back/forth nav with SWD reads in between
            for kc in [KEY['KEY_BACK'], KEY['KEY_FUNC'], KEY['KEY_BACK'],
                       KEY['KEY_FUNC'], KEY['KEY_OK']]:
                ik.queue_events(DEVICE, ELF, [(0x80 | kc, 0), (kc, 0)])
                # Quick SWD read between keys
                try:
                    ik.read_u32(DEVICE, ik.buf_addrs(ELF)['inject_pushed'])
                except Exception:
                    pass

            # Poll postmortems every iter
            c0_m = read_pm_magic(PM_C0)
            c1_m = read_pm_magic(PM_C1)
            elapsed = time.time() - t0
            if c0_m == PM_MAGIC_OK or c1_m == PM_MAGIC_OK:
                round_elapsed = time.time() - round_t0
                print(f"\n!!! REPRO at iter={iters} (round {rounds}, "
                      f"round_elapsed={round_elapsed:.1f}s, iter_elapsed={elapsed:.1f}s) !!!")
                if c0_m == PM_MAGIC_OK: dump_pm(PM_C0, 'c0')
                if c1_m == PM_MAGIC_OK: dump_pm(PM_C1, 'c1')
                return 0
            print(f"  R{rounds:2d}.iter={round_iters:3d} (total {iters:4d}) "
                  f"elapsed={elapsed:5.1f}s  c0=0x{c0_m:08X}  c1=0x{c1_m:08X}")
    except KeyboardInterrupt:
        print(f"\nstopped after {iters} iters / {rounds} rounds with no repro")
        return 1


if __name__ == '__main__':
    sys.exit(main())
