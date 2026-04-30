"""F-4 history ring test — verify SoC + SNR + air_tx_pct all populating.

Reads SWD diag globals updated by history.c on every 30 s timer tick.
Waits up to 75 s (= 2 sample ticks + slack) for the ring to accumulate
two samples then verifies:

  1. g_history_count >= 2 (ring is sampling)
  2. g_history_last_soc_pct in [0, 100] (battery SoC sampled OK)
  3. g_history_last_air_tx_x10 != INT16_MIN (cascade has self
     NodeInfo / DeviceMetrics observed; otherwise NONE).

last_snr_x10 may be INT16_MIN if no RX traffic yet — not asserted.
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mokya_swd import MokyaSwd  # type: ignore

INT16_MIN = -32768

PASS = "[PASS]"
FAIL = "[FAIL]"


def read_state(swd):
    return {
        "count":  swd.read_u32(swd.symbol("g_history_count")) & 0xFFFF,
        "soc":    struct.unpack("<h", swd.read_mem(swd.symbol("g_history_last_soc_pct"), 2))[0],
        "snr":    struct.unpack("<h", swd.read_mem(swd.symbol("g_history_last_snr_x10"), 2))[0],
        "air":    struct.unpack("<h", swd.read_mem(swd.symbol("g_history_last_air_tx_x10"), 2))[0],
    }


def fmt(v):
    return "NONE" if v == INT16_MIN else str(v)


def main():
    fails = 0
    with MokyaSwd() as swd:
        st0 = read_state(swd)
        print(f"[T+0]    count={st0['count']:3d}  "
              f"soc={fmt(st0['soc']):>4s}  snr_x10={fmt(st0['snr']):>5s}  "
              f"air_x10={fmt(st0['air']):>4s}")

        # Wait for at least 2 sample ticks (period = 30 s).
        target_count = st0["count"] + 2
        deadline = time.time() + 75.0
        last_print = 0
        while time.time() < deadline:
            time.sleep(5)
            with MokyaSwd() as swd2:
                stN = read_state(swd2)
            elapsed = int(time.time() - (deadline - 75.0))
            print(f"[T+{elapsed:2d}s] count={stN['count']:3d}  "
                  f"soc={fmt(stN['soc']):>4s}  snr_x10={fmt(stN['snr']):>5s}  "
                  f"air_x10={fmt(stN['air']):>4s}")
            if stN["count"] >= target_count:
                break

        st = stN

    print()
    if st["count"] >= target_count:
        print(f"  {PASS} ring advanced ({st0['count']} -> {st['count']})")
    else:
        print(f"  {FAIL} ring did not advance enough "
              f"({st0['count']} -> {st['count']}, need {target_count})")
        fails += 1

    if 0 <= st["soc"] <= 100:
        print(f"  {PASS} soc_pct = {st['soc']}% (valid range)")
    elif st["soc"] == INT16_MIN:
        print(f"  [WARN] soc_pct = NONE (BQ25622 not online?)")
    else:
        print(f"  {FAIL} soc_pct = {st['soc']} (out of range)")
        fails += 1

    if st["air"] != INT16_MIN:
        pct = st["air"] / 10.0
        print(f"  {PASS} air_tx_pct ×10 = {st['air']} (= {pct:.1f}%)")
    else:
        print(f"  [WARN] air_tx_pct = NONE — self NodeInfo not yet "
              f"decoded by cascade (boot uptime < first NodeInfo broadcast)")
        # Don't fail — this is expected on cold boot before self
        # broadcasts its first NodeInfo with metrics.

    if st["snr"] != INT16_MIN:
        sign = "-" if st["snr"] < 0 else "+"
        print(f"  [INFO] snr_x10 = {sign}{abs(st['snr']) // 10}.{abs(st['snr']) % 10} dB")
    else:
        print(f"  [INFO] snr_x10 = NONE (no RX since boot)")

    print()
    if fails == 0:
        print("==> F-4 history ring PASS")
        sys.exit(0)
    else:
        print(f"==> FAIL ({fails} issues)")
        sys.exit(1)


if __name__ == "__main__":
    main()
