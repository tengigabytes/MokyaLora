#!/usr/bin/env python3
"""measure_view_pool.py — drive view_router through every view via
SWD-injected FUNC presses and report the LVGL pool peak / free at
each step (parsed from the firmware's RTT lvgl_mem,stats trace).

Output: per-step active view, total/max/free, plus deltas.
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

VIEW_NAMES = {
    0: "keypad", 1: "rf_debug", 2: "font_test", 3: "ime",
    4: "messages", 5: "nodes", 6: "settings",
}
RTT_LOGGER = r"C:\Program Files\SEGGER\JLink_V932\JLinkRTTLogger.exe"
RING_EVENTS = 32
EVENT_SIZE  = 2
KC_FUNC     = 0x06   # MOKYA_KEY_FUNC, see firmware/mie/include/mie/keycode.h


def parse_lvgl_stats(log_path):
    pat = re.compile(
        r"^(\d+),lvgl_mem,stats,total=(\d+),free=(\d+),max_used=(\d+),"
    )
    with open(log_path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = pat.match(line)
            if m:
                yield (int(m.group(1)), int(m.group(2)),
                       int(m.group(3)), int(m.group(4)))


class Driver:
    def __init__(self, swd):
        self.swd = swd
        self.a_active = swd.symbol("s_view_router_active")
        base = swd.symbol("g_key_inject_buf")
        self.a_magic    = base
        self.a_producer = base + 4
        self.a_consumer = base + 8
        self.a_events   = base + 20

    def active(self):
        return self.swd.read_u32(self.a_active)

    def queue_events(self, event_list):
        chunk_max = RING_EVENTS - 2
        idx = 0
        n = len(event_list)
        while idx < n:
            producer = self.swd.read_u32(self.a_producer)
            consumer = self.swd.read_u32(self.a_consumer)
            while producer - consumer >= chunk_max:
                time.sleep(0.005)
                consumer = self.swd.read_u32(self.a_consumer)
            free = chunk_max - (producer - consumer)
            take = min(free, n - idx)
            writes = []
            for (kb, fb) in event_list[idx:idx + take]:
                slot = producer % RING_EVENTS
                writes.append((self.a_events + slot * EVENT_SIZE + 0, kb))
                writes.append((self.a_events + slot * EVENT_SIZE + 1, fb))
                producer += 1
            self.swd.write_u8_many(writes)
            self.swd.write_u32(self.a_producer, producer)
            idx += take
        deadline = time.time() + 2.0
        target_producer = self.swd.read_u32(self.a_producer)
        while time.time() < deadline:
            if self.swd.read_u32(self.a_consumer) >= target_producer:
                return
            time.sleep(0.005)
        raise RuntimeError("inject timeout")

    def press_func(self):
        self.queue_events([
            (0x80 | (KC_FUNC & 0x7F), 0x00),
            (0x00 | (KC_FUNC & 0x7F), 0x00),
        ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=2)
    ap.add_argument("--gap-s", type=float, default=6.0,
                    help="seconds between FUNC presses (≥5 for lvgl TRACE)")
    ap.add_argument("--label", default="run")
    args = ap.parse_args()

    log_path = Path(f"/tmp/view_pool_{args.label}.log")
    log_path.parent.mkdir(exist_ok=True)
    log_path.unlink(missing_ok=True)
    win_log = Path(rf"C:\Users\user\AppData\Local\Temp\view_pool_{args.label}.log")
    win_log.unlink(missing_ok=True)

    rtt = subprocess.Popen(
        [RTT_LOGGER, "-Device", "RP2350_M33_1", "-If", "SWD", "-Speed", "4000",
         "-RTTSearchRanges", "0x20000000 0x80000",
         "-RTTChannel", "0", str(log_path)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    swd = MokyaSwd()
    swd.open()
    drv = Driver(swd)

    # Detect VIEW_COUNT from runtime cycle: press FUNC and watch the
    # active id wrap. Actually simpler: assume MOKYA_DEBUG_VIEWS=1 → 7.
    view_count = 7

    print(f"[{args.label}] starting; gap_s={args.gap_s}")
    time.sleep(args.gap_s)
    cur = drv.active()
    log = [("boot", cur)]
    print(f"  boot: active={cur} ({VIEW_NAMES.get(cur,'?')})")

    total_steps = view_count * args.cycles
    for step in range(1, total_steps + 1):
        drv.press_func()
        time.sleep(args.gap_s)
        cur = drv.active()
        log.append((f"step{step}", cur))
        print(f"  step{step:2d}: active={cur} ({VIEW_NAMES.get(cur,'?')})")

    subprocess.run(["taskkill", "/IM", "JLinkRTTLogger.exe", "/F"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    swd.close()

    src = log_path if log_path.stat().st_size > 0 else win_log
    samples = list(parse_lvgl_stats(src))
    print(f"\n[{args.label}] parsed {len(samples)} lvgl_mem samples\n")

    print(f"{'event':>8s} | {'view':>9s} | {'total':>6s} | {'max':>6s} | {'free':>6s} | {'used%':>6s}")
    print("-" * 64)
    prev_max = None
    for i, sample in enumerate(samples):
        ts, total, free, used = sample
        ev_name, view_idx = log[i] if i < len(log) else (f"s{i}", -1)
        view_name = VIEW_NAMES.get(view_idx, "?")
        used_pct = 100 * used // total if total else 0
        delta = (used - prev_max) if prev_max is not None else 0
        prev_max = used
        delta_s = f"+{delta}" if delta > 0 else (f"{delta}" if delta < 0 else "  0")
        print(f"{ev_name:>8s} | {view_name:>9s} | {total:6d} | {used:6d} | {free:6d} | {used_pct:5d}% | Δmax {delta_s}")


if __name__ == "__main__":
    main()
