#!/usr/bin/env python3
"""test_lru_regression.py -- Phase 1.6 LRU promotion regression.

Runs ime_text_test.py twice on a passage. Pass 1 warms the LRU cache;
pass 2 exercises the same inputs and must show a rank-histogram shift
toward rank 0. Optionally reboots the board between passes to
demonstrate flash persistence.

Usage:
    python scripts/test_lru_regression.py PASSAGE [--reboot] [--erase]
                                          [--limit N] [--v4]

Pass-2 expectations (fail test if not met):
    - pass2.rank_hist[0] >= pass1.rank_hist[0]              (at least equal)
    - At least N=10 chars that were rank >= 8 in pass 1
      are rank <= 3 in pass 2.

Assumptions:
    - Core 1 + Core 0 already flashed with latest build.
    - Board enumerates as a Meshtastic USB CDC.
    - User has patched LRU persistence already (Phase 1.6 Step 3).

SPDX-License-Identifier: MIT
"""

import argparse
import ast
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

JLINK = r"C:/Program Files/SEGGER/JLink_V932/JLink.exe"

LRU_PARTITION_ADDR = 0x10C00000
LRU_PARTITION_SIZE = 0x10000  # 64 KB reserved
LRU_MAGIC          = 0x3155524C  # 'LRU1' little-endian

# Known boot artifacts used to sanity-check the board state after SWD reset.
CORE0_FLASH_BASE   = 0x10000000   # Meshtastic image
CORE1_FLASH_BASE   = 0x10200000   # Bridge image
BREADCRUMB_ADDR    = 0x20078010   # Core 0 boot phase breadcrumb
BOOTROM_PC_MAX     = 0x00008000   # Cortex-M33 bootrom lives well below here
XIP_BASE           = 0x10000000

REG_PC   = 15
REG_LR   = 14


def run_jlink(script_lines, device="RP2350_M33_0"):
    """Run a J-Link Commander script. Returns stdout as str. Used only for
    flash-range erase (pylink does not expose range-erase cleanly)."""
    script = "\n".join(script_lines) + "\nqc\n"
    script_path = Path("/tmp/lru_reg_jlink.jlink")
    script_path.write_text(script)
    proc = subprocess.run(
        [JLINK, "-device", device, "-if", "SWD", "-speed", "4000",
         "-autoconnect", "1",
         "-CommanderScript", str(script_path.resolve())],
        capture_output=True, text=True, timeout=60)
    return proc.stdout


# ── state dump helpers ─────────────────────────────────────────────────

def _safe_read_u32(swd, addr):
    try:
        return swd.read_u32(addr)
    except Exception as e:
        return f"ERR:{e.__class__.__name__}"


def dump_state(tag, halt_for_pc=True):
    """Read and print board state via a short-lived pylink session.
    halt_for_pc=True halts the core briefly to sample PC / LR then resumes.
    Always closes the connection so subsequent tools can open their own."""
    print(f"[state:{tag}] ---")
    try:
        with MokyaSwd(device="RP2350_M33_0") as swd:
            c0_magic = _safe_read_u32(swd, CORE0_FLASH_BASE + 4)
            c1_magic = _safe_read_u32(swd, CORE1_FLASH_BASE + 4)
            lru_magic = _safe_read_u32(swd, LRU_PARTITION_ADDR)
            breadcrumb = _safe_read_u32(swd, BREADCRUMB_ADDR)
            pc = lr = None
            if halt_for_pc:
                try:
                    swd._jl.halt()
                    pc = (swd._jl.register_read(REG_PC) & 0xFFFFFFFF)
                    lr = swd._jl.register_read(REG_LR) & 0xFFFFFFFF
                finally:
                    try: swd._jl.restart()
                    except Exception: pass
        def _fmt(v):
            return f"0x{v:08X}" if isinstance(v, int) else str(v)
        print(f"[state:{tag}] core0[+4]=   {_fmt(c0_magic)}  "
              f"core1[+4]=   {_fmt(c1_magic)}")
        print(f"[state:{tag}] lru_magic=   {_fmt(lru_magic)}  "
              f"breadcrumb=  {_fmt(breadcrumb)}")
        if pc is not None:
            zone = "BOOTROM" if pc < BOOTROM_PC_MAX else (
                   "XIP" if pc >= XIP_BASE else "SRAM/other")
            print(f"[state:{tag}] pc=          {_fmt(pc)} ({zone})  "
                  f"lr=          {_fmt(lr)}")
    except Exception as e:
        print(f"[state:{tag}] FAILED: {e}")
    print(f"[state:{tag}] ---")


