"""measure_c0_flash.py — Core 0 flash op duty-cycle profiler.

Reads `g_c0_flash_stats` from Core 0's SRAM via SWD (J-Link target
RP2350_M33_0). The struct is bumped inside __wrap_flash_range_erase /
__wrap_flash_range_program in Core 0's flash_safety_wrap.c.

Modes:
  poll           One-shot snapshot — print struct contents, exit.
  baseline N     Sample for N seconds at SAMPLE_INTERVAL_S, compute
                 ops/min + duty cycle in the window. Useful for
                 characterising idle / steady-state behaviour.
  trigger CMD    Snapshot before, run host CMD, snapshot after.
                 CMD is run as a shell command (e.g. "python -m meshtastic
                 --port COMxx --info"). Reports ops + total_us delta.

Usage:
    python scripts/measure_c0_flash.py poll
    python scripts/measure_c0_flash.py baseline 60
    python scripts/measure_c0_flash.py baseline 600
    python scripts/measure_c0_flash.py trigger "python -m meshtastic --port COM21 --set device.role ROUTER"

The struct layout MUST match flash_safety_wrap.c:
    offset  field
    ─────────────────────────
    0x00    magic               'CFLS' (0x534C4643)
    0x04    erase_count
    0x08    program_count
    0x0C    total_us            (32-bit, wraps every ~71 min)
    0x10    last_dur_us
    0x14    last_offset
    0x18    last_count
    0x1C    last_kind           (0=erase, 1=program)
    0x20    last_op_timestamp_us
    0x24    boot_us
"""
import glob
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

SAMPLE_INTERVAL_S = 2.0
C0_DEVICE = "RP2350_M33_0"


def _resolve_c0_elf() -> str:
    """Pick the most recently modified Core 0 ELF in the PlatformIO output."""
    pat = "firmware/core0/meshtastic/.pio/build/rp2350b-mokya/firmware-*.elf"
    candidates = sorted(glob.glob(pat), key=lambda p: Path(p).stat().st_mtime,
                        reverse=True)
    if not candidates:
        raise SystemExit(f"no Core 0 ELF found at {pat}")
    return candidates[0]


def _open_c0() -> MokyaSwd:
    elf = _resolve_c0_elf()
    print(f"[swd] elf={elf}")
    swd = MokyaSwd(device=C0_DEVICE, elf=elf)
    swd.open()
    return swd


def read_stats(swd: MokyaSwd) -> dict:
    addr = swd.symbol("g_c0_flash_stats")
    raw = swd.read_mem(addr, 0x28)
    (magic, erase, program, total_us, last_dur, last_off,
     last_cnt, last_kind, last_ts, boot_us) = struct.unpack(
         "<10I", bytes(raw))
    return dict(
        magic=magic, erase=erase, program=program, total_us=total_us,
        last_dur=last_dur, last_off=last_off, last_cnt=last_cnt,
        last_kind=last_kind, last_ts=last_ts, boot_us=boot_us,
    )


def fmt_stats(s: dict) -> str:
    kind = "erase" if s['last_kind'] == 0 else "program"
    return (
        f"  magic    : 0x{s['magic']:08X}  "
        f"({'CFLS' if s['magic'] == 0x534C4643 else '!! BAD !!'})\n"
        f"  erase    : {s['erase']}\n"
        f"  program  : {s['program']}\n"
        f"  total_us : {s['total_us']}  ({s['total_us']/1e3:.2f} ms)\n"
        f"  last     : kind={kind}  off=0x{s['last_off']:08X}  "
        f"count={s['last_cnt']}  dur={s['last_dur']/1e3:.2f} ms\n"
        f"  last_ts  : {s['last_ts']/1e6:.2f} s (timerawl)\n"
        f"  boot_us  : {s['boot_us']/1e6:.2f} s (timerawl of first op)"
    )


def cmd_poll():
    swd = _open_c0()
    try:
        s = read_stats(swd)
        print(fmt_stats(s))
    finally:
        swd.close()


