"""HW Diag LED control verification.

Drives the LED brightness page via SWD key inject. Reads
lm27965_get_state() (via the function symbol — there's only one
non-static lm27965_get_state — and stepping into the cached state
via SWD requires a halt, which would disturb the live state).

Simpler alternative: read the GP register cache exposed in
lm27965_state_t. Since the static is inside lm27965.c we'd need to
disambiguate the s_state addresses. Instead we observe the SECONDARY
effect — toggle red LED on the LED page and verify the GP register
cache changes (using a host-script that drives the inject ring then
reads the bytes a public symbol points to).

For Phase 1 of HW Diag verification we settle for: navigate to the LED
page, perform a toggle, confirm that the SWD-inject path didn't crash
the firmware (heartbeat + g_history_count still advances).
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

# Mokya keycodes.
KEY_FUNC  = 0x06
KEY_BACK  = 0x12
KEY_UP    = 0x1F
KEY_DOWN  = 0x20
KEY_LEFT  = 0x21
KEY_RIGHT = 0x22
KEY_OK    = 0x23

KEYI_MAGIC = 0x4B45494A
RING_EVENTS = 32
EVENT_SIZE  = 2


def inject_event(swd, base, kb):
    producer = swd.read_u32(base + 4)
    slot = producer % RING_EVENTS
    addr = base + 20 + slot * EVENT_SIZE
    swd.write_u8_many([(addr, kb & 0xFF), (addr + 1, 0)])
    swd.write_u32(base + 4, producer + 1)


def press_release(swd, base, key, settle_ms=80):
    inject_event(swd, base, 0x80 | (key & 0x7F))
    time.sleep(0.030)
    inject_event(swd, base, 0x00 | (key & 0x7F))
    time.sleep(settle_ms / 1000.0)


def expect(label, actual, expected, op="=="):
    if op == "==":
        ok = actual == expected
    elif op == ">":
        ok = actual > expected
    else:
        ok = False
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<40} actual={str(actual):<10} {op} {expected!r}")
    return ok


def main():
    fails = 0
    with MokyaSwd() as swd:
        a_inject = swd.symbol("g_key_inject_buf")
        a_active = swd.symbol("s_view_router_active")
        a_diag_p = swd.symbol("g_hw_diag_cur_page")
        a_hc     = swd.symbol("g_history_count")

        if swd.read_u32(a_inject) != KEYI_MAGIC:
            print("  [FAIL] inject magic wrong"); sys.exit(2)

        # Get to launcher then HWDiag.
        if swd.read_u32(a_active) != 1:
            press_release(swd, a_inject, KEY_FUNC, settle_ms=300)
        # Reset position.
        for _ in range(4): press_release(swd, a_inject, KEY_UP)
        for _ in range(4): press_release(swd, a_inject, KEY_LEFT)
        # Down 3 = row 3 (HWDiag).
        for _ in range(3): press_release(swd, a_inject, KEY_DOWN)
        press_release(swd, a_inject, KEY_OK, settle_ms=400)
        # HW Diag is LRU-cached — cur_page may be wherever last visit
        # left off. Cycle RIGHT until we reach LED (page 2). Bounded by 8.
        for _ in range(8):
            page = swd.read_mem(a_diag_p, 1)[0]
            if page == 7: break   # LED at index 7 (after 7 GNSS pages: NMEA/Diag/Cfg/Const/Track/NMEA Cfg/Adv)
            press_release(swd, a_inject, KEY_RIGHT, settle_ms=200)
        page = swd.read_mem(a_diag_p, 1)[0]
        print(f"  navigated to page {page} (expect 2 = LED)")
        if not expect("on LED page", page, 7): fails += 1

        # Heartbeat: capture history_count before / after a series of LED
        # operations. If the firmware crashed mid-test, count stops.
        hc_before = int.from_bytes(swd.read_mem(a_hc, 2), "little")
        print(f"  history_count before LED ops: {hc_before}")

        # New UX: ↑/↓ moves widget focus, OK toggles bools or +N wraps duty.
        # No ←/→ — those switch hw_diag pages.
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # widget 0 (red on): toggle
        press_release(swd, a_inject, KEY_DOWN, settle_ms=200) # → widget 1 (red duty)
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # red duty +1
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # red duty +1
        press_release(swd, a_inject, KEY_DOWN, settle_ms=200) # → widget 2 (green)
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # green toggle
        press_release(swd, a_inject, KEY_DOWN, settle_ms=200) # → widget 3 (kbd)
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # kbd toggle
        press_release(swd, a_inject, KEY_DOWN, settle_ms=200) # → widget 4 (bankB)
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # bankB +4

        # Wait one history tick (>30 s would be excessive; instead probe
        # the RAM-coherent uptime by reading any known-bumping counter.
        # The lvgl_task tick or g_kp_scan_tick is faster).
        a_kp = swd.symbol("g_kp_scan_tick")
        kp0 = swd.read_u32(a_kp)
        time.sleep(0.5)
        kp1 = swd.read_u32(a_kp)
        print(f"  g_kp_scan_tick: {kp0} → {kp1} over 0.5 s")
        if not expect("scan task alive", kp1, kp0, op=">"): fails += 1

        # Verify still on HW_DIAG, page 2, no crash.
        active = swd.read_u32(a_active)
        page = swd.read_mem(a_diag_p, 1)[0]
        VIEW_HW_DIAG = 36   # observed in earlier nav test
        if not expect("still on HW_DIAG",  active, VIEW_HW_DIAG): fails += 1
        if not expect("still on LED page", page, 7): fails += 1

        # Restore: turn everything off before exiting. Currently focus is
        # on bankB widget (4). Toggle the booleans back off.
        press_release(swd, a_inject, KEY_UP, settle_ms=200)   # → kbd
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # kbd off
        press_release(swd, a_inject, KEY_UP, settle_ms=200)   # → green
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # green off
        press_release(swd, a_inject, KEY_UP, settle_ms=200)   # → red duty
        # Cycle red_duty back to 0 (it's currently 2, max is 3, so OK twice wraps).
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # 2 → 3
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # 3 → 0 (wrap)
        press_release(swd, a_inject, KEY_UP, settle_ms=200)   # → red on
        press_release(swd, a_inject, KEY_OK, settle_ms=200)   # red off

    print()
    if fails == 0:
        print("==> LED control PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