# ── operations ─────────────────────────────────────────────────────────

def erase_lru_partition():
    """J-Link range-erase of the LRU slot. Neighbouring partitions are at
    0x10A00000 (MIEF font, 2 MB) below and 0x10C10000 free above, so a
    range-erase that strictly respects its arguments is safe. We dump the
    core image magic words before and after to catch accidental spill."""
    dump_state("pre-erase", halt_for_pc=False)
    print(f"[erase] erasing LRU partition 0x{LRU_PARTITION_ADDR:08X} "
          f"+ {LRU_PARTITION_SIZE} bytes via J-Link Commander")
    out = run_jlink([
        "connect", "h",
        f"erase 0x{LRU_PARTITION_ADDR:08X} "
        f"0x{LRU_PARTITION_ADDR + LRU_PARTITION_SIZE:08X}",
        "r", "g",
    ])
    time.sleep(3)  # USB CDC re-enumerate
    dump_state("post-erase", halt_for_pc=True)


def wait_for_xip_ready(timeout_s=8.0, poll_s=0.2):
    """Poll a known-valid flash word (Core 0 image vector) until the read
    succeeds. XIP is disabled by flash_safety_wrap.c for the duration of
    any flash erase/program, so a failing read here means a save is still
    in flight. Returns True once readable; raises on timeout.

    Must be called on an entry boundary (e.g. after a test pass) before
    injecting anything into Core 1 — halting Core 1 while it's in a
    flash op leaves Core 0 stuck in the park spin and XIP off, which is
    the exact wedge that kills QMI unrecoverably."""
    deadline = time.time() + timeout_s
    with MokyaSwd(device="RP2350_M33_0") as swd:
        while time.time() < deadline:
            try:
                swd.read_u32(CORE0_FLASH_BASE + 4)
                return True
            except Exception:
                time.sleep(poll_s)
    raise RuntimeError(
        f"XIP never became readable within {timeout_s}s — flash op stuck; "
        "aborting rather than piling on halts (power-cycle required).")


def force_lru_save():
    """Inject a MODE keypress to trip the mode_tripwire throttle so the
    current LruCache is written to flash even when commit count < 50.

    IMPORTANT: do NOT halt Core 1 here. Core 1 runs the key_inject
    consumer and the LRU flash save path. Halting it while it is inside
    flash_range_program leaves XIP off + Core 0 parked forever, which
    wedges QMI unrecoverably (project_qmi_wedge_recovery.md).

    Atomicity w.r.t. the consumer is instead provided by write order:
    event bytes first, producer_idx last. The consumer loads producer_idx
    with acquire semantics and only then reads the event slots, so it
    cannot observe a torn event.

    We also wait for XIP to be readable before injecting, so any
    in-progress save from Pass 1 has fully completed."""
    wait_for_xip_ready()
    with MokyaSwd(device="RP2350_M33_1") as swd:
        base        = swd.symbol("g_key_inject_buf")
        prod_addr   = base + 4
        events_base = base + 20
        RING        = 32                 # KEY_INJECT_RING_EVENTS
        EVSZ        = 2                  # KEY_INJECT_EVENT_SIZE
        # Read current producer_idx and append — NEVER reset it to 0/2.
        # The ring is shared with Pass 1's leftover consumer position, so
        # overwriting producer_idx with a small value would make the
        # consumer race around the ring re-processing ~30 stale events,
        # corrupting IME state and crashing Core 1 mid-flash-write.
        cur_prod = swd.read_u32(prod_addr)
        slot0 = (cur_prod      ) & (RING - 1)
        slot1 = (cur_prod + 1  ) & (RING - 1)
        swd.write_u8_many([
            (events_base + slot0 * EVSZ + 0, 0x99),  # MODE press
            (events_base + slot0 * EVSZ + 1, 0x00),
            (events_base + slot1 * EVSZ + 0, 0x19),  # MODE release
            (events_base + slot1 * EVSZ + 1, 0x00),
        ])
        swd.write_u32(prod_addr, cur_prod + 2)


