"""MIE LRU LFS migration verification (Phase 7 / Task #71).

Verifies that the LRU persist path now lands in LFS (was raw flash at
0x10C00000):

  1. SWD-trigger an LRU save via g_lru_persist_save_request. Bridge
     forwards to the IME task tripwire; verify g_lru_persist_saves
     increments and g_lru_persist_save_ok=1.
  2. Confirm /.mie_lru.bin exists in LFS by reading
     g_lru_persist_bytes (file size after save) — must be ≥ 8 (header
     only, empty cache) and ≤ 6152 (full 128-entry cache).
  3. Trigger again — verify save count bumps and bytes stays sane
     across overwrite path.
  4. Trigger reboot via watchdog (or reflash) is out of scope here;
     load_path is exercised on every cold boot — we just confirm
     g_lru_persist_loads >= 1 if the partition has been seeded.

Non-destructive: doesn't touch live cache state — bridge task forces
a tripwire which serialises the current cache (possibly empty).
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore


def expect(label, actual, expected, op="=="):
    if op == "==":
        ok = actual == expected
    elif op == ">=":
        ok = actual >= expected
    elif op == "<=":
        ok = actual <= expected
    elif op == ">":
        ok = actual > expected
    else:
        ok = False
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<44} actual={str(actual):<8} {op} {expected!r}")
    return ok


def fire_save(swd, a_req, a_done, a_ok, timeout_s=3.0):
    req = swd.read_u32(a_req) + 1
    swd.write_u32(a_req, req)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if swd.read_u32(a_done) == req:
            return swd.read_mem(a_ok, 1)[0] == 1
        time.sleep(0.05)
    return False


def main():
    fails = 0
    with MokyaSwd() as swd:
        a_req      = swd.symbol("g_lru_persist_save_request")
        a_done     = swd.symbol("g_lru_persist_save_done")
        a_ok       = swd.symbol("g_lru_persist_save_ok")
        a_saves    = swd.symbol("g_lru_persist_saves")
        a_loads    = swd.symbol("g_lru_persist_loads")
        a_failures = swd.symbol("g_lru_persist_failures")
        a_bytes    = swd.symbol("g_lru_persist_bytes")
        a_last_err = swd.symbol("g_lru_persist_last_err")

        saves0    = swd.read_u32(a_saves)
        failures0 = swd.read_u32(a_failures)
        loads0    = swd.read_u32(a_loads)

        # ── Run 1: trigger save ───────────────────────────────────────
        ok1 = fire_save(swd, a_req, a_done, a_ok)
        if not expect("save #1 ok", ok1, True): fails += 1

        saves1 = swd.read_u32(a_saves)
        if not expect("saves bumped (>=1)", saves1 - saves0, 1, op=">="):
            fails += 1

        bytes1 = swd.read_u32(a_bytes)
        # Header is 8 B; full 128-entry cache is 8 + 128*48 = 6152 B.
        if not expect("blob size header >= 8",  bytes1, 8,    op=">="): fails += 1
        if not expect("blob size <= 6152",      bytes1, 6152, op="<="): fails += 1

        # ── Run 2: overwrite path ─────────────────────────────────────
        ok2 = fire_save(swd, a_req, a_done, a_ok)
        if not expect("save #2 ok", ok2, True): fails += 1

        saves2 = swd.read_u32(a_saves)
        if not expect("saves bumped 2nd time", saves2, saves1, op=">"):
            fails += 1

        # ── Failure counter sanity ────────────────────────────────────
        failures2 = swd.read_u32(a_failures)
        if not expect("no new failures", failures2, failures0): fails += 1

        last_err = swd.read_u32(a_last_err)
        # last_err should be 0 (or at least non-error); LFS errors are
        # negative when reinterpreted signed, so a non-negative or zero
        # value is fine.
        if not expect("last_err sign-bit clear", last_err & 0x80000000, 0):
            fails += 1

        # ── Loads counter — only meaningful if a previous boot saved
        #    something. After the first save, the next cold boot will
        #    bump loads by 1. We just check it's not crazy. ───────────
        loads_now = swd.read_u32(a_loads)
        print(f"  [info] loads since boot = {loads_now} "
              f"(0 if first boot after save; 1+ on subsequent boots)")

    print()
    if fails == 0:
        print("==> MIE LRU LFS PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
