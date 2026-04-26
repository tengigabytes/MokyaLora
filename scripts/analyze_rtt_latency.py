#!/usr/bin/env python3
"""Parse mokya_trace CSV log and report per-keystroke latency breakdown.

The trace points (instrumented in keypad_scan.c, ime_task.cpp, ime_view.c,
lvgl_glue.c) emit one CSV line per event:

    <us_timestamp>,<source>,<event>[,<key=val>...]

Causality model — IMPORTANT
---------------------------
ime_task runs at FreeRTOS priority +3 vs keypad_scan at +2. When keypad_scan
calls key_event_push_hw(), ime_task preempts BEFORE keypad_scan reaches its
TRACE("kpad","commit") line. So the kpad,commit timestamp is observed AFTER
ime,key_pop in wall-clock terms — even though logically the queue push
happens first. We therefore anchor each keystroke on ime,key_pop (key-down)
and walk forward through subsequent events.

Pipeline measured (per key-down event):
    t0 = ime,key_pop          (queue popped, ImeLogic about to run)
    t1 = ime,done             (ImeLogic processed, dirty_counter bumped)
    t2 = lvgl,render_start    (next time LVGL noticed dirty + started rebuild)
    t3 = lvgl,render_end      (paired render_end after t2)
    t4 = lvgl,flush_start     (first flush rect after t3)
    t5 = lvgl,flush_done      (LAST flush_done in the burst — LVGL emits
                               multiple dirty rects per refresh)

Burst-end heuristic: a flush burst ends when there is > 50 ms gap between
flush_done and the next event of any kind, OR when the next ime,key_pop
arrives.
"""

import sys


def parse(path):
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(",")
            if len(parts) < 3:
                continue
            try:
                ts = int(parts[0])
            except ValueError:
                continue
            src, ev = parts[1], parts[2]
            fields = {}
            for kv in parts[3:]:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    fields[k] = v
            yield ts, src, ev, fields


def find_after(events, start_idx, src, name, max_gap_us=None, max_idx=None):
    """Find the next event with (src, name) starting from start_idx+1.
    Returns (idx, ts, fields) or (None, None, None) if not found / past gap.
    max_gap_us — max wall-clock distance from events[start_idx] to candidate.
    max_idx    — don't search past this index.
    """
    if start_idx is None:
        return None, None, None
    base_ts = events[start_idx][0]
    end = max_idx if max_idx is not None else len(events)
    for i in range(start_idx + 1, end):
        ts, s, n, f = events[i]
        if max_gap_us is not None and ts - base_ts > max_gap_us:
            return None, None, None
        if s == src and n == name:
            return i, ts, f
    return None, None, None


def analyze(path):
    events = list(parse(path))
    print(f"Parsed {len(events)} events from {path}")
    print()

    # Locate every key-down ime_pop as anchor.
    keystroke_anchors = [
        i for i, (_, s, n, f) in enumerate(events)
        if s == "ime" and n == "key_pop" and f.get("p") == "1"
    ]

    rows = []
    for ki, anchor_idx in enumerate(keystroke_anchors):
        # Bound this keystroke's window: stop at next key-down anchor.
        next_anchor = (keystroke_anchors[ki + 1]
                       if ki + 1 < len(keystroke_anchors) else len(events))

        t0 = events[anchor_idx][0]
        kc = events[anchor_idx][3].get("kc", "?")

        # ime,done immediately after the pop (within 500 ms).
        i1, t1, _ = find_after(events, anchor_idx, "ime", "done",
                               max_gap_us=500_000, max_idx=next_anchor)
        if t1 is None:
            continue

        # First lvgl,render_start after ime,done (cap 500 ms — anything
        # longer is a refresh that was likely triggered by a different key).
        i2, t2, _ = find_after(events, i1, "lvgl", "render_start",
                               max_gap_us=500_000, max_idx=next_anchor)
        if t2 is None:
            continue

        # Paired render_end immediately following.
        i3, t3, f3 = find_after(events, i2, "lvgl", "render_end",
                                max_gap_us=2_000_000, max_idx=next_anchor)
        if t3 is None:
            continue

        # First flush_start after render_end (cap 200 ms).
        i4, t4, _ = find_after(events, i3, "lvgl", "flush_start",
                               max_gap_us=200_000, max_idx=next_anchor)
        if t4 is None:
            continue

        # Last flush_done in the burst — keep extending while consecutive
        # flush_start/flush_done pairs come within 50 ms of each other.
        last_flush_done = None
        cursor = i4
        while True:
            i_fd, t_fd, _ = find_after(events, cursor, "lvgl", "flush_done",
                                       max_gap_us=200_000,
                                       max_idx=next_anchor)
            if t_fd is None:
                break
            last_flush_done = t_fd
            i_fs_next, t_fs_next, _ = find_after(events, i_fd,
                                                 "lvgl", "flush_start",
                                                 max_gap_us=50_000,
                                                 max_idx=next_anchor)
            if t_fs_next is None:
                break
            cursor = i_fs_next
        if last_flush_done is None:
            continue
        t5 = last_flush_done

        rows.append({
            "i":                  ki,
            "kc":                 kc,
            "ime_proc_ms":       (t1 - t0) / 1000.0,
            "lvgl_wakeup_ms":    (t2 - t1) / 1000.0,
            "render_ms":         (t3 - t2) / 1000.0,
            "render_to_flush_ms":(t4 - t3) / 1000.0,
            "flush_ms":          (t5 - t4) / 1000.0,
            "total_ms":          (t5 - t0) / 1000.0,
            "n_cand":            f3.get("n_cand", "?"),
        })

    if not rows:
        print("No complete keystroke pipelines parsed.")
        return

    print(f"Pipelines reconstructed: {len(rows)} / {len(keystroke_anchors)}")
    print()
    hdr = (f"{'#':>3} {'kc':>5} | {'ime_proc':>9} {'lvgl_wake':>10} "
           f"{'render':>8} {'r->f':>7} {'flush':>8} | {'TOTAL':>9}  n_cand")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['i']:>3} {r['kc']:>5} | "
              f"{r['ime_proc_ms']:>7.2f}ms "
              f"{r['lvgl_wakeup_ms']:>8.2f}ms "
              f"{r['render_ms']:>6.2f}ms "
              f"{r['render_to_flush_ms']:>5.2f}ms "
              f"{r['flush_ms']:>6.2f}ms | "
              f"{r['total_ms']:>7.2f}ms  {r['n_cand']}")

    print()
    print(f"Per-segment statistics across {len(rows)} keystrokes (ms):")
    print(f"{'':18} {'min':>8} {'p50':>8} {'p90':>8} {'max':>8}")

    def stats(key):
        v = sorted(r[key] for r in rows)
        n = len(v)
        return v[0], v[n // 2], v[min(int(n * 0.9), n - 1)], v[-1]

    for label, key in [
        ("ime_proc",        "ime_proc_ms"),
        ("lvgl_wakeup",     "lvgl_wakeup_ms"),
        ("render",          "render_ms"),
        ("render_to_flush", "render_to_flush_ms"),
        ("flush",           "flush_ms"),
        ("TOTAL",           "total_ms"),
    ]:
        lo, p50, p90, hi = stats(key)
        print(f"{label:18} {lo:>8.2f} {p50:>8.2f} {p90:>8.2f} {hi:>8.2f}")


if __name__ == "__main__":
    analyze(sys.argv[1] if len(sys.argv) > 1 else "/tmp/rtt.log")