def wait_for_save_complete(timeout_s=10.0, stable_reads=5, poll_s=0.15):
    """Poll the LRU partition's first word until the LRU1 magic appears
    stably. This is the hand-shake that protects the reboot path from
    the P1.6 QMI-wedge failure mode:

      - While flash_range_program is running, XIP cache is disabled by
        flash_safety_wrap.c; SWD reads of the XIP window throw
        JLinkReadException. We treat any read failure as "save in
        progress, keep waiting".
      - Once the magic word is seen `stable_reads` times in a row we
        consider the write fully flushed and the unpark step complete.
        A SYSRESETREQ at this point is safe.

    Without this, a 2 s sleep sometimes races the actual write and the
    reset lands mid-program — which leaves QMI in an unrecoverable state
    that survives chip erase (see project_qmi_wedge_recovery memory).

    Returns True on confirmed stable magic, False on timeout.
    """
    deadline = time.time() + timeout_s
    stable = 0
    last_val = None
    with MokyaSwd(device="RP2350_M33_0") as swd:
        while time.time() < deadline:
            try:
                v = swd.read_u32(LRU_PARTITION_ADDR)
                last_val = v
            except Exception as e:
                # XIP disabled during write, or transient SWD hiccup.
                stable = 0
                last_val = f"ERR:{e.__class__.__name__}"
                time.sleep(poll_s)
                continue
            if v == LRU_MAGIC:
                stable += 1
                if stable >= stable_reads:
                    print(f"[save-wait] magic stable ({stable} reads) — "
                          f"save complete")
                    return True
            else:
                stable = 0
            time.sleep(poll_s)
    print(f"[save-wait] TIMEOUT after {timeout_s}s; last value = "
          f"{last_val!r}")
    return False


def reboot_and_verify(max_wait_s=8.0):
    """Reset the target via pylink and wait until flash (XIP) is
    readable AND matches the expected Core 0 image vector. We do NOT
    sample PC by halt/restart: on RP2350 with Security Extension,
    halting mid-boot-sequence can stall the chip in odd states (we've
    seen EXC_RETURN-like PC values that defeat any "is it in bootrom?"
    check). Flash readability is sufficient — if Core 0 is wedged in a
    RAM park spin, XIP stays off and reads fail. If Core 0 is in
    bootrom, XIP is on and reads return the boot image. Either way a
    stable readable image means the chip is healthy post-reset; Core 1
    liveness is implicitly validated when Pass 2 runs ime_text_test.py."""
    print("[reboot] pylink reset (halt=False)")
    with MokyaSwd(device="RP2350_M33_0") as swd:
        swd._jl.reset(ms=0, halt=False)
        deadline = time.time() + max_wait_s
        last_err = None
        while time.time() < deadline:
            time.sleep(0.4)
            try:
                c0 = swd.read_u32(CORE0_FLASH_BASE + 4)
                c1 = swd.read_u32(CORE1_FLASH_BASE + 4)
                if c0 != 0 and c0 != 0xFFFFFFFF:
                    print(f"[reboot] flash readable: core0[+4]=0x{c0:08X} "
                          f"core1[+4]=0x{c1:08X}")
                    break
            except Exception as e:
                last_err = e
        else:
            raise RuntimeError(
                f"Flash never became readable within {max_wait_s}s post-"
                f"reset (last err: {last_err}). QMI may be wedged — "
                "physical USB re-plug required.")
    # USB CDC re-enumerate + give Core 1 time to be launched by Core 0.
    time.sleep(5)
    dump_state("post-reboot", halt_for_pc=False)


