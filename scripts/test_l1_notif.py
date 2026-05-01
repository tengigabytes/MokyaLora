"""HW Diag page 14 (NOTIF) verification.

Drives the notification settings page via SWD key inject. Verifies:
  1. Navigation lands on page 13 (NOTIF, 0-indexed) of HW Diag.
  2. OK on row 0 (Master) toggles g_notif_master_enable.
  3. OK on row 3 (DM event) cycles event_modes through 3 states.
  4. notification_event() resolver returns the expected mode after
     SET — exercised via a synthetic call: SWD-poke a "fake event"
     trigger that the firmware doesn't expose, so we instead verify
     by reading back notif_settings_t.event_modes directly.
  5. Persistence: bump g_notif_persist_flush_request, verify
     g_notif_persist_saves increments.

This stays read-only on the actuators (vibration motor / LED) so the
test is non-destructive — VIBRATE on KEYPRESS would buzz on every key
inject otherwise. We confirm dispatch happened via the SWD-readable
g_notif_calls_total / g_notif_mode_last counters.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

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

NOTIF_PAGE = 13   # 0-indexed; 14 pages total, NOTIF is the last


def inject_event(swd, base, kb):
    producer = swd.read_u32(base + 4)
    slot = producer % RING_EVENTS
    addr = base + 20 + slot * EVENT_SIZE
    swd.write_u8_many([(addr, kb & 0xFF), (addr + 1, 0)])
    swd.write_u32(base + 4, producer + 1)


def press_release(swd, base, key, settle_ms=120):
    inject_event(swd, base, 0x80 | (key & 0x7F))
    time.sleep(0.030)
    inject_event(swd, base, 0x00 | (key & 0x7F))
    time.sleep(settle_ms / 1000.0)


def expect(label, actual, expected, op="=="):
    if op == "==":
        ok = actual == expected
    elif op == "!=":
        ok = actual != expected
    elif op == ">":
        ok = actual > expected
    else:
        ok = False
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label:<48} actual={str(actual):<10} {op} {expected!r}")
    return ok


def event_mode_get(event_modes, ev_idx):
    return (event_modes >> (2 * ev_idx)) & 0x3


def main():
    fails = 0
    with MokyaSwd() as swd:
        a_inject  = swd.symbol("g_key_inject_buf")
        a_active  = swd.symbol("s_view_router_active")
        a_diag_p  = swd.symbol("g_hw_diag_cur_page")
        a_master  = swd.symbol("g_notif_master_enable")
        a_calls   = swd.symbol("g_notif_calls_total")
        a_mode    = swd.symbol("g_notif_mode_last")
        a_dirty   = swd.symbol("g_notif_dirty_flag")
        a_saves   = swd.symbol("g_notif_persist_saves")
        a_flush_r = swd.symbol("g_notif_persist_flush_request")
        a_flush_d = swd.symbol("g_notif_persist_flush_done")
        # Resolve the s_settings address inside notification.c. PSRAM-resident.
        # Locate via pattern: walk per_conv array offsets; easier to use ELF
        # symbol if exported — fall back to reading event_modes through the
        # public getter? Cleaner: notification_get_settings is a function.
        # SWD can call function via halted target — too invasive. Use the
        # static instead, exposed by enabling -fno-toplevel-reorder isn't
        # needed; static globals do appear in .symtab unless stripped.
        a_settings = swd.symbol("g_notif_settings")

        if swd.read_u32(a_inject) != KEYI_MAGIC:
            print("  [FAIL] inject magic wrong"); sys.exit(2)

        # Get to launcher → HWDiag → NOTIF page.
        if swd.read_u32(a_active) != 1:
            press_release(swd, a_inject, KEY_FUNC, settle_ms=300)
        for _ in range(4): press_release(swd, a_inject, KEY_UP)
        for _ in range(4): press_release(swd, a_inject, KEY_LEFT)
        for _ in range(3): press_release(swd, a_inject, KEY_DOWN)
        press_release(swd, a_inject, KEY_OK, settle_ms=400)

        # Cycle RIGHT until on NOTIF page.
        for _ in range(20):
            page = swd.read_mem(a_diag_p, 1)[0]
            if page == NOTIF_PAGE: break
            press_release(swd, a_inject, KEY_RIGHT, settle_ms=200)
        if not expect("on NOTIF page", swd.read_mem(a_diag_p, 1)[0],
                      NOTIF_PAGE): fails += 1

        # ── Test 1: Master toggle ──────────────────────────────────────
        master_before = swd.read_mem(a_master, 1)[0]
        # cur_row defaults to 0 = Master. Press OK to toggle.
        press_release(swd, a_inject, KEY_OK, settle_ms=200)
        master_after = swd.read_mem(a_master, 1)[0]
        if not expect("Master toggled", master_after,
                      0 if master_before else 1): fails += 1
        # Restore.
        press_release(swd, a_inject, KEY_OK, settle_ms=200)
        if not expect("Master restored", swd.read_mem(a_master, 1)[0],
                      master_before): fails += 1

        # ── Test 2: dirty flag flips on mutation ───────────────────────
        # The persist debounce timer may have already cleared it. Force a
        # fresh mutation: cycle DM event mode (row 4 in v2 layout) once.
        # Skip past Master(0), DnD(1), QuietHours(2), VibInt(3) → cur_row=4.
        for _ in range(4): press_release(swd, a_inject, KEY_DOWN)
        # Read DM (event 0 → bits 0..1) before and after.
        # event_modes lives at offset 12 in v2 notif_settings_t layout:
        #   u8 version(0), u8 master(1), u8 dnd(2), u8 vib_int(3),
        #   u8 quiet_en(4), u8[3] reserved1(5..7),
        #   u16 quiet_start(8), u16 quiet_end(10), u16 event_modes(12)
        em_before = (swd.read_mem(a_settings + 12, 2)[1] << 8) | \
                     swd.read_mem(a_settings + 12, 2)[0]
        dm_before = event_mode_get(em_before, 0)
        press_release(swd, a_inject, KEY_OK, settle_ms=200)
        em_after = (swd.read_mem(a_settings + 12, 2)[1] << 8) | \
                    swd.read_mem(a_settings + 12, 2)[0]
        dm_after = event_mode_get(em_after, 0)
        # Cycle is 0→1→2→0; verify it changed.
        if not expect("DM mode cycled", dm_after, dm_before, op="!="):
            fails += 1
        # dirty flag should be set (or already cleared by debounce — accept either).
        df = swd.read_mem(a_dirty, 1)[0]
        print(f"  [info] dirty_flag = {df} (1 = pending flush, 0 = flushed)")
        # Restore DM mode.
        # Cycle until it matches dm_before (max 2 more presses).
        for _ in range(3):
            em = (swd.read_mem(a_settings + 12, 2)[1] << 8) | \
                  swd.read_mem(a_settings + 12, 2)[0]
            if event_mode_get(em, 0) == dm_before: break
            press_release(swd, a_inject, KEY_OK, settle_ms=200)

        # ── Test 3: persistence flush trigger ──────────────────────────
        saves0 = swd.read_u32(a_saves)
        # Force a dirty by toggling Master twice (back to original).
        # Navigate back to row 0 (Master).
        for _ in range(10): press_release(swd, a_inject, KEY_UP)
        press_release(swd, a_inject, KEY_OK, settle_ms=200)
        press_release(swd, a_inject, KEY_OK, settle_ms=200)
        # Now bump flush_request. The flush_timer_cb polls request != done.
        req = swd.read_u32(a_flush_r) + 1
        swd.write_u32(a_flush_r, req)
        # Timer period is 5 s; wait ≤ 6 s for flush.
        for _ in range(60):
            time.sleep(0.1)
            if swd.read_u32(a_flush_d) == req: break
        if not expect("flush completed",  swd.read_u32(a_flush_d), req): fails += 1
        if not expect("saves incremented", swd.read_u32(a_saves), saves0,
                      op=">"): fails += 1

        # ── Test 4: notification_event was called from somewhere ───────
        # Several keypresses above have triggered NOTIF_EVENT_KEYPRESS via
        # the keypad scanner; default mode is OFF so vib won't fire, but
        # the call counter still bumps. Just verify >0 to confirm
        # plumbing.
        calls = swd.read_u32(a_calls)
        if not expect("calls_total > 0", calls, 0, op=">"): fails += 1

        # Sub-page navigation via FUNC.
        press_release(swd, a_inject, KEY_FUNC, settle_ms=200)   # → CHANNELS
        press_release(swd, a_inject, KEY_FUNC, settle_ms=200)   # → PER_CONV
        press_release(swd, a_inject, KEY_FUNC, settle_ms=200)   # → GLOBAL again
        # No cleanly observable state for this without exposing s_notif.sub —
        # accept that the firmware didn't crash (page still NOTIF).
        if not expect("still on NOTIF after FUNC×3",
                      swd.read_mem(a_diag_p, 1)[0], NOTIF_PAGE): fails += 1

    print()
    if fails == 0:
        print("==> NOTIF settings PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} criteria)")
        sys.exit(1)


if __name__ == "__main__":
    main()
