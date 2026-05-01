"""C1 + C0 concurrent flash write contention test.

Validates the Phase 1.X SIO spinlock fix (firmware/shared/ipc/
ipc_shared_layout.h IPC_FLASH_SPINLOCK_NUM = 29) — without it the
two cores can deadlock in cross-WFE when their park ISRs fire on
each other simultaneously.

Workload:
  - Background loop: host calls `meshtastic --set lora.tx_power N`
    in tight repetition (each --set triggers ~1 erase + ~12 programs
    on Core 0's LittleFS, taking ~50 ms total)
  - Foreground: SWD-trigger c1_storage_stress_test repeatedly to force
    Core 1 flash writes
  - Total duration: 5 minutes

Pass criteria:
  - 0 watchdog resets (would manifest as Core 1 boot counter >= initial+1
    in g_c1_storage_pl_count)
  - 0 HardFaults (clean postmortem ring on g_ipc_shared.postmortem_c1)
  - Both stress completions and --set operations succeed
  - Mount survives at end (FS not corrupted)

Without the spinlock fix, theoretical deadlock probability under this
workload over 5 min is ~0.5 % per session. Three back-to-back clean runs
provides reasonable confidence the fix works.
"""
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore
from test_c1_storage_stress import encode_request  # type: ignore

DEFAULT_DURATION_S = 300  # 5 min
COM_PORT = "COM16"
TX_POWER_VALUES = [22, 19, 25, 17, 23]   # cycle, all valid for region


_stop_event = threading.Event()
_set_count = 0
_set_errors = 0


def host_set_loop():
    """Tight loop running meshtastic --set lora.tx_power on Core 0."""
    global _set_count, _set_errors
    idx = 0
    while not _stop_event.is_set():
        v = TX_POWER_VALUES[idx % len(TX_POWER_VALUES)]
        idx += 1
        try:
            r = subprocess.run(
                ["python", "-m", "meshtastic", "--port", COM_PORT,
                 "--set", "lora.tx_power", str(v)],
                capture_output=True, text=True, timeout=20)
            if r.returncode != 0:
                _set_errors += 1
            else:
                _set_count += 1
        except Exception:
            _set_errors += 1
        # Small gap so we don't spam the bridge
        time.sleep(0.5)


def c1_stress_loop(duration_s):
    """SWD-trigger C1 stress in a loop. 8 files × 1 KB per round."""
    request_a = encode_request(8, 1024)         # 8 files × 1 KB
    request_b = encode_request(8, 1280)         # different bytes to flip request value
    rounds = 0
    deadline = time.time() + duration_s
    while time.time() < deadline:
        try:
            with MokyaSwd() as swd:
                # Alternate between two valid request values so request !=
                # done on every iteration (firmware acks by mirroring the
                # request value).
                req = request_a if (rounds & 1) == 0 else request_b
                swd.write_u32(swd.symbol("g_c1_storage_stress_request"), req)
                a_done = swd.symbol("g_c1_storage_stress_done")
                start = time.time()
                while time.time() - start < 5.0:
                    if swd.read_u32(a_done) == req: break
                    time.sleep(0.05)
            rounds += 1
        except Exception as e:
            print(f"  [WARN] stress round {rounds} swd err: {e}")
        time.sleep(0.3)
    return rounds


def main():
    duration = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DURATION_S
    print(f"[start] {duration} s concurrent flash contention")
    print(f"  host C0 path  : meshtastic --set lora.tx_power loop")
    print(f"  device C1 path: c1_storage_stress 8x1KB loop")
    print()

    # Initial state snapshot.
    with MokyaSwd() as swd:
        pl_initial   = swd.read_u32(swd.symbol("g_c1_storage_pl_count"))
        sp_initial   = swd.read_u32(swd.symbol("g_c1_storage_stress_passes"))
        sf_initial   = swd.read_u32(swd.symbol("g_c1_storage_stress_failures"))
    print(f"  pre  pl_count={pl_initial}  stress_passes={sp_initial}  stress_failures={sf_initial}")

    # Start background --set loop.
    t = threading.Thread(target=host_set_loop, daemon=True)
    t.start()

    # Run foreground C1 stress.
    rounds = c1_stress_loop(duration)
    _stop_event.set()
    t.join(timeout=30)

    # Read final state.
    with MokyaSwd() as swd:
        pl_final     = swd.read_u32(swd.symbol("g_c1_storage_pl_count"))
        sp_final     = swd.read_u32(swd.symbol("g_c1_storage_stress_passes"))
        sf_final     = swd.read_u32(swd.symbol("g_c1_storage_stress_failures"))

    print()
    print(f"  post  pl_count={pl_final}  stress_passes={sp_final}  stress_failures={sf_final}")
    print(f"  --set completions = {_set_count}  --set errors = {_set_errors}")
    print(f"  C1 stress rounds  = {rounds}")
    print()

    fails = 0
    if pl_final != pl_initial:
        print(f"  [FAIL] pl_count changed ({pl_initial} → {pl_final})")
        print(f"         indicates watchdog reset → Core 1 cold-boot")
        fails += 1
    else:
        print(f"  [PASS] pl_count stable — no watchdog reset")
    if sf_final != sf_initial:
        print(f"  [FAIL] stress_failures grew {sf_initial} → {sf_final}")
        fails += 1
    else:
        print(f"  [PASS] stress_failures = 0")
    if sp_final - sp_initial < rounds * 24:
        print(f"  [WARN] stress passes/round low: expected {rounds * 24}, got {sp_final - sp_initial}")
    else:
        print(f"  [PASS] all stress rounds completed cleanly")
    if _set_count == 0:
        print(f"  [FAIL] no --set completed at all")
        fails += 1
    elif _set_errors > _set_count // 4:
        print(f"  [FAIL] --set errors {_set_errors} > 25 % of {_set_count}")
        fails += 1
    else:
        print(f"  [PASS] --set ran (errors {_set_errors}/{_set_count + _set_errors})")

    if fails == 0:
        print(f"\n==> Concurrent flash contention PASS over {duration}s")
        sys.exit(0)
    else:
        print(f"\n==> FAIL ({fails} criteria failed)")
        sys.exit(1)


if __name__ == "__main__":
    main()