def read_lru_magic():
    try:
        with MokyaSwd(device="RP2350_M33_0") as swd:
            return f"{swd.read_u32(LRU_PARTITION_ADDR):08X}"
    except Exception as e:
        return f"ERR:{e}"


def run_pass(passage, limit, v4):
    """Invoke ime_text_test.py and parse its rank-histogram output."""
    cmd = [sys.executable, "scripts/ime_text_test.py", str(passage),
           "--user-sim"]
    if limit:
        cmd += ["--limit", str(limit)]
    print(f"[pass] {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=600)
    out = (proc.stdout or "") + (proc.stderr or "")
    # Windows console is cp950 by default — strip to ASCII-safe for the
    # log tail print so CJK chars in a ime_text_test summary don't crash
    # the aggregator.
    tail = out[-2000:]
    try:
        print(tail)
    except UnicodeEncodeError:
        print(tail.encode("ascii", "replace").decode("ascii"))
    m = re.search(r'rank hist:\s*(\{[^}]*\})', out)
    if not m:
        raise RuntimeError("ime_text_test.py did not emit a rank histogram")
    return ast.literal_eval(m.group(1))


def summarise(hist):
    total = sum(hist.values())
    r0    = hist.get(0, 0)
    le3   = sum(v for k, v in hist.items() if k <= 3)
    ge8   = sum(v for k, v in hist.items() if k >= 8)
    return total, r0, le3, ge8


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("passage", type=Path)
    ap.add_argument("--erase", action="store_true",
                    help="erase LRU flash partition before pass 1")
    ap.add_argument("--reboot", action="store_true",
                    help="SWD reboot between pass 1 and pass 2")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--v4", action="store_true",
                    help="(documentation only — dict already flashed)")
    args = ap.parse_args()

    if not args.passage.exists():
        sys.exit(f"passage not found: {args.passage}")

    if args.erase:
        erase_lru_partition()
        print(f"[erase] post-erase first word = {read_lru_magic()} "
              f"(expected FFFFFFFF)")

    print("\n=== Pass 1 (cold cache) ===")
    hist1 = run_pass(args.passage, args.limit, args.v4)
    t1, r01, le31, ge81 = summarise(hist1)

    if args.reboot:
        # Force a save so the LRU is flushed despite the 50-commit gate.
        force_lru_save()
        if not wait_for_save_complete(timeout_s=10.0):
            sys.exit("LRU save never completed; aborting before reset to "
                     "avoid wedging QMI mid-program.")
        # Extra settle: guarantees the --wrap unpark has fully run, Core 0
        # is out of park spin, IPC flash_lock_c0 is back to IDLE. Without
        # this, a fast reset can still catch the unpark epilogue.
        time.sleep(0.5)
        reboot_and_verify()

    print("\n=== Pass 2 (warm cache) ===")
    hist2 = run_pass(args.passage, args.limit, args.v4)
    t2, r02, le32, ge82 = summarise(hist2)

    print("\n=== Summary ===")
    print(f"Pass 1: total={t1}  rank0={r01}  rank<=3={le31}  rank>=8={ge81}")
    print(f"Pass 2: total={t2}  rank0={r02}  rank<=3={le32}  rank>=8={ge82}")
    print(f"Delta : rank0 {r02-r01:+d}  rank<=3 {le32-le31:+d}  "
          f"rank>=8 {ge82-ge81:+d}")

    fail = []
    if r02 < r01:
        fail.append(f"pass-2 rank 0 count ({r02}) < pass-1 ({r01})")
    if le32 < le31:
        fail.append(f"pass-2 rank<=3 count ({le32}) < pass-1 ({le31})")
    if (ge81 - ge82) < 10 and ge81 >= 10:
        fail.append(f"fewer than 10 chars promoted out of rank>=8 tail "
                    f"({ge81} → {ge82})")

    if fail:
        print("\nFAIL:")
        for f in fail:
            print(f"  - {f}")
        sys.exit(1)
    print("\nPASS: LRU promotion observed across passes.")


if __name__ == "__main__":
    main()