def cmd_baseline(window_s: float):
    """Sample stats every SAMPLE_INTERVAL_S for window_s, report duty cycle."""
    swd = _open_c0()
    try:
        s0 = read_stats(swd)
        if s0['magic'] != 0x534C4643:
            raise SystemExit(f"bad magic 0x{s0['magic']:08X} — wrong build?")
        print(f"[start] {s0['erase']} erases, {s0['program']} programs, "
              f"total_us={s0['total_us']}")
        print(f"[window] sampling for {window_s:.0f} s "
              f"(every {SAMPLE_INTERVAL_S} s)\n")

        t_start = time.time()
        per_window = []
        last_e, last_p, last_us = s0['erase'], s0['program'], s0['total_us']
        while True:
            elapsed = time.time() - t_start
            if elapsed >= window_s:
                break
            time.sleep(SAMPLE_INTERVAL_S)
            s = read_stats(swd)
            de = s['erase'] - last_e
            dp = s['program'] - last_p
            dus = s['total_us'] - last_us  # 32-bit wrap-safe in Python int
            dus &= 0xFFFFFFFF
            if de or dp:
                tag = "erase" if s['last_kind'] == 0 else "program"
                print(f"  [{elapsed:6.1f}s] +e={de:2d} +p={dp:2d} "
                      f"+us={dus:8d} ({dus/1e3:7.2f} ms)  "
                      f"last:{tag}@0x{s['last_off']:08X} cnt={s['last_cnt']}")
            per_window.append((elapsed, de, dp, dus))
            last_e, last_p, last_us = s['erase'], s['program'], s['total_us']

        s1 = read_stats(swd)
        total_e = s1['erase'] - s0['erase']
        total_p = s1['program'] - s0['program']
        total_us = (s1['total_us'] - s0['total_us']) & 0xFFFFFFFF

        wall_us = window_s * 1_000_000
        duty_pct = 100.0 * total_us / wall_us if wall_us > 0 else 0.0

        print()
        print("=" * 60)
        print(f"window:        {window_s:.1f} s")
        print(f"erase ops:     {total_e}")
        print(f"program ops:   {total_p}")
        print(f"total ops:     {total_e + total_p}")
        print(f"total flash us:{total_us}  ({total_us/1e3:.2f} ms)")
        print(f"duty cycle:    {duty_pct:.4f} %")
        ops_per_min = (total_e + total_p) * 60.0 / window_s if window_s else 0
        print(f"ops/min:       {ops_per_min:.2f}")
        if total_e + total_p > 0:
            avg_us = total_us / (total_e + total_p)
            print(f"avg op dur:    {avg_us:.0f} us ({avg_us/1e3:.2f} ms)")
        print("=" * 60)
    finally:
        swd.close()


def cmd_trigger(host_cmd: str):
    """Snapshot → run host_cmd → snapshot → diff."""
    swd = _open_c0()
    try:
        s0 = read_stats(swd)
        print(f"[before] erase={s0['erase']} program={s0['program']} "
              f"total_us={s0['total_us']}")
    finally:
        swd.close()

    print(f"[run] {host_cmd}")
    rc = subprocess.run(host_cmd, shell=True, capture_output=False)
    print(f"[run] rc={rc.returncode}")

    # Allow a few seconds for any deferred flash flush.
    time.sleep(3.0)

    swd = _open_c0()
    try:
        s1 = read_stats(swd)
        de = s1['erase'] - s0['erase']
        dp = s1['program'] - s0['program']
        dus = (s1['total_us'] - s0['total_us']) & 0xFFFFFFFF
        print(f"[after]  erase={s1['erase']} program={s1['program']} "
              f"total_us={s1['total_us']}")
        print()
        print(f"==> +erase={de} +program={dp} +flash_us={dus} "
              f"({dus/1e3:.2f} ms)")
        if s1['last_kind'] == 0:
            kind = "erase"
        else:
            kind = "program"
        print(f"    last op: {kind} @ 0x{s1['last_off']:08X} "
              f"count={s1['last_cnt']} dur={s1['last_dur']/1e3:.2f} ms")
    finally:
        swd.close()


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    mode = sys.argv[1]
    if mode == "poll":
        cmd_poll()
    elif mode == "baseline":
        if len(sys.argv) < 3:
            print("usage: measure_c0_flash.py baseline <seconds>"); sys.exit(1)
        cmd_baseline(float(sys.argv[2]))
    elif mode == "trigger":
        if len(sys.argv) < 3:
            print("usage: measure_c0_flash.py trigger <shell-cmd>"); sys.exit(1)
        cmd_trigger(" ".join(sys.argv[2:]))
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
