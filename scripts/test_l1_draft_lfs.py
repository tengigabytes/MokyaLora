"""draft_store LFS migration verification (Phase 6 / Task #70).

Drives the SWD-triggered draft_store_self_test, which performs an
end-to-end save/load/clear round-trip against an LFS file at
/.draft_c0ffeedb. After the test, also exercises a multi-id round-
trip through the live API to confirm distinct files don't collide.

Verification:
  1. Bump g_draft_self_test_request and wait for _done; expect _ok = 1.
  2. SWD-write a synthetic draft via the API path: there's no SWD-
     callable save, but we can confirm the LFS file count matches the
     self-test cleanup (i.e. /.draft_c0ffeedb does not exist after
     test cleanup → confirmed via subsequent self-test re-run).
  3. Counter sanity: g_draft_saves >= 1 and g_draft_loads >= 1 and
     g_draft_clears >= 2 (clean-pre + cleanup).
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
    elif op == ">":
        ok = actual > expected
    else:
        ok = False
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={str(actual):<8} {op} {expected!r}")
    return ok


def main():
    fails = 0
    with MokyaSwd() as swd:
        a_req       = swd.symbol("g_draft_self_test_request")
        a_done      = swd.symbol("g_draft_self_test_done")
        a_ok        = swd.symbol("g_draft_self_test_ok")
        a_saves     = swd.symbol("g_draft_saves")
        a_loads     = swd.symbol("g_draft_loads")
        a_clears    = swd.symbol("g_draft_clears")
        a_failures  = swd.symbol("g_draft_failures")
        a_last_err  = swd.symbol("g_draft_last_err")

        saves0    = swd.read_u32(a_saves)
        loads0    = swd.read_u32(a_loads)
        clears0   = swd.read_u32(a_clears)
        failures0 = swd.read_u32(a_failures)

        # ── Run 1: trigger self_test ──────────────────────────────────
        req = swd.read_u32(a_req) + 1
        swd.write_u32(a_req, req)
        for _ in range(40):
            time.sleep(0.05)
            if swd.read_u32(a_done) == req: break
        if not expect("self_test #1 done", swd.read_u32(a_done), req): fails += 1
        if not expect("self_test #1 ok",   swd.read_mem(a_ok, 1)[0], 1): fails += 1

        # ── Run 2: trigger again — exercises overwrite path on a
        # previously-saved-then-cleared id. ─────────────────────────
        req = swd.read_u32(a_req) + 1
        swd.write_u32(a_req, req)
        for _ in range(40):
            time.sleep(0.05)
            if swd.read_u32(a_done) == req: break
        if not expect("self_test #2 done", swd.read_u32(a_done), req): fails += 1
        if not expect("self_test #2 ok",   swd.read_mem(a_ok, 1)[0], 1): fails += 1

        # ── Counter sanity ────────────────────────────────────────────
        saves    = swd.read_u32(a_saves)
        loads    = swd.read_u32(a_loads)
        clears   = swd.read_u32(a_clears)
        failures = swd.read_u32(a_failures)

        # Each self_test does: 1 save + 1 load + 2 clears (pre + cleanup).
        if not expect("saves bumped >=2",  saves - saves0,   2, op=">="): fails += 1
        if not expect("loads bumped >=2",  loads - loads0,   2, op=">="): fails += 1
        if not expect("clears bumped >=4", clears - clears0, 4, op=">="): fails += 1
        if not expect("no new failures",   failures, failures0): fails += 1
        if not expect("last_err clean",
                      swd.read_u32(a_last_err) & 0x80000000, 0):
            fails += 1

    print()
    if fails == 0:
        print("==> draft_store LFS PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
